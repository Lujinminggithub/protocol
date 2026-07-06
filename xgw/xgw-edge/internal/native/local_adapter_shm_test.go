package native

import (
	"encoding/binary"
	"testing"
	"time"
)

// newTestAdapter builds a sharded shmDirectAdapter with no real shared-memory
// queue and no background goroutines. Tests drive a shard's logic by calling
// handleOp / dispatch directly on the test goroutine, which is the SOLE accessor
// of the shard's private conn table in this setup — mirroring the worker's
// exclusive ownership without spawning it.
func newTestAdapter(shards int) *shmDirectAdapter {
	a := &shmDirectAdapter{
		closed: make(chan struct{}),
		done:   make(chan struct{}),
	}
	p := 1
	for p < shards {
		p <<= 1
	}
	a.shardMask = uint32(p - 1)
	a.shards = make([]*shmShard, p)
	for i := range a.shards {
		a.shards[i] = &shmShard{
			conns: make(map[uint32]*shmQueueConn),
			ch:    make(chan shardOp, shmWorkerQueueDepth),
		}
	}
	return a
}

// registerForTest registers a conn the way open()+worker would, synchronously on
// the test goroutine.
func (a *shmDirectAdapter) registerForTest(conn *shmQueueConn) {
	s := a.shardFor(conn.id)
	a.connCount.Add(1)
	a.handleOp(s, shardOp{kind: shardOpRegister, id: conn.id, conn: conn})
}

func newTestConn(id uint32, proto byte, readDepth int) *shmQueueConn {
	return &shmQueueConn{
		id:       id,
		proto:    proto,
		target:   "test",
		readCh:   make(chan *pooledRingFrame, readDepth),
		statusCh: make(chan error, 1),
		done:     make(chan struct{}),
	}
}

func makeFrame(kind byte, id uint32, body []byte) *pooledRingFrame {
	f := acquireRingFrame()
	f.data = f.storage[:0]
	f.data = append(f.data, kind)
	var idb [4]byte
	binary.BigEndian.PutUint32(idb[:], id)
	f.data = append(f.data, idb[:]...)
	f.data = append(f.data, body...)
	return f
}

// dispatchFrame routes a frame through the shard the way the worker does:
// id parsed once, then dispatch on the private table.
func (a *shmDirectAdapter) dispatchFrame(frame *pooledRingFrame) {
	id := binary.BigEndian.Uint32(frame.data[1:5])
	a.dispatch(a.shardFor(id), id, frame)
}

// TestDeliverFrameNonBlocking verifies deliverFrame never blocks: a full readCh
// yields deliverBackpre rather than parking the caller.
func TestDeliverFrameNonBlocking(t *testing.T) {
	conn := newTestConn(1, bridgeProtoTCP, 1)
	if res := deliverFrame(conn, makeFrame(bridgeRingKindTCPData, 1, []byte("a"))); res != deliverOK {
		t.Fatalf("first deliver want deliverOK, got %v", res)
	}
	done := make(chan deliverResult, 1)
	go func() {
		done <- deliverFrame(conn, makeFrame(bridgeRingKindTCPData, 1, []byte("b")))
	}()
	select {
	case res := <-done:
		if res != deliverBackpre {
			t.Fatalf("full readCh want deliverBackpre, got %v", res)
		}
	case <-time.After(time.Second):
		t.Fatal("deliverFrame blocked on a full readCh (head-of-line risk)")
	}

	conn.transition(connClosed)
	if res := deliverFrame(conn, makeFrame(bridgeRingKindTCPData, 1, nil)); res != deliverClosed {
		t.Fatalf("closed conn want deliverClosed, got %v", res)
	}
}

// TestRegisterThenDataIsOrdered verifies a register op followed by data ops on
// the same shard channel is processed in FIFO order: the data lands on the conn
// that the register created (no "frame before conn exists" drop).
func TestRegisterThenDataIsOrdered(t *testing.T) {
	a := newTestAdapter(2)
	conn := newTestConn(11, bridgeProtoTCP, 4)
	s := a.shardFor(11)
	// Enqueue register BEFORE data, then drain in order as the worker would.
	a.connCount.Add(1)
	s.ch <- shardOp{kind: shardOpRegister, id: 11, conn: conn}
	s.ch <- shardOp{kind: shardOpFrame, id: 11, frame: makeFrame(bridgeRingKindTCPData, 11, []byte("hi"))}
	for i := 0; i < 2; i++ {
		a.handleOp(s, <-s.ch)
	}
	select {
	case f := <-conn.readCh:
		if string(f.data) != "hi" {
			t.Fatalf("want 'hi', got %q", f.data)
		}
		releaseRingFrame(f)
	default:
		t.Fatal("data frame did not reach the conn registered just before it")
	}
}

