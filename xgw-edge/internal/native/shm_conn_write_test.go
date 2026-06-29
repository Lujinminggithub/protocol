package native

import (
	"context"
	"errors"
	"sync"
	"testing"
)

// fakeEndpoint is a controllable shmQueueEndpoint for exercising shmQueueConn's
// Write path without a real shared-memory ring.
type fakeEndpoint struct {
	mu       sync.Mutex
	pushed   [][]byte // payloads of successful PushEvent (UDP/close/etc.)
	failNext int      // number of upcoming PushEvent calls to fail
	failErr  error
}

func (f *fakeEndpoint) PushEvent(ctx context.Context, kind byte, id uint32, payload []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.failNext > 0 {
		f.failNext--
		return f.failErr
	}
	cp := append([]byte(nil), payload...)
	f.pushed = append(f.pushed, cp)
	return nil
}

func (f *fakeEndpoint) PushStreamData(ctx context.Context, kind byte, id uint32, payload []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.failNext > 0 {
		f.failNext--
		return 0, f.failErr
	}
	cp := append([]byte(nil), payload...)
	f.pushed = append(f.pushed, cp)
	return len(payload), nil
}

func (f *fakeEndpoint) PopEvents(batch []*pooledRingFrame) (int, error) { return 0, nil }
func (f *fakeEndpoint) Close() error                                    { return nil }

func (f *fakeEndpoint) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.pushed)
}

// udpFrame builds a length-prefixed UDP datagram payload as Write expects:
// addrLen(2) | addr | dataLen(2) | data.
func udpFrame(addr string, data []byte) []byte {
	out := make([]byte, 0, 4+len(addr)+len(data))
	out = append(out, byte(len(addr)>>8), byte(len(addr)))
	out = append(out, addr...)
	out = append(out, byte(len(data)>>8), byte(len(data)))
	out = append(out, data...)
	return out
}

func newUDPConn(ep shmQueueEndpoint) *shmQueueConn {
	return &shmQueueConn{
		queue:  ep,
		id:     1,
		proto:  bridgeProtoUDP,
		target: "test",
		done:   make(chan struct{}),
	}
}

// TestWriteUDPReassembly verifies multiple datagrams across fragmented writes are
// reassembled and pushed in order.
func TestWriteUDPReassembly(t *testing.T) {
	ep := &fakeEndpoint{}
	c := newUDPConn(ep)
	f1 := udpFrame("1.1.1.1:53", []byte("aaa"))
	f2 := udpFrame("2.2.2.2:53", []byte("bbbb"))
	stream := append(append([]byte(nil), f1...), f2...)

	// Feed in two arbitrary fragments that split a datagram boundary.
	split := len(f1) + 2
	if n, err := c.Write(stream[:split]); err != nil || n != split {
		t.Fatalf("write1: n=%d err=%v", n, err)
	}
	if n, err := c.Write(stream[split:]); err != nil || n != len(stream)-split {
		t.Fatalf("write2: n=%d err=%v", n, err)
	}
	if ep.count() != 2 {
		t.Fatalf("want 2 datagrams pushed, got %d", ep.count())
	}
}

// TestWriteUDPDropOnTransientFailure verifies a transient (non-ctx) push failure
// drops that datagram but keeps the conn usable (UDP loss tolerance) and reports
// the whole write consumed (no caller retry / duplication).
func TestWriteUDPDropOnTransientFailure(t *testing.T) {
	ep := &fakeEndpoint{failNext: 1, failErr: errors.New("ring full")}
	c := newUDPConn(ep)
	f := udpFrame("9.9.9.9:53", []byte("zzz"))

	n, err := c.Write(f)
	if err != nil {
		t.Fatalf("transient failure should not surface as error, got %v", err)
	}
	if n != len(f) {
		t.Fatalf("want full write consumed (%d), got %d", len(f), n)
	}
	if ep.count() != 0 {
		t.Fatalf("datagram should have been dropped, got %d pushed", ep.count())
	}
	// Buffer must be drained (datagram consumed, not left to grow).
	c.writeMu.Lock()
	bufLen := len(c.udpRx)
	c.writeMu.Unlock()
	if bufLen != 0 {
		t.Fatalf("udpRx should be empty after drop, got %d bytes", bufLen)
	}

	// Next datagram pushes fine — conn still alive.
	if n, err := c.Write(udpFrame("9.9.9.9:53", []byte("ok"))); err != nil || n == 0 {
		t.Fatalf("conn should still work after a dropped datagram: n=%d err=%v", n, err)
	}
	if ep.count() != 1 {
		t.Fatalf("want 1 pushed after recovery, got %d", ep.count())
	}
}

