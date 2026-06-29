package native

import (
	"context"
	"encoding/binary"
	"net"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"github.com/local/xgw-edge/internal/coremodel"
)

// shmDirectAdapter is the next-step local adapter implementation path.
// It intentionally reuses the canonical OpenRequest contract while the data
// mover is upgraded from "bridge transport" to a purer shared-memory adapter.
//
// The first usable landing reuses the mmap ring mover under the stricter local
// adapter contract. This keeps the canonical OpenRequest stable while moving
// away from "bridge as a transport semantic" toward "shared memory as a local
// adapter implementation".
//
// Dispatch model: a single readLoop pops frames from the rx ring (the only place
// that drains the ring, so ring order is preserved) and routes each frame to one
// of N shards by hash(id)&mask. A given connection id is always handled by the
// same shard, so per-connection ordering is preserved while different
// connections are dispatched in parallel. Each shard owns its own conn table
// (sharded lock, so dispatch/open/unregister never contend on a single global
// mutex) and its own worker goroutine. Delivery into a connection is bounded and
// non-blocking: a slow downstream never blocks the worker (and therefore never
// blocks other connections) — see deliverFrame. The readLoop also never blocks
// on a full shard queue (see routeFrame), so a hot shard cannot stall the ring
// drain for the other shards.
//
// Shard conn tables are goroutine-private: each shard's conn map is owned and
// mutated solely by that shard's worker goroutine, so it needs no lock. Frames
// AND lifecycle commands (register/unregister) travel on the same shard channel
// (shardOp), giving them a single FIFO order: a register command is always
// processed before any data frame for that connection, and an unregister cannot
// race a concurrent map read. Internal teardown (a worker tearing down a conn it
// is already handling) mutates the private map directly; only out-of-worker
// callers (open's failure paths) post an unregister command.
type shmDirectAdapter struct {
	basePath string
	queue    *shmQueuePair
	nextID   atomic.Uint32
	closed   chan struct{}
	done     chan struct{}

	shards    []*shmShard
	shardMask uint32
	activeWG  sync.WaitGroup // dispatch workers
	postWG    sync.WaitGroup // in-flight postOp senders (lifecycle commands)
	connCount atomic.Int64   // live conns across all shards (lock-free idle check)
}

// shardOpKind tags a shardOp.
type shardOpKind uint8

const (
	shardOpFrame      shardOpKind = iota // a data/status/lifecycle frame to dispatch
	shardOpRegister                      // register conn (carried in op.conn)
	shardOpUnregister                    // unregister op.id (from outside the worker)
)

// shardOp is the unit on a shard channel. Frames and lifecycle commands share
// one queue so they observe a single FIFO order within the shard.
type shardOp struct {
	kind  shardOpKind
	id    uint32 // connection id (frame id pre-parsed by routeFrame; or unregister target)
	frame *pooledRingFrame
	conn  *shmQueueConn // register payload
}

// shmShard is one dispatch partition: a goroutine-private conn table plus the
// channel feeding its worker. No mutex: conns is only ever touched by the worker.
type shmShard struct {
	conns map[uint32]*shmQueueConn // OWNED by the worker goroutine; do not touch elsewhere
	ch    chan shardOp
}

// shmWorkerQueueDepth is the per-shard backlog. The worker never blocks on a
// connection (delivery is non-blocking), so this only needs to absorb short
// bursts between the readLoop's batch pop (≤64) and the worker; 256 is ample.
const shmWorkerQueueDepth = 256

// shmConnReadDepth is the per-conn downstream buffer (readCh). Kept modest: a
// healthy downstream drains it continuously, and a slow one is bounded by the
// TCP backpressure grace window (shmTCPBackpressureGrace) well below this depth,
// so it never needs to be large. A smaller depth bounds memory under many
// concurrent connections (each buffered frame pins a pooled storage buffer).
const shmConnReadDepth = 256

// shmMaxDispatchShards caps the shard fan-out regardless of core count.
const shmMaxDispatchShards = 16

// shmHashMul is a Knuth multiplicative-hash constant used to scatter sequential
// connection ids across shards so same-residue ids do not pile onto one shard.
const shmHashMul = 2654435761