// TestDispatchTCPBackpressureGrace verifies a TCP conn survives a transient
// burst (grace window) but is torn down after sustained backpressure.
func TestDispatchTCPBackpressureGrace(t *testing.T) {
	a := newTestAdapter(2)
	conn := newTestConn(7, bridgeProtoTCP, 1)
	a.registerForTest(conn)
	s := a.shardFor(7)

	a.dispatch(s, 7, makeFrame(bridgeRingKindTCPData, 7, []byte("x"))) // fills slot
	for i := 0; i < shmTCPBackpressureGrace-1; i++ {
		a.dispatch(s, 7, makeFrame(bridgeRingKindTCPData, 7, []byte("y")))
	}
	if _, present := s.conns[7]; !present {
		t.Fatalf("TCP conn closed before grace window exhausted (streak=%d)", conn.backpressure)
	}

	a.dispatch(s, 7, makeFrame(bridgeRingKindTCPData, 7, []byte("z"))) // crosses threshold
	if _, present := s.conns[7]; present {
		t.Fatal("TCP conn should be torn down after sustained backpressure")
	}
	if conn.currentState() != connClosed {
		t.Fatalf("TCP conn want connClosed, got %v", conn.currentState())
	}
}

// TestDispatchTCPBackpressureResets verifies a successful delivery resets the
// grace counter, so intermittent (recoverable) slowness never accumulates to a
// teardown.
func TestDispatchTCPBackpressureResets(t *testing.T) {
	a := newTestAdapter(2)
	conn := newTestConn(8, bridgeProtoTCP, 1)
	a.registerForTest(conn)
	s := a.shardFor(8)

	for i := 0; i < shmTCPBackpressureGrace*3; i++ {
		a.dispatch(s, 8, makeFrame(bridgeRingKindTCPData, 8, []byte("x")))
		select {
		case f := <-conn.readCh:
			releaseRingFrame(f)
		default:
		}
	}
	if _, present := s.conns[8]; !present {
		t.Fatal("TCP conn wrongly closed despite recovering between bursts")
	}
}

// TestDispatchUDPBackpressureDropsFrame verifies a slow UDP downstream drops the
// datagram but keeps the conn alive (UDP tolerates loss).
func TestDispatchUDPBackpressureDropsFrame(t *testing.T) {
	a := newTestAdapter(2)
	conn := newTestConn(9, bridgeProtoUDP, 1)
	a.registerForTest(conn)
	s := a.shardFor(9)

	a.dispatch(s, 9, makeFrame(bridgeRingKindUDPData, 9, []byte("x"))) // fills slot
	for i := 0; i < shmTCPBackpressureGrace*2; i++ {
		a.dispatch(s, 9, makeFrame(bridgeRingKindUDPData, 9, []byte("y"))) // dropped
	}
	if _, present := s.conns[9]; !present {
		t.Fatal("UDP conn must stay alive under backpressure (datagram dropped, not conn closed)")
	}
	if conn.currentState() != connOpen {
		t.Fatalf("UDP conn want connOpen, got %v", conn.currentState())
	}
}

