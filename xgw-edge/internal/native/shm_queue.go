package native

import (
	"context"
	"errors"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// shmQueueEndpoint is the purer shared-memory queue API under the local adapter
// contract. It deliberately exposes queue/event operations instead of bridge
// semantics so higher layers no longer depend on "bridge transport" details.
type shmQueueEndpoint interface {
	PushEvent(ctx context.Context, kind byte, id uint32, payload []byte) error
	PushStreamData(ctx context.Context, kind byte, id uint32, payload []byte) (int, error)
	PopEvents(batch []*pooledRingFrame) (int, error)
	Close() error
}

type shmQueuePair struct {
	basePath string
	tx *bridgeRingFile
	rx *bridgeRingFile
	mu sync.Mutex
	// retired holds ring files swapped out by reopen whose mmap may still be
	// referenced by a goroutine parked in waitForChange (which runs without
	// q.mu). They are unmapped lazily by reapRetiredLocked once their inWait
	// count drops to zero, so a lock-free park never touches freed memory.
	retired []*bridgeRingFile
	// retiredPending mirrors len(retired) > 0 as a lock-free hint so a park
	// wake-up can cheaply decide whether an opportunistic reap is worth taking
	// q.mu for, without reading the slice under no lock.
	retiredPending atomic.Bool
}

const shmQueueWaitTimeout = 50 * time.Millisecond

// shmUDPReassemblyMax bounds the per-conn UDP reassembly buffer. The buffer only
// holds a partially received datagram plus any datagrams awaiting push; under
// sustained backpressure it must not grow without limit. When exceeded, the
// oldest unparsed bytes are dropped (UDP is loss-tolerant) rather than risking
// OOM. Sized to comfortably hold several max-size datagrams.
const shmUDPReassemblyMax = 1 << 20 // 1 MiB

func newShmQueuePair(basePath string) (*shmQueuePair, error) {
	tx, err := openBridgeRingFile(basePath+".go2c", true, bridgeRingRoleFront, bridgeRingFlagWriterReady, bridgeRingFlagReaderReady, true)
	if err != nil {
		return nil, err
	}
	rx, err := openBridgeRingFile(basePath+".c2go", true, bridgeRingRoleFront, bridgeRingFlagReaderReady, bridgeRingFlagWriterReady, false)
	if err != nil {
		_ = tx.close()
		return nil, err
	}
	return &shmQueuePair{basePath: basePath, tx: tx, rx: rx}, nil
}

func (q *shmQueuePair) refresh() error {
	if q == nil || q.tx == nil || q.rx == nil {
		return errors.New("shm queue pair is not initialized")
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	return q.refreshLocked()
}

// refreshLocked keeps both rings attached to the live epoch via the pure
// generation/attach protocol. Ring file paths are a stable namespace (never
// stat'd for health); generation is the sole epoch identity and attach flags
// declare liveness. Steady state is an in-mmap header read plus a cheap attach
// re-assert — no filesystem syscalls and no per-frame header writes. A peer
// restart bumps the generation, which triggers an in-place reattach (resync
// header, re-publish attach) with the mmap preserved — NOT a reopen. Reopen is
// the structural last resort only when the mapping is corrupt.
func (q *shmQueuePair) refreshLocked() error {
	if q == nil || q.tx == nil || q.rx == nil {
		return errors.New("shm queue pair is not initialized")
	}
	if err := q.tx.refreshLayout(); err != nil {
		return q.reopenLocked("tx_layout_invalid")
	}
	if err := q.rx.refreshLayout(); err != nil {
		return q.reopenLocked("rx_layout_invalid")
	}
	reattached := q.tx.ensureAttached()
	if q.rx.ensureAttached() {
		reattached = true
	}
	if reattached && ringDebug {
		println("shmdirect.queue.reattach base=" + q.basePath)
	}
	return nil
}

func (q *shmQueuePair) reopen(reason string) error {
	q.mu.Lock()
	defer q.mu.Unlock()
	return q.reopenLocked(reason)
}

func (q *shmQueuePair) reopenLocked(reason string) error {
	var (
		newTx *bridgeRingFile
		newRx *bridgeRingFile
		err   error
	)
	if q == nil {
		return errors.New("shm queue pair is nil")
	}
	if q.basePath == "" {
		return errors.New("shm queue base path is empty")
	}
	if ringDebug {
		println("shmdirect.queue.reopen base=" + q.basePath + " reason=" + reason)
	}
	newTx, err = openBridgeRingFile(q.basePath+".go2c", true, bridgeRingRoleFront, bridgeRingFlagWriterReady, bridgeRingFlagReaderReady, true)
	if err != nil {
		return err
	}
	newRx, err = openBridgeRingFile(q.basePath+".c2go", true, bridgeRingRoleFront, bridgeRingFlagReaderReady, bridgeRingFlagWriterReady, false)
	if err != nil {
		_ = newTx.close()
		return err
	}
	oldTx := q.tx
	oldRx := q.rx
	q.tx = newTx
	q.rx = newRx
	// Defer unmapping the swapped-out rings: a goroutine may be parked in
	// waitForChange on oldRx/oldTx without holding q.mu. Retire them and reap
	// once no waiter references them, so the lock-free park never dereferences
	// freed memory.
	if oldTx != nil {
		q.retired = append(q.retired, oldTx)
	}
	if oldRx != nil {
		q.retired = append(q.retired, oldRx)
	}
	q.reapRetiredLocked()
	if ringDebug {
		println("shmdirect.queue.reopen.done base=" + q.basePath + " reason=" + reason)
	}
	return nil
}

// reapRetiredLocked unmaps retired ring files that no goroutine is still parked
// on (inWait == 0). Must be called with q.mu held. Rings with active waiters are
// kept until a later reap (next reopen or Close) observes them idle.
func (q *shmQueuePair) reapRetiredLocked() {
	if len(q.retired) == 0 {
		q.retiredPending.Store(false)
		return
	}
	kept := q.retired[:0]
	for _, r := range q.retired {
		if r == nil {
			continue
		}
		if r.inWait.Load() > 0 {
			kept = append(kept, r)
			continue
		}
		_ = r.close()
	}
	q.retired = kept
	q.retiredPending.Store(len(q.retired) > 0)
}

// maybeReapRetired opportunistically reaps retired rings outside the normal
// reopen path. It is called by a park wake-up so a swapped-out ring whose last
// waiter just left does not have to wait for the next reopen (which may never
// come) to be unmapped. The atomic hint keeps the common case (nothing retired)
// lock-free; only when something is pending does it take q.mu briefly.
func (q *shmQueuePair) maybeReapRetired() {
	if !q.retiredPending.Load() {
		return
	}
	q.mu.Lock()
	q.reapRetiredLocked()
	q.mu.Unlock()
}

func (q *shmQueuePair) PushEvent(ctx context.Context, kind byte, id uint32, payload []byte) error {
	started := time.Now()
	attempts := 0
	// Bounded wait per park: long enough to keep an idle writer off the CPU,
	// short enough to re-check peerReady when the peer has not attached (an
	// unattached peer never bumps notify, so we must not block indefinitely).
	// Under normal operation the futex/event wake returns immediately, so this
	// mainly bounds missed-wake recovery and should stay comfortably above the
	// scheduler tick to avoid turning idle waits into CPU churn.
	const waitTimeout = shmQueueWaitTimeout
	for {
		attempts++
		q.mu.Lock()
		err := q.refreshLocked()
		if err == nil {
			var ok bool
			ok, err = q.tx.pushFrame(kind, id, payload)
			peerReady := q.tx.peerReady()
			writable := q.tx.hasWritable()
			q.mu.Unlock()
			if err != nil {
				ringTracef("shmdirect.push.fail kind=%d id=%d attempts=%d err=%v\n", kind, id, attempts, err)
				return err
			}
			if ok {
				if attempts > 1 {
					ringTracef("shmdirect.push.ok kind=%d id=%d attempts=%d wait_ms=%d\n",
						kind, id, attempts, time.Since(started).Milliseconds())
				}
				return nil
			}
			if !peerReady && time.Since(started) >= 200*time.Millisecond {
				// A missing attach bit is a lifecycle state, not structural
				// corruption. refresh() is enough to re-sample generation/attach and
				// will only reopen on an actually invalid mapping.
				if refreshErr := q.refresh(); refreshErr != nil {
					ringTracef("shmdirect.push.refresh_peer_not_ready_fail kind=%d id=%d attempts=%d err=%v\n", kind, id, attempts, refreshErr)
					return refreshErr
				}
			}
			if attempts == 1 || attempts%50 == 0 {
				ringTracef("shmdirect.push.wait kind=%d id=%d attempts=%d peer_ready=%t writable=%t wait_ms=%d\n",
					kind, id, attempts, peerReady, writable, time.Since(started).Milliseconds())
			}
		} else {
			q.mu.Unlock()
			ringTracef("shmdirect.push.refresh_fail kind=%d id=%d attempts=%d err=%v\n", kind, id, attempts, err)
			return err
		}
		select {
		case <-ctx.Done():
			ringTracef("shmdirect.push.ctx_done kind=%d id=%d attempts=%d wait_ms=%d err=%v\n",
				kind, id, attempts, time.Since(started).Milliseconds(), ctx.Err())
			return ctx.Err()
		default:
		}
		// Ring full (or peer not yet ready): park on the tx notify futex until
		// the C peer pops a frame and bumps, with a bounded timeout safety net.
		if waitErr := q.WaitForWritable(ctx.Done(), waitTimeout); waitErr != nil {
			return waitErr
		}
	}
}

func (q *shmQueuePair) PushStreamData(ctx context.Context, kind byte, id uint32, payload []byte) (int, error) {
	written := 0
	idleBackoff := shmQueueWaitTimeout
	for written < len(payload) {
		q.mu.Lock()
		err := q.refreshLocked()
		n := 0
		if err == nil {
			n, err = q.tx.pushDataFrames(kind, id, payload[written:], bridgeRingDataPayloadMax)
		}
		q.mu.Unlock()
		if err != nil {
			return written, err
		}
		if n > 0 {
			written += n
			continue
		}
		// Ring is full but payload remains. Park on the tx notify futex until
		// the C peer pops a frame and bumps the counter, instead of spinning on
		// a timer. The timeout is only a safety net for a missed wake.
		select {
		case <-ctx.Done():
			return written, ctx.Err()
		default:
		}
		if waitErr := q.WaitForWritable(ctx.Done(), idleBackoff); waitErr != nil {
			return written, waitErr
		}
	}
	return written, nil
}

func (q *shmQueuePair) PopEvents(batch []*pooledRingFrame) (int, error) {
	q.mu.Lock()
	defer q.mu.Unlock()
	if err := q.refreshLocked(); err != nil {
		return 0, err
	}
	return q.rx.popBatch(batch)
}

// WaitForReadable parks until the rx ring has a frame or the timeout elapses.
// The lock is held only for the attach-state resync and the notify-counter
// sample; the futex wait itself runs WITHOUT q.mu so it never blocks concurrent
// push/pop. inWait is incremented UNDER the lock before unlocking so a concurrent
// reopen cannot unmap this ring's mmap while we are parked on it: reopen retires
// the swapped-out ring and only reaps (unmaps) it once inWait drops to zero.
func (q *shmQueuePair) WaitForReadable(done <-chan struct{}, timeout time.Duration) error {
	q.mu.Lock()
	if err := q.refreshLocked(); err != nil {
		q.mu.Unlock()
		return err
	}
	if q.rx == nil {
		q.mu.Unlock()
		sleepOrDone(done, timeout)
		return nil
	}
	rx := q.rx
	observed := rx.notifySeq()
	if rx.hasReadable() {
		q.mu.Unlock()
		return nil
	}
	rx.inWait.Add(1)
	q.mu.Unlock()
	rx.waitForChange(done, observed, timeout)
	rx.inWait.Add(-1)
	q.maybeReapRetired()
	return nil
}

// WaitForWritable parks until the tx ring has a free slot or the timeout
// elapses. The C peer bumps the tx notify counter after it pops a frame, so a
// producer that fills the ring parks on the futex instead of busy-spinning while
// waiting for the consumer to drain. As with WaitForReadable, q.mu is released
// before the futex wait so other producers/consumers are not blocked; observed
// is sampled under the lock before the hasWritable() check so a pop racing in
// between still wakes us, and inWait pins the mmap against a concurrent reopen
// unmap for the duration of the park.
func (q *shmQueuePair) WaitForWritable(done <-chan struct{}, timeout time.Duration) error {
	q.mu.Lock()
	if err := q.refreshLocked(); err != nil {
		q.mu.Unlock()
		return err
	}
	if q.tx == nil {
		q.mu.Unlock()
		sleepOrDone(done, timeout)
		return nil
	}
	tx := q.tx
	observed := tx.notifySeq()
	if tx.hasWritable() {
		q.mu.Unlock()
		return nil
	}
	tx.inWait.Add(1)
	q.mu.Unlock()
	tx.waitForChange(done, observed, timeout)
	tx.inWait.Add(-1)
	q.maybeReapRetired()
	return nil
}

func (q *shmQueuePair) Close() error {
	if q == nil {
		return nil
	}
	q.mu.Lock()
	// Unmap any retired rings unconditionally on teardown. By the time Close is
	// called the adapter's readLoop and producers have stopped, so no waiter
	// remains parked; force-reap regardless of inWait.
	for _, r := range q.retired {
		if r != nil {
			_ = r.close()
		}
	}
	q.retired = nil
	q.retiredPending.Store(false)
	tx := q.tx
	rx := q.rx
	q.mu.Unlock()
	var err1 error
	if tx != nil {
		err1 = tx.close()
	}
	if rx != nil {
		_ = rx.close()
	}
	return err1
}

// connState is the explicit lifecycle state of a shmQueueConn. It replaces the
// previous implicit scattering of sync.Once + channel-close + bool flags with a
// single authoritative state guarded by stateMu. The channels (done, readCh)
// remain the underlying wakeup primitives, but every lifecycle transition flows
// through transition() so the order and idempotency are centralized and testable.
type connState int32

const (
	// connOpen is the initial/live state: both read and write halves usable.
	connOpen connState = iota
	// connReadClosed means the peer half-closed; the read half (readCh) is shut
	// but the write half is still usable. Reachable only from connOpen.
	connReadClosed
	// connClosed is terminal: both halves torn down, done closed.
	connClosed
)

func (s connState) String() string {
	switch s {
	case connOpen:
		return "open"
	case connReadClosed:
		return "read_closed"
	case connClosed:
		return "closed"
	default:
		return "unknown"
	}
}

// shmQueueConn adapts the shared-memory queue API back to net.Conn semantics
// for compatibility frontends while keeping the implementation underneath free
// from bridge-transport specific helpers.
type shmQueueConn struct {
	queue    shmQueueEndpoint
	id       uint32
	proto    byte
	target   string
	readCh   chan *pooledRingFrame
	statusCh chan error
	done     chan struct{}

	stateMu   sync.Mutex
	state     connState // guarded by stateMu
	readChCl  bool      // readCh already closed; guarded by stateMu
	doneCl    bool      // done already closed; guarded by stateMu
	closeSent bool      // wire-level close frame already pushed; guarded by stateMu

	// readFrame/readOff are owned by the single Read() goroutine (net.Conn's
	// contract: reads are serialized by the caller).
	readFrame *pooledRingFrame
	readOff   int

	// writeMu serializes Write() so the UDP reassembly buffer (udpRx) and the
	// stream write path are safe under net.Conn's concurrent-Write contract.
	writeMu sync.Mutex
	udpRx   []byte // guarded by writeMu

	// backpressure counts consecutive failed (full-buffer) deliveries for a TCP
	// conn. Only read/written by the single dispatch worker that owns this conn's
	// shard, so it needs no synchronization. Reset to 0 on a successful delivery.
	backpressure int
}

// transition advances the connection lifecycle to the target state and performs
// the channel side effects exactly once. Transitions are monotonic: a request to
// move to an earlier-or-equal state than the current one is ignored (idempotent),
// so concurrent Close / half-close / status-drop callers converge safely.
//
//   connOpen -> connReadClosed : close readCh (read EOF), write half stays open
//   connOpen -> connClosed     : close readCh + done (full teardown)
//   connReadClosed -> connClosed: close done (readCh already closed)
func (c *shmQueueConn) transition(to connState) {
	c.stateMu.Lock()
	defer c.stateMu.Unlock()
	if to <= c.state {
		return
	}
	c.state = to
	if to >= connReadClosed && !c.readChCl {
		c.readChCl = true
		close(c.readCh)
	}
	if to >= connClosed && !c.doneCl {
		c.doneCl = true
		close(c.done)
	}
}

func (c *shmQueueConn) currentState() connState {
	c.stateMu.Lock()
	defer c.stateMu.Unlock()
	return c.state
}

func (c *shmQueueConn) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	for c.readFrame == nil || c.readOff >= len(c.readFrame.data) {
		if c.readFrame != nil {
			releaseRingFrame(c.readFrame)
			c.readFrame = nil
			c.readOff = 0
		}
		frame, ok := <-c.readCh
		if !ok {
			return 0, io.EOF
		}
		if frame == nil || len(frame.data) == 0 {
			releaseRingFrame(frame)
			continue
		}
		c.readFrame = frame
		c.readOff = 0
	}
	n := copy(p, c.readFrame.data[c.readOff:])
	c.readOff += n
	if c.readOff >= len(c.readFrame.data) {
		releaseRingFrame(c.readFrame)
		c.readFrame = nil
		c.readOff = 0
	}
	return n, nil
}

func (c *shmQueueConn) Write(p []byte) (int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if c.proto == bridgeProtoUDP {
		return c.writeUDP(ctx, p)
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	written, err := c.queue.PushStreamData(ctx, bridgeRingKindTCPData, c.id, p)
	return written, err
}

// writeUDP reassembles length-prefixed UDP datagrams from the stream of Write
// payloads and pushes each complete datagram. It is guarded by writeMu so the
// udpRx buffer is safe under concurrent Write. The buffer is bounded so sustained
// backpressure cannot grow it without limit. A datagram that cannot be pushed
// because the context is done propagates that error; any other push failure is
// treated as UDP loss (datagram dropped, connection kept) — symmetric with the
// dispatch-side UDP backpressure policy. The whole of p is always consumed, so
// the caller never re-sends (which would duplicate datagrams).
func (c *shmQueueConn) writeUDP(ctx context.Context, p []byte) (int, error) {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	// Bound the reassembly buffer: if absorbing p would exceed the cap, the peer
	// is hopelessly behind. Drop the stale backlog (UDP tolerates loss) rather
	// than risk unbounded growth.
	if len(c.udpRx)+len(p) > shmUDPReassemblyMax {
		ringTracef("shmdirect.write.udp_drop_backlog id=%d backlog=%d incoming=%d\n", c.id, len(c.udpRx), len(p))
		c.udpRx = c.udpRx[:0]
		if len(p) > shmUDPReassemblyMax {
			return len(p), nil // single oversized write cannot be valid; drop it
		}
	}

	c.udpRx = append(c.udpRx, p...)
	for {
		if len(c.udpRx) < 4 {
			return len(p), nil
		}
		addrLen := int(readBE16Go(c.udpRx[:2]))
		if addrLen == 0 || addrLen > 65535 || len(c.udpRx) < 4+addrLen {
			return len(p), nil
		}
		dataLen := int(readBE16Go(c.udpRx[2+addrLen : 4+addrLen]))
		total := 4 + addrLen + dataLen
		if len(c.udpRx) < total {
			return len(p), nil
		}
		err := c.queue.PushEvent(ctx, bridgeRingKindUDPData, c.id, c.udpRx[:total])
		if err != nil {
			if ctx.Err() != nil {
				// Caller-visible cancellation/timeout: surface it. p is reported
				// fully consumed so a retry does not duplicate the buffered tail.
				c.udpRx = c.udpRx[:0]
				return len(p), err
			}
			// Transient push failure under load: drop this datagram (UDP loss),
			// keep the connection, continue with the rest of the buffer.
			ringTracef("shmdirect.write.udp_drop id=%d bytes=%d err=%v\n", c.id, total, err)
		}
		// Consume the datagram from the head whether pushed or dropped.
		c.udpRx = c.udpRx[:copy(c.udpRx, c.udpRx[total:])]
	}
}

func (c *shmQueueConn) Close() error {
	// Atomically claim the role of the closer: the first caller to flip closeSent
	// pushes the wire-level close frame exactly once; concurrent callers return
	// immediately. The transition to connClosed performs the channel teardown
	// idempotently regardless of who wins.
	c.stateMu.Lock()
	if c.closeSent || c.state >= connClosed {
		c.stateMu.Unlock()
		return nil
	}
	c.closeSent = true
	c.stateMu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	err := c.queue.PushEvent(ctx, bridgeRingKindClose, c.id, nil)
	c.transition(connClosed)
	return err
}

func (c *shmQueueConn) CloseWrite() error {
	if c.proto != bridgeProtoTCP {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	return c.queue.PushEvent(ctx, bridgeRingKindHalfClose, c.id, nil)
}

func (c *shmQueueConn) LocalAddr() net.Addr  { return bridgeAddr("shm-direct") }
func (c *shmQueueConn) RemoteAddr() net.Addr { return bridgeAddr(c.target) }
func (c *shmQueueConn) SetDeadline(time.Time) error      { return nil }
func (c *shmQueueConn) SetReadDeadline(time.Time) error  { return nil }
func (c *shmQueueConn) SetWriteDeadline(time.Time) error { return nil }

func (c *shmQueueConn) closeLocal() {
	c.transition(connClosed)
}

// closeReadHalf delivers downstream EOF to the consumer (so the phone-facing
// side sees a clean FIN once buffered frames drain) without tearing down the
// upstream write half. Used when the peer signals a TCP half-close.
func (c *shmQueueConn) closeReadHalf() {
	c.transition(connReadClosed)
}

func readBE16Go(p []byte) uint16 {
	return uint16(p[0])<<8 | uint16(p[1])
}