// shmTCPBackpressureGrace is how many consecutive full-buffer deliveries a TCP
// conn may incur before it is torn down. A short grace window lets a transient
// downstream burst recover instead of killing an otherwise healthy connection;
// the counter resets on the first successful delivery.
const shmTCPBackpressureGrace = 32

// shmShardCount picks a power-of-two shard count from the CPU budget so the
// shard index can use a cheap mask instead of a modulo, and so the fan-out
// scales with the machine instead of being a fixed constant.
func shmShardCount() int {
	n := runtime.GOMAXPROCS(0)
	if n < 1 {
		n = 1
	}
	if n > shmMaxDispatchShards {
		n = shmMaxDispatchShards
	}
	// round up to the next power of two
	p := 1
	for p < n {
		p <<= 1
	}
	return p
}

func newShmDirectAdapter(basePath string) (LocalAdapter, error) {
	queue, err := newShmQueuePair(basePath)
	if err != nil {
		return nil, err
	}
	a := &shmDirectAdapter{
		basePath: basePath,
		queue:    queue,
		closed:   make(chan struct{}),
		done:     make(chan struct{}),
	}
	count := shmShardCount()
	a.shardMask = uint32(count - 1)
	a.shards = make([]*shmShard, count)
	for i := range a.shards {
		s := &shmShard{
			conns: make(map[uint32]*shmQueueConn),
			ch:    make(chan shardOp, shmWorkerQueueDepth),
		}
		a.shards[i] = s
		a.activeWG.Add(1)
		go a.dispatchWorker(s)
	}
	go a.readLoop()
	return a, nil
}

// shardFor maps a connection id to its shard via a multiplicative hash + mask.
func (a *shmDirectAdapter) shardFor(id uint32) *shmShard {
	return a.shards[(id*shmHashMul)&a.shardMask]
}

func (a *shmDirectAdapter) OpenTCP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error) {
	if a == nil || a.queue == nil {
		return nil, net.ErrClosed
	}
	return a.open(ctx, bridgeRingKindTCPOpen, bridgeProtoTCP, req)
}

func (a *shmDirectAdapter) OpenUDP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error) {
	if a == nil || a.queue == nil {
		return nil, net.ErrClosed
	}
	return a.open(ctx, bridgeRingKindUDPOpen, bridgeProtoUDP, req)
}

func (a *shmDirectAdapter) Close() error {
	if a == nil || a.queue == nil {
		return nil
	}
	select {
	case <-a.closed:
	default:
		close(a.closed)
	}
	<-a.done
	// readLoop has exited. Signal workers via a.closed (already closed above) and
	// wait for them to exit. s.ch is intentionally NOT closed — open()'s postOp
	// may still be racing, and closing under it would panic; postOp observes
	// a.closed instead. Order: wait for workers to stop, then for any in-flight
	// postOp senders to finish, then drain+teardown each shard as the sole
	// remaining accessor — so no op, frame, or conn leaks across the shutdown.
	a.activeWG.Wait()
	a.postWG.Wait()
	for _, s := range a.shards {
		a.teardownShard(s)
	}
	a.connCount.Store(0)
	return a.queue.Close()
}