// TestShardForIsStableAndInBounds verifies a connection id always maps to the
// same shard (ordering invariant) and never indexes out of range.
func TestShardForIsStableAndInBounds(t *testing.T) {
	a := newTestAdapter(8)
	for _, id := range []uint32{1, 2, 3, 100, 12345, 0xFFFFFFFF, 0} {
		s1 := a.shardFor(id)
		if s1 != a.shardFor(id) {
			t.Fatalf("shard for id=%d not stable", id)
		}
		found := false
		for _, s := range a.shards {
			if s == s1 {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("shard for id=%d not in adapter shards", id)
		}
	}
}

// TestShardDistribution sanity-checks the multiplicative hash spreads sequential
// ids across shards rather than collapsing onto one.
func TestShardDistribution(t *testing.T) {
	a := newTestAdapter(8)
	counts := make([]int, len(a.shards))
	for id := uint32(1); id <= 8000; id++ {
		counts[(id*shmHashMul)&a.shardMask]++
	}
	for i, c := range counts {
		if c == 0 {
			t.Fatalf("shard %d got no connections; hash distribution collapsed", i)
		}
	}
}

// TestDispatchPreservesOrder feeds an ordered TCP stream for one connection and
// verifies the consumer sees frames in order (the property the single-worker-
// per-shard model guarantees).
func TestDispatchPreservesOrder(t *testing.T) {
	a := newTestAdapter(4)
	const id = 42
	const n = 256
	conn := newTestConn(id, bridgeProtoTCP, n+1)
	a.registerForTest(conn)

	for i := 0; i < n; i++ {
		a.dispatchFrame(makeFrame(bridgeRingKindTCPData, id, []byte{byte(i)}))
	}
	for i := 0; i < n; i++ {
		select {
		case f := <-conn.readCh:
			if len(f.data) != 1 || f.data[0] != byte(i) {
				t.Fatalf("frame %d out of order: got %v", i, f.data)
			}
			releaseRingFrame(f)
		default:
			t.Fatalf("missing frame %d", i)
		}
	}
}

// TestRouteFrameDoesNotBlockOnFullShard verifies the readLoop path (routeFrame)
// never blocks when a shard queue is full: a hot shard must not stall the ring
// drain. A full shard applies per-protocol backpressure instead.
func TestRouteFrameDoesNotBlockOnFullShard(t *testing.T) {
	a := newTestAdapter(2)
	conn := newTestConn(5, bridgeProtoUDP, 1)
	a.registerForTest(conn)
	s := a.shardFor(5)
	for len(s.ch) < cap(s.ch) {
		s.ch <- shardOp{kind: shardOpFrame, id: 5, frame: makeFrame(bridgeRingKindUDPData, 5, []byte("fill"))}
	}

	done := make(chan struct{})
	go func() {
		a.routeFrame(makeFrame(bridgeRingKindUDPData, 5, []byte("overflow")))
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("routeFrame blocked on a full shard queue (ring-drain head-of-line)")
	}

	for len(s.ch) > 0 {
		op := <-s.ch
		releaseRingFrame(op.frame)
	}
}

// TestCloseTearsDownLiveConns verifies Close drains in-flight ops and closes
// surviving conns (no leak), exercising the real worker + teardown path.
func TestCloseTearsDownLiveConns(t *testing.T) {
	a := &shmDirectAdapter{
		closed: make(chan struct{}),
		done:   make(chan struct{}),
	}
	a.shardMask = 1 // 2 shards
	a.shards = []*shmShard{
		{conns: make(map[uint32]*shmQueueConn), ch: make(chan shardOp, shmWorkerQueueDepth)},
		{conns: make(map[uint32]*shmQueueConn), ch: make(chan shardOp, shmWorkerQueueDepth)},
	}
	for _, s := range a.shards {
		a.activeWG.Add(1)
		go a.dispatchWorker(s)
	}
	// Register a couple of conns via the real command path.
	conns := []*shmQueueConn{newTestConn(101, bridgeProtoTCP, 1), newTestConn(202, bridgeProtoUDP, 1)}
	for _, c := range conns {
		s := a.shardFor(c.id)
		a.connCount.Add(1)
		if !a.postOp(s, shardOp{kind: shardOpRegister, id: c.id, conn: c}) {
			t.Fatal("register postOp failed before close")
		}
	}
	// Let the workers process the registers.
	time.Sleep(20 * time.Millisecond)

	// Simulate the readLoop having exited so Close can proceed.
	close(a.done)
	close(a.closed)
	a.activeWG.Wait()
	a.postWG.Wait()
	for _, s := range a.shards {
		a.teardownShard(s)
	}

	if a.connCount.Load() != 0 {
		// connCount.Store(0) is done by Close() proper; here we assert teardown
		// already balanced it.
		t.Fatalf("connCount want 0 after teardown, got %d", a.connCount.Load())
	}
	for _, c := range conns {
		if c.currentState() != connClosed {
			t.Fatalf("conn %d want connClosed after teardown, got %v", c.id, c.currentState())
		}
	}
}