// TestWriteUDPHealthyCtxNeverSurfaces verifies that as long as the per-Write ctx
// is healthy, repeated push failures are absorbed as UDP loss and Write never
// returns an error (it only surfaces when the ctx itself is done). This documents
// that writeUDP keys on ctx state, not on the push error value.
func TestWriteUDPHealthyCtxNeverSurfaces(t *testing.T) {
	ep := &fakeEndpoint{failNext: 100, failErr: errors.New("ring full")}
	c := newUDPConn(ep)
	for i := 0; i < 5; i++ {
		if _, err := c.Write(udpFrame("9.9.9.9:53", []byte("zzz"))); err != nil {
			t.Fatalf("healthy ctx must absorb push failure as loss, got err=%v", err)
		}
	}
	c.writeMu.Lock()
	bufLen := len(c.udpRx)
	c.writeMu.Unlock()
	if bufLen != 0 {
		t.Fatalf("udpRx should be drained, got %d bytes", bufLen)
	}
}

// TestWriteUDPBufferBound verifies the reassembly buffer never exceeds the cap:
// a flood of partial (never-complete) datagrams gets dropped rather than grown
// without bound.
func TestWriteUDPBufferBound(t *testing.T) {
	ep := &fakeEndpoint{}
	c := newUDPConn(ep)
	// Craft a header claiming a huge datagram so bytes accumulate without ever
	// completing, then keep feeding until we cross the cap.
	chunk := make([]byte, 64*1024)
	// First write a valid-looking oversized header (addrLen small, dataLen large).
	hdr := udpFrame("1.2.3.4:1", make([]byte, 0)) // empty data; completes immediately
	_ = hdr
	total := 0
	for i := 0; i < (shmUDPReassemblyMax/len(chunk))+4; i++ {
		// Use a partial datagram: a 2-byte addrLen prefix that is incomplete so it
		// stays buffered. Feed raw bytes that look like an in-progress frame.
		partial := make([]byte, len(chunk))
		partial[0] = 0xFF // addrLen high byte -> large addr, never satisfied
		partial[1] = 0xFF
		c.Write(partial)
		total += len(partial)
	}
	c.writeMu.Lock()
	bufLen := len(c.udpRx)
	c.writeMu.Unlock()
	if bufLen > shmUDPReassemblyMax {
		t.Fatalf("udpRx exceeded cap: %d > %d (fed %d)", bufLen, shmUDPReassemblyMax, total)
	}
}

// TestWriteUDPConcurrent verifies concurrent Write calls are safe (writeMu) and
// the race detector finds nothing. Each goroutine sends complete datagrams.
func TestWriteUDPConcurrent(t *testing.T) {
	ep := &fakeEndpoint{}
	c := newUDPConn(ep)
	var wg sync.WaitGroup
	const writers = 8
	const each = 50
	for w := 0; w < writers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < each; i++ {
				_, _ = c.Write(udpFrame("1.1.1.1:1", []byte("x")))
			}
		}()
	}
	wg.Wait()
	if ep.count() != writers*each {
		t.Fatalf("want %d datagrams, got %d", writers*each, ep.count())
	}
}