func (a *shmDirectAdapter) open(ctx context.Context, kind byte, proto byte, req coremodel.OpenRequest) (net.Conn, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	id := a.nextID.Add(1)
	if id == 0 {
		id = a.nextID.Add(1)
	}
	body, err := buildRingOpenBody(proto, req)
	if err != nil {
		return nil, err
	}
	conn := &shmQueueConn{
		queue:    a.queue,
		id:       id,
		proto:    proto,
		target:   req.Flow.Target,
		readCh:   make(chan *pooledRingFrame, shmConnReadDepth),
		statusCh: make(chan error, 1),
		done:     make(chan struct{}),
	}
	s := a.shardFor(id)
	// connCount tracks live intent and is bumped synchronously here so the
	// readLoop's idle check sees the conn immediately; the worker decrements it
	// when it actually removes the conn from its private table.
	a.connCount.Add(1)
	if !a.postOp(s, shardOp{kind: shardOpRegister, id: id, conn: conn}) {
		// Adapter is shutting down: undo the count and fail the open.
		a.connCount.Add(-1)
		return nil, net.ErrClosed
	}
	ringTracef("shmdirect.open.begin id=%d kind=%d target=%s bytes=%d\n", id, kind, req.Flow.Target, len(body)+5)
	if err := a.queue.PushEvent(ctx, kind, id, body); err != nil {
		ringTracef("shmdirect.open.write_fail id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, err)
		a.unregister(id)
		return nil, err
	}
	ringTracef("shmdirect.open.write_ok id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
	timer := time.NewTimer(defaultDialTimeout)
	defer timer.Stop()
	select {
	case err := <-conn.statusCh:
		ringTracef("shmdirect.open.status id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, err)
		if err != nil {
			a.unregister(id)
			return nil, err
		}
		return conn, nil
	case <-ctx.Done():
		ringTracef("shmdirect.open.ctx_done id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, ctx.Err())
		// Open frame already reached the peer; tell it to drop any upstream socket
		// it may have established before we abandon the conn.
		a.abortConn(id)
		return nil, ctx.Err()
	case <-timer.C:
		ringTracef("shmdirect.open.timeout id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
		// 恢复路径对齐稳态策略：优先 in-place reattach（refresh 内部仅在结构损坏
		// 时才回退 reopen），而不是无条件 reopen 重建 ring。refresh 成功后重写
		// open 帧并再等一次 status。
		if healErr := a.queue.refresh(); healErr == nil {
			retryTimer := time.NewTimer(defaultDialTimeout / 2)
			defer retryTimer.Stop()
			if err := a.queue.PushEvent(ctx, kind, id, body); err == nil {
				ringTracef("shmdirect.open.heal_rewrite_ok id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
				select {
				case err := <-conn.statusCh:
					ringTracef("shmdirect.open.heal_status id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, err)
					if err == nil {
						return conn, nil
					}
				case <-retryTimer.C:
					ringTracef("shmdirect.open.heal_timeout id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
				case <-ctx.Done():
					ringTracef("shmdirect.open.heal_ctx_done id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, ctx.Err())
				case <-a.closed:
				}
			} else {
				ringTracef("shmdirect.open.heal_rewrite_fail id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, err)
			}
		} else {
			ringTracef("shmdirect.open.heal_refresh_fail id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, healErr)
		}
		// Giving up after the open frame reached the peer: tell it to tear down
		// any upstream socket it established rather than leak it.
		a.abortConn(id)
		return nil, context.DeadlineExceeded
	case <-a.closed:
		ringTracef("shmdirect.open.closed id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
		a.unregister(id)
		return nil, net.ErrClosed
	}
}

func (a *shmDirectAdapter) readLoop() {
	defer close(a.done)
	var batch [bridgeRingBatchLimit]*pooledRingFrame
	idleBackoff := 250 * time.Microsecond
	for {
		select {
		case <-a.closed:
			return
		default:
		}
		n, err := a.queue.PopEvents(batch[:])
		maxIdleBackoff := 5 * time.Millisecond
		if !a.hasActiveConns() {
			maxIdleBackoff = 250 * time.Millisecond
		}
		if err != nil {
			ringTracef("shmdirect.read.pop_err err=%v\n", err)
			if waitErr := a.queue.WaitForReadable(a.closed, idleBackoff); waitErr != nil {
				ringTracef("shmdirect.read.wait_err err=%v\n", waitErr)
			}
			if idleBackoff < maxIdleBackoff {
				idleBackoff *= 2
				if idleBackoff > maxIdleBackoff {
					idleBackoff = maxIdleBackoff
				}
			}
			continue
		}
		if n == 0 {
			if waitErr := a.queue.WaitForReadable(a.closed, idleBackoff); waitErr != nil {
				ringTracef("shmdirect.read.wait_err err=%v\n", waitErr)
			}
			if idleBackoff < maxIdleBackoff {
				idleBackoff *= 2
				if idleBackoff > maxIdleBackoff {
					idleBackoff = maxIdleBackoff
				}
			}
			continue
		}
		idleBackoff = 250 * time.Microsecond
		for i := 0; i < n; i++ {
			a.routeFrame(batch[i])
			batch[i] = nil
		}
	}
}

// routeFrame sends a freshly popped frame to the shard that owns its connection
// id, preserving per-connection order. The send is NON-BLOCKING with respect to
// the readLoop: if the target shard queue is full, the readLoop does not block
// (which would stall the ring drain for every other shard). Instead the frame is
// handled under the same per-protocol backpressure policy as a full conn buffer
// (TCP: close the conn; UDP: drop the datagram). Frames too short to carry an id
// are dropped. The id is parsed once here and carried in the op so the worker
// need not re-decode it (B7).
func (a *shmDirectAdapter) routeFrame(frame *pooledRingFrame) {
	if frame == nil {
		return
	}
	if len(frame.data) < 5 {
		releaseRingFrame(frame)
		return
	}
	kind := frame.data[0]
	id := binary.BigEndian.Uint32(frame.data[1:5])
	s := a.shardFor(id)
	op := shardOp{kind: shardOpFrame, id: id, frame: frame}
	select {
	case s.ch <- op:
		return
	case <-a.closed:
		releaseRingFrame(frame)
		return
	default:
		// Shard queue full: do NOT block the ring drain. Apply backpressure to
		// this one connection only.
	}
	// Data frames are subject to drop/close; lifecycle frames (Status/Close/
	// HalfClose) must not be dropped silently, so fall back to a bounded blocking
	// send for those — they are rare and small, and dropping them would corrupt
	// connection state.
	if kind == bridgeRingKindTCPData || kind == bridgeRingKindUDPData {
		bodyLen := len(frame.data) - 5
		releaseRingFrame(frame)
		a.applyDataBackpressure(id, kind, bodyLen)
		return
	}
	select {
	case s.ch <- op:
	case <-a.closed:
		releaseRingFrame(frame)
	}
}

// applyDataBackpressure enacts the per-protocol policy when a data frame cannot
// be queued without blocking: UDP datagrams are dropped (loss-tolerant), TCP
// connections are torn down (a stream cannot drop bytes, and a persistently slow
// peer must not stall its shard).
func (a *shmDirectAdapter) applyDataBackpressure(id uint32, kind byte, n int) {
	if kind == bridgeRingKindUDPData {
		ringTracef("shmdirect.route.udp_drop id=%d bytes=%d reason=shard_full\n", id, n)
		return
	}
	ringTracef("shmdirect.route.tcp_close id=%d bytes=%d reason=shard_full\n", id, n)
	a.unregister(id)
}

// dispatchWorker serially processes one shard's op stream: register/unregister
// commands and data frames in a single FIFO order. It is the SOLE accessor of
// the shard's conn table during normal operation, so that table needs no lock.
// It exits promptly when a.closed is signaled; the remaining queued ops and the
// surviving conn table are reclaimed by Close after every worker has exited (at
// which point Close is the only accessor).
func (a *shmDirectAdapter) dispatchWorker(s *shmShard) {
	defer a.activeWG.Done()
	for {
		select {
		case op := <-s.ch:
			a.handleOp(s, op)
		case <-a.closed:
			return
		}
	}
}

// handleOp applies a single shard op on the worker goroutine.
func (a *shmDirectAdapter) handleOp(s *shmShard, op shardOp) {
	switch op.kind {
	case shardOpRegister:
		s.conns[op.id] = op.conn
	case shardOpUnregister:
		a.removeConnLocal(s, op.id)
	default: // shardOpFrame
		a.dispatch(s, op.id, op.frame)
	}
}

// teardownShard reclaims a shard after its worker has exited: it drains any ops
// still queued (releasing pooled frames, accounting late registers) and closes
// every surviving conn. MUST be called only after activeWG.Wait(), so the worker
// is gone and Close is the sole accessor of s.ch / s.conns.
func (a *shmDirectAdapter) teardownShard(s *shmShard) {
	for {
		select {
		case op := <-s.ch:
			switch op.kind {
			case shardOpFrame:
				releaseRingFrame(op.frame)
			case shardOpRegister:
				s.conns[op.id] = op.conn
			case shardOpUnregister:
				a.removeConnLocal(s, op.id)
			}
		default:
			for id := range s.conns {
				conn := s.conns[id]
				delete(s.conns, id)
				a.connCount.Add(-1)
				conn.closeLocal()
			}
			return
		}
	}
}

func (a *shmDirectAdapter) hasActiveConns() bool {
	return a.connCount.Load() > 0
}

// deliverResult is the outcome of a non-blocking frame delivery into a conn.
type deliverResult int

const (
	deliverOK      deliverResult = iota // frame handed to the conn's readCh
	deliverClosed                       // conn already torn down (done closed)
	deliverBackpre                      // readCh full: downstream not keeping up
)

// deliverFrame attempts a NON-BLOCKING handoff of frame to conn.readCh. It never
// blocks the calling dispatch worker on a slow downstream: if readCh is full it
// returns deliverBackpre and leaves the caller to apply the per-protocol policy
// (TCP: close the conn; UDP: drop the datagram). The recover guards the race
// where readCh was closed concurrently by a lifecycle transition.
func deliverFrame(conn *shmQueueConn, frame *pooledRingFrame) (res deliverResult) {
	defer func() {
		if recover() != nil {
			res = deliverClosed
		}
	}()
	select {
	case conn.readCh <- frame:
		return deliverOK
	case <-conn.done:
		return deliverClosed
	default:
		return deliverBackpre
	}
}

func safeDeliverStatus(conn *shmQueueConn, err error) (delivered bool) {
	defer func() {
		if recover() != nil {
			delivered = false
		}
	}()
	select {
	case conn.statusCh <- err:
		return true
	case <-conn.done:
		return false
	default:
		return false
	}
}

// dispatch handles one data/status/lifecycle frame for connection id. Called
// only from the shard worker, so the conn lookup reads the private table without
// a lock. id was pre-parsed by routeFrame (B7: not re-decoded here).
func (a *shmDirectAdapter) dispatch(s *shmShard, id uint32, frame *pooledRingFrame) {
	if len(frame.data) < 5 {
		releaseRingFrame(frame)
		return
	}
	kind := frame.data[0]
	conn := s.conns[id]
	if conn == nil {
		releaseRingFrame(frame)
		return
	}
	body := frame.data[5:]
	switch kind {
	case bridgeRingKindStatus:
		err := decodeRingStatus(body)
		ringTracef("shmdirect.dispatch.status id=%d err=%v bytes=%d\n", id, err, len(body))
		releaseRingFrame(frame)
		if !safeDeliverStatus(conn, err) {
			// open() already stopped waiting (it timed out), so this status is
			// orphaned. The peer believes the conn is live and may hold an upstream
			// socket; push a Close so it tears that down, then drop the local entry.
			ringTracef("shmdirect.dispatch.status_drop id=%d\n", id)
			actx, acancel := context.WithTimeout(context.Background(), defaultDialTimeout/2)
			if perr := a.queue.PushEvent(actx, bridgeRingKindClose, id, nil); perr != nil {
				ringTracef("shmdirect.dispatch.status_drop_close_fail id=%d err=%v\n", id, perr)
			}
			acancel()
			a.removeConnLocal(s, id)
		}
	case bridgeRingKindTCPData, bridgeRingKindUDPData:
		frame.data = body
		switch deliverFrame(conn, frame) {
		case deliverOK:
			conn.backpressure = 0 // handed off; reset the grace counter
		case deliverClosed:
			ringTracef("shmdirect.dispatch.frame_drop id=%d kind=%d bytes=%d reason=closed\n", id, kind, len(body))
			releaseRingFrame(frame)
		case deliverBackpre:
			releaseRingFrame(frame)
			if kind == bridgeRingKindUDPData {
				// UDP tolerates loss: drop this datagram, keep the conn alive.
				ringTracef("shmdirect.dispatch.udp_drop id=%d bytes=%d reason=backpressure\n", id, len(body))
				break
			}
			// TCP cannot drop bytes. Allow a short grace window so a transient
			// burst does not kill an otherwise healthy connection; only tear it
			// down after sustained backpressure (the downstream is truly stuck).
			conn.backpressure++
			if conn.backpressure < shmTCPBackpressureGrace {
				ringTracef("shmdirect.dispatch.tcp_backpressure id=%d bytes=%d streak=%d\n", id, len(body), conn.backpressure)
				break
			}
			ringTracef("shmdirect.dispatch.tcp_close id=%d bytes=%d streak=%d reason=backpressure\n", id, len(body), conn.backpressure)
			a.removeConnLocal(s, id)
		}
	case bridgeRingKindClose:
		ringTracef("shmdirect.dispatch.close id=%d\n", id)
		releaseRingFrame(frame)
		a.removeConnLocal(s, id)
	case bridgeRingKindHalfClose:
		ringTracef("shmdirect.dispatch.half_close id=%d\n", id)
		releaseRingFrame(frame)
		conn.closeReadHalf()
	default:
		releaseRingFrame(frame)
	}
}

// postOp sends a shard op, blocking only until the worker accepts it or the
// adapter shuts down. Returns false if the adapter is closing (op dropped).
// Used for lifecycle commands (register/unregister) which must not be lost; the
// shard channel is sized so these rarely block, and they are low frequency.
//
// A closing adapter is detected up front so a late open() fails cleanly with
// ErrClosed instead of racing the worker exit. Any op that still makes it onto
// the channel during the shutdown window is reclaimed by Close's post-Wait drain,
// so no conn or pooled frame leaks regardless of interleaving.
func (a *shmDirectAdapter) postOp(s *shmShard, op shardOp) bool {
	select {
	case <-a.closed:
		return false
	default:
	}
	// Register as an in-flight sender so Close's teardown drain cannot run
	// concurrently with a send (which would let a late op land after the drain
	// and leak). Close waits on postWG before draining.
	a.postWG.Add(1)
	defer a.postWG.Done()
	// Re-check after registering: if Close fired between the first check and the
	// Add, bail out rather than send into a channel nobody will drain in order.
	select {
	case <-a.closed:
		return false
	default:
	}
	select {
	case s.ch <- op:
		return true
	case <-a.closed:
		return false
	}
}

// unregister tears down a connection from OUTSIDE its worker goroutine (open's
// failure/timeout paths). It posts an unregister command so the actual map
// mutation happens on the owning worker, keeping the conn table lock-free. The
// teardown is idempotent: the worker drops a no-op if the conn is already gone.
func (a *shmDirectAdapter) unregister(id uint32) {
	s := a.shardFor(id)
	a.postOp(s, shardOp{kind: shardOpUnregister, id: id})
}

// abortConn tears down a connection that the Go side is unilaterally giving up
// on AFTER its open frame already reached the C peer (open timeout / ctx
// cancel / a late status that arrived once open had stopped waiting). In those
// cases the C peer may have already established the upstream socket, so a Close
// frame is pushed (best effort, bounded) to make it tear that socket down rather
// than leak it. Then the local table entry is removed. Not used when the open
// frame never reached the peer (no socket to clean up) — plain unregister suffices.
func (a *shmDirectAdapter) abortConn(id uint32) {
	ctx, cancel := context.WithTimeout(context.Background(), defaultDialTimeout/2)
	if err := a.queue.PushEvent(ctx, bridgeRingKindClose, id, nil); err != nil {
		ringTracef("shmdirect.abort.close_push_fail id=%d err=%v\n", id, err)
	}
	cancel()
	a.unregister(id)
}

// removeConnLocal removes a conn from the worker-private table. MUST be called
// only from the shard's worker goroutine. Idempotent: a missing id is a no-op,
// so worker-internal teardown and an external unregister command for the same id
// converge without double-counting or double-closing.
func (a *shmDirectAdapter) removeConnLocal(s *shmShard, id uint32) {
	conn, ok := s.conns[id]
	if !ok {
		return
	}
	delete(s.conns, id)
	a.connCount.Add(-1)
	conn.closeLocal()
}
