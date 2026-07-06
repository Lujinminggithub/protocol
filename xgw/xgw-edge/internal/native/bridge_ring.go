package native

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"github.com/local/xgw-edge/internal/coremodel"
)

const (
	bridgeRingMagic          uint32 = 0x58475231
	bridgeRingVersion        uint32 = 2
	bridgeRingCapacity              = 1024
	bridgeRingSlotSize              = 65536
	bridgeRingHeaderSize            = 64
	bridgeRingSlotHead              = 16
	bridgeRingFrameMax              = bridgeRingSlotSize - bridgeRingSlotHead
	bridgeRingDataPayloadMax        = bridgeRingFrameMax - 5
	bridgeRingNotifyOffset          = 16
	bridgeRingFlagsOffset           = 20
	bridgeRingGenerationOffset      = 24
	bridgeRingWriteSeqOffset        = 32
	bridgeRingReadSeqOffset         = 40
	bridgeRingWriterRoleOffset      = 48
	bridgeRingReaderRoleOffset      = 52
	bridgeRingWriterAttachOffset    = 56
	bridgeRingReaderAttachOffset    = 60
	bridgeRingBatchLimit            = 64
	bridgeRingMaxPendingOpens       = 512

	bridgeRingKindStatus  byte = 1
	bridgeRingKindTCPOpen byte = 2
	bridgeRingKindUDPOpen byte = 3
	bridgeRingKindTCPData byte = 4
	bridgeRingKindUDPData byte = 5
	bridgeRingKindClose   byte = 6
	bridgeRingKindHalfClose byte = 7
)

const (
	bridgeRingFlagOwnerReady uint32 = 1 << 0
	bridgeRingFlagWriterReady uint32 = 1 << 1
	bridgeRingFlagReaderReady uint32 = 1 << 2
)

const (
	bridgeRingRoleFront   uint32 = 1
	bridgeRingRoleIngress uint32 = 2
)

type sharedRingBridgeTransport struct {
	basePath string
	go2c     *bridgeRingFile
	c2go     *bridgeRingFile
	nextID   atomic.Uint32
	closed   chan struct{}
	done     chan struct{}

	mu    sync.Mutex
	conns map[uint32]*sharedRingConn
	pendingOpens int
}

const sharedRingWaitTimeout = 50 * time.Millisecond

// refreshFilesIfNeeded keeps both rings attached to the live epoch using the
// pure generation/attach protocol. The ring paths are a stable namespace, so we
// never stat the filesystem and never reopen on a generation bump: a new epoch
// is handled by an in-place reattach (resync header, re-publish attach, drop
// stale per-conn read state) while the mmap is preserved. A full reopen is the
// last resort only when the mapping itself is structurally invalid (corrupt
// magic/version), which the stable-namespace contract should make unreachable.
func (t *sharedRingBridgeTransport) refreshFilesIfNeeded() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.go2c == nil || t.c2go == nil {
		return errors.New("shared-ring files are not initialized")
	}
	if err := t.go2c.refreshLayout(); err != nil {
		return t.reopenLocked("go2c_layout_invalid")
	}
	if err := t.c2go.refreshLayout(); err != nil {
		return t.reopenLocked("c2go_layout_invalid")
	}
	reattached := t.go2c.ensureAttached()
	if t.c2go.ensureAttached() {
		reattached = true
	}
	if reattached {
		ringTracef("sharedring.reattach base=%s generation_go2c=%d generation_c2go=%d\n",
			t.basePath, t.go2c.generation, t.c2go.generation)
		for _, conn := range t.conns {
			if conn.readFrame != nil {
				releaseRingFrame(conn.readFrame)
				conn.readFrame = nil
			}
			conn.readOff = 0
		}
	}
	return nil
}

// reopenLocked is the structural-recovery path used only when the existing
// mapping is unusable (corrupt header). Under the stable-namespace contract this
// should not happen in steady state; it exists so a one-off bad mapping can heal
// rather than wedge the transport. Caller holds t.mu.
func (t *sharedRingBridgeTransport) reopenLocked(reason string) error {
	ringTracef("sharedring.reopen base=%s reason=%s\n", t.basePath, reason)
	newGo2c, err := openBridgeRingFile(t.basePath+".go2c", true, bridgeRingRoleFront, bridgeRingFlagWriterReady, bridgeRingFlagReaderReady, true)
	if err != nil {
		return err
	}
	newC2go, err := openBridgeRingFile(t.basePath+".c2go", true, bridgeRingRoleFront, bridgeRingFlagReaderReady, bridgeRingFlagWriterReady, false)
	if err != nil {
		_ = newGo2c.close()
		return err
	}
	oldGo2c := t.go2c
	oldC2go := t.c2go
	t.go2c = newGo2c
	t.c2go = newC2go
	for _, conn := range t.conns {
		if conn.readFrame != nil {
			releaseRingFrame(conn.readFrame)
			conn.readFrame = nil
		}
		conn.readOff = 0
	}
	_ = oldGo2c.close()
	_ = oldC2go.close()
	return nil
}

type bridgeRingFile struct {
	mu       sync.Mutex
	path     string
	file     *os.File
	data     []byte
	capacity uint32
	slotSize uint32
	// generation is the live epoch id read from the shared header.
	// attachedGeneration is the epoch this side has joined/synced its local
	// state to. They diverge only when the peer republishes a new generation
	// in place (restart); that divergence drives an in-place reattach, never a
	// file reopen — the path is a stable namespace and the mmap is preserved.
	generation         uint64
	attachedGeneration uint64
	flags              uint32
	writerRole         uint32
	readerRole         uint32
	localReadyFlag     uint32
	peerReadyFlag      uint32
	localAttachOff     int
	peerAttachOff      int
	// inWait counts goroutines currently parked in waitForChange on this ring's
	// mmap WITHOUT holding the owning shmQueuePair lock. A retired ring (swapped
	// out by reopen) must not be unmapped while inWait > 0, otherwise the parked
	// futex/event wait would dereference freed memory. See shmQueuePair.reapRetiredLocked.
	inWait atomic.Int32
}

type pooledRingFrame struct {
	storage []byte
	data    []byte
}

type sharedRingConn struct {
	transport *sharedRingBridgeTransport
	id        uint32
	proto     byte
	target    string
	readCh    chan *pooledRingFrame
	statusCh  chan error
	done      chan struct{}
	closeOnce sync.Once
	localOnce sync.Once
	readCloseOnce sync.Once

	readFrame *pooledRingFrame
	readOff   int
	udpRx     []byte
	deadline  atomic.Value
	closed    atomic.Bool
}

var bridgeRingFramePool = sync.Pool{
	New: func() any {
		storage := make([]byte, bridgeRingFrameMax)
		return &pooledRingFrame{storage: storage, data: storage[:0]}
	},
}

func acquireRingFrame() *pooledRingFrame {
	frame := bridgeRingFramePool.Get().(*pooledRingFrame)
	frame.data = frame.storage[:0]
	return frame
}

func releaseRingFrame(frame *pooledRingFrame) {
	if frame == nil {
		return
	}
	frame.data = frame.storage[:0]
	bridgeRingFramePool.Put(frame)
}

func newSharedRingBridgeTransport(basePath string) (*sharedRingBridgeTransport, error) {
	if basePath == "" {
		return nil, errors.New("empty bridge ring path")
	}
	if err := os.MkdirAll(filepath.Dir(basePath), 0o755); err != nil {
		return nil, err
	}
	go2c, err := openBridgeRingFile(basePath+".go2c", true, bridgeRingRoleFront, bridgeRingFlagWriterReady, bridgeRingFlagReaderReady, true)
	if err != nil {
		return nil, err
	}
	c2go, err := openBridgeRingFile(basePath+".c2go", true, bridgeRingRoleFront, bridgeRingFlagReaderReady, bridgeRingFlagWriterReady, false)
	if err != nil {
		_ = go2c.close()
		return nil, err
	}
	t := &sharedRingBridgeTransport{
		basePath: basePath,
		go2c:     go2c,
		c2go:     c2go,
		closed:   make(chan struct{}),
		done:     make(chan struct{}),
		conns:    make(map[uint32]*sharedRingConn),
	}
	go t.readLoop()
	return t, nil
}

func (t *sharedRingBridgeTransport) OpenTCP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error) {
	return t.open(ctx, bridgeRingKindTCPOpen, req)
}

func (t *sharedRingBridgeTransport) OpenUDP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error) {
	return t.open(ctx, bridgeRingKindUDPOpen, req)
}

func (t *sharedRingBridgeTransport) Close() error {
	select {
	case <-t.closed:
	default:
		close(t.closed)
	}
	<-t.done
	t.mu.Lock()
	for _, conn := range t.conns {
		conn.closeLocal()
	}
	t.conns = map[uint32]*sharedRingConn{}
	t.mu.Unlock()
	err1 := t.go2c.close()
	err2 := t.c2go.close()
	if err1 != nil {
		return err1
	}
	return err2
}

func (t *sharedRingBridgeTransport) open(ctx context.Context, kind byte, req coremodel.OpenRequest) (net.Conn, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	proto := bridgeProtoTCP
	if kind == bridgeRingKindUDPOpen {
		proto = bridgeProtoUDP
	}
	id := t.nextID.Add(1)
	if id == 0 {
		id = t.nextID.Add(1)
	}
	conn := &sharedRingConn{
		transport: t,
		id:        id,
		proto:     proto,
		target:    req.Flow.Target,
		readCh:    make(chan *pooledRingFrame, 1024),
		statusCh:  make(chan error, 1),
		done:      make(chan struct{}),
	}
	t.mu.Lock()
	if t.pendingOpens >= bridgeRingMaxPendingOpens {
		t.mu.Unlock()
		return nil, fmt.Errorf("shared-ring bridge open backlog full target=%s", req.Flow.Target)
	}
	t.pendingOpens++
	t.conns[id] = conn
	t.mu.Unlock()
	defer func() {
		t.mu.Lock()
		if t.pendingOpens > 0 {
			t.pendingOpens--
		}
		t.mu.Unlock()
	}()

	openPayload, err := encodeRingOpenPayload(proto, req)
	if err != nil {
		t.unregister(id)
		return nil, err
	}
	if err := t.refreshFilesIfNeeded(); err != nil {
		t.unregister(id)
		return nil, err
	}
	ringTracef("sharedring.open.begin id=%d kind=%d target=%s bytes=%d\n", id, kind, req.Flow.Target, len(openPayload)+5)
	if err := t.writeFrameCtx(ctx, kind, id, openPayload); err != nil {
		ringTracef("sharedring.open.write_fail id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, err)
		t.unregister(id)
		return nil, err
	}
	ringTracef("sharedring.open.write_ok id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
	timer := time.NewTimer(defaultDialTimeout)
	defer timer.Stop()
	select {
	case err := <-conn.statusCh:
		ringTracef("sharedring.open.status id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, err)
		if err != nil || !t.c2go.peerReady() {
			t.unregister(id)
			if err == nil {
				return nil, fmt.Errorf("shared-ring peer not ready target=%s", req.Flow.Target)
			}
			return nil, err
		}
		return conn, nil
	case <-ctx.Done():
		ringTracef("sharedring.open.ctx_done id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, ctx.Err())
		t.unregister(id)
		return nil, ctx.Err()
	case <-timer.C:
		ringTracef("sharedring.open.timeout id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
		if healErr := t.refreshFilesIfNeeded(); healErr == nil {
			if !t.c2go.peerReady() {
				time.Sleep(10 * time.Millisecond)
				_ = t.refreshFilesIfNeeded()
			}
			retryTimer := time.NewTimer(defaultDialTimeout / 2)
			defer retryTimer.Stop()
			if err := t.writeFrameCtx(ctx, kind, id, openPayload); err == nil {
				select {
				case err := <-conn.statusCh:
					ringTracef("sharedring.open.heal_status id=%d kind=%d target=%s err=%v\n", id, kind, req.Flow.Target, err)
					if err == nil && t.c2go.peerReady() {
						return conn, nil
					}
				case <-retryTimer.C:
					ringTracef("sharedring.open.heal_timeout id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
				case <-ctx.Done():
				case <-t.closed:
				}
			}
		}
		t.unregister(id)
		return nil, fmt.Errorf("shared-ring bridge open timeout target=%s", req.Flow.Target)
	case <-t.closed:
		ringTracef("sharedring.open.closed id=%d kind=%d target=%s\n", id, kind, req.Flow.Target)
		t.unregister(id)
		return nil, io.ErrClosedPipe
	}
}

func (t *sharedRingBridgeTransport) writeFrame(kind byte, id uint32, payload []byte) error {
	return t.writeFrameCtx(context.Background(), kind, id, payload)
}

func (t *sharedRingBridgeTransport) writeFrameCtx(ctx context.Context, kind byte, id uint32, payload []byte) error {
	if len(payload)+5 > bridgeRingFrameMax {
		return fmt.Errorf("shared-ring frame too large kind=%d bytes=%d", kind, len(payload)+5)
	}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-t.closed:
			return io.ErrClosedPipe
		default:
		}
		observed := t.go2c.notifySeq()
		ok, err := t.go2c.pushFrame(kind, id, payload)
		if err != nil {
			return err
		}
		if ok {
			return nil
		}
		if err := t.refreshFilesIfNeeded(); err != nil {
			return err
		}
		// Ring full: park on the notify futex until the C peer pops and bumps.
		// observed was sampled before pushFrame so a pop racing in between still
		// wakes us. The bounded timeout is only a missed-wake safety net.
		t.go2c.waitForChange(t.closed, observed, sharedRingWaitTimeout)
	}
}

func (t *sharedRingBridgeTransport) writeDataFrames(kind byte, id uint32, payload []byte) (int, error) {
	written := 0
	for written < len(payload) {
		select {
		case <-t.closed:
			return written, io.ErrClosedPipe
		default:
		}
		observed := t.go2c.notifySeq()
		n, err := t.go2c.pushDataFrames(kind, id, payload[written:], bridgeRingDataPayloadMax)
		if err != nil {
			return written, err
		}
		if n > 0 {
			written += n
			continue
		}
		if err := t.refreshFilesIfNeeded(); err != nil {
			return written, err
		}
		// Ring full with payload remaining: park on the notify futex until the
		// C peer pops and bumps, rather than busy-spinning. observed sampled
		// before the push so a racing pop still wakes us.
		t.go2c.waitForChange(t.closed, observed, sharedRingWaitTimeout)
	}
	return written, nil
}

func (t *sharedRingBridgeTransport) readLoop() {
	defer close(t.done)
	var batch [bridgeRingBatchLimit]*pooledRingFrame
	idleBackoff := 250 * time.Microsecond
	for {
		select {
		case <-t.closed:
			return
		default:
		}
		n, err := t.c2go.popBatch(batch[:])
		if err != nil {
			observed := t.c2go.notifySeq()
			_ = t.refreshFilesIfNeeded()
			t.c2go.waitForChange(t.closed, observed, idleBackoff)
			if idleBackoff < sharedRingWaitTimeout {
				idleBackoff *= 2
				if idleBackoff > sharedRingWaitTimeout {
					idleBackoff = sharedRingWaitTimeout
				}
			}
			continue
		}
		if n == 0 {
			// Empty: sample the notify counter (after the failed pop, so a
			// racing write still shows up) and park on the futex until the C
			// peer writes and bumps. No hasReadable() short-circuit, which
			// would reintroduce a busy-spin on the empty boundary.
			observed := t.c2go.notifySeq()
			_ = t.refreshFilesIfNeeded()
			t.c2go.waitForChange(t.closed, observed, idleBackoff)
			if idleBackoff < sharedRingWaitTimeout {
				idleBackoff *= 2
				if idleBackoff > sharedRingWaitTimeout {
					idleBackoff = sharedRingWaitTimeout
				}
			}
			continue
		}
		idleBackoff = 250 * time.Microsecond
		for i := 0; i < n; i++ {
			t.dispatchFrame(batch[i])
			batch[i] = nil
		}
	}
}

func (t *sharedRingBridgeTransport) dispatchFrame(frame *pooledRingFrame) {
	if len(frame.data) < 5 {
		releaseRingFrame(frame)
		return
	}
	kind := frame.data[0]
	id := binary.BigEndian.Uint32(frame.data[1:5])
	t.mu.Lock()
	conn := t.conns[id]
	t.mu.Unlock()
	if conn == nil {
		releaseRingFrame(frame)
		return
	}
	body := frame.data[5:]
	switch kind {
	case bridgeRingKindStatus:
		err := decodeRingStatus(body)
		ringTracef("sharedring.dispatch.status id=%d err=%v bytes=%d\n", id, err, len(body))
		releaseRingFrame(frame)
		select {
		case conn.statusCh <- err:
		default:
			t.unregister(id)
		}
	case bridgeRingKindTCPData:
		frame.data = body
		if !conn.deliver(frame) {
			t.unregister(id)
		}
	case bridgeRingKindUDPData:
		frame.data = body
		if !conn.deliver(frame) {
			t.unregister(id)
		}
	case bridgeRingKindClose:
		releaseRingFrame(frame)
		conn.closeLocal()
		t.unregister(id)
	case bridgeRingKindHalfClose:
		ringTracef("sharedring.dispatch.half_close id=%d\n", id)
		releaseRingFrame(frame)
		conn.closeReadHalf()
	default:
		releaseRingFrame(frame)
	}
}

func (t *sharedRingBridgeTransport) unregister(id uint32) {
	t.mu.Lock()
	conn := t.conns[id]
	delete(t.conns, id)
	t.mu.Unlock()
	if conn != nil {
		conn.closeLocal()
	}
}

func openBridgeRingFile(path string, create bool, localRole uint32, localReadyFlag uint32, peerReadyFlag uint32, writer bool) (*bridgeRingFile, error) {
	flag := os.O_RDWR
	if create {
		flag |= os.O_CREATE
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, err
	}
	file, err := os.OpenFile(path, flag, 0o600)
	if err != nil {
		return nil, err
	}
	r := &bridgeRingFile{
		path:          path,
		file:          file,
		capacity:      bridgeRingCapacity,
		slotSize:      bridgeRingSlotSize,
		localReadyFlag: localReadyFlag,
		peerReadyFlag:  peerReadyFlag,
	}
	if writer {
		r.localAttachOff = bridgeRingWriterAttachOffset
		r.peerAttachOff = bridgeRingReaderAttachOffset
	} else {
		r.localAttachOff = bridgeRingReaderAttachOffset
		r.peerAttachOff = bridgeRingWriterAttachOffset
	}
	if localRole == bridgeRingRoleFront {
		if writer {
			r.writerRole = bridgeRingRoleFront
			r.readerRole = bridgeRingRoleIngress
		} else {
			r.writerRole = bridgeRingRoleIngress
			r.readerRole = bridgeRingRoleFront
		}
	}
	if err := r.ensureLayout(); err != nil {
		_ = file.Close()
		return nil, err
	}
	data, err := mapBridgeRingFile(file, r.layoutSize())
	if err != nil {
		_ = file.Close()
		return nil, err
	}
	r.data = data
	// Initialization (open + layout + map) is complete; joining the live epoch
	// is a separate step (attach), keeping the two responsibilities distinct.
	if err := r.refreshLayout(); err != nil {
		_ = unmapBridgeRingFile(r.data)
		r.data = nil
		_ = file.Close()
		return nil, err
	}
	r.attach()
	return r, nil
}

func (r *bridgeRingFile) close() error {
	if r == nil || r.file == nil {
		return nil
	}
	r.markDetached()
	if len(r.data) > 0 {
		_ = unmapBridgeRingFile(r.data)
		r.data = nil
	}
	return r.file.Close()
}

func (r *bridgeRingFile) ensureLayout() error {
	size := r.layoutSize()
	info, err := r.file.Stat()
	if err != nil {
		return err
	}
	header := make([]byte, bridgeRingHeaderSize)
	if info.Size() >= int64(bridgeRingHeaderSize) {
		if _, err := r.file.ReadAt(header, 0); err == nil {
			magic := binary.LittleEndian.Uint32(header[:4])
			version := binary.LittleEndian.Uint32(header[4:8])
			capacity := binary.LittleEndian.Uint32(header[8:12])
			slotSize := binary.LittleEndian.Uint32(header[12:16])
			if magic == bridgeRingMagic && version == bridgeRingVersion && capacity == bridgeRingCapacity && slotSize == bridgeRingSlotSize {
				if info.Size() != r.layoutSize() {
					return r.file.Truncate(r.layoutSize())
				}
				return nil
			}
		}
	}
	if err := r.file.Truncate(size); err != nil {
		return err
	}
	for i := range header {
		header[i] = 0
	}
	binary.LittleEndian.PutUint32(header[:4], bridgeRingMagic)
	binary.LittleEndian.PutUint32(header[4:8], bridgeRingVersion)
	binary.LittleEndian.PutUint32(header[8:12], bridgeRingCapacity)
	binary.LittleEndian.PutUint32(header[12:16], bridgeRingSlotSize)
	binary.LittleEndian.PutUint64(header[bridgeRingGenerationOffset:bridgeRingGenerationOffset+8], uint64(time.Now().UnixNano()))
	binary.LittleEndian.PutUint32(header[bridgeRingWriterRoleOffset:bridgeRingWriterRoleOffset+4], r.writerRole)
	binary.LittleEndian.PutUint32(header[bridgeRingReaderRoleOffset:bridgeRingReaderRoleOffset+4], r.readerRole)
	if _, err := r.file.WriteAt(header, 0); err != nil {
		return err
	}
	if err := r.file.Sync(); err != nil {
		return err
	}
	return nil
}

func (r *bridgeRingFile) layoutSize() int64 {
	return int64(bridgeRingHeaderSize + bridgeRingCapacity*bridgeRingSlotSize)
}

func (r *bridgeRingFile) pushFrame(kind byte, id uint32, payload []byte) (bool, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	frameLen := 5 + len(payload)
	if frameLen > bridgeRingFrameMax {
		return false, fmt.Errorf("shared-ring payload too large: %d", frameLen)
	}
	if err := r.refreshLayout(); err != nil {
		return false, err
	}
	writeSeq := binary.LittleEndian.Uint64(r.data[bridgeRingWriteSeqOffset : bridgeRingWriteSeqOffset+8])
	readSeq := binary.LittleEndian.Uint64(r.data[bridgeRingReadSeqOffset : bridgeRingReadSeqOffset+8])
	if writeSeq-readSeq >= uint64(r.capacity) {
		return false, nil
	}
	r.writeSlotFrame(writeSeq, kind, id, payload)
		binary.LittleEndian.PutUint64(r.data[bridgeRingWriteSeqOffset:bridgeRingWriteSeqOffset+8], writeSeq+1)
	if err := syncBridgeRingMapping(r.data); err != nil {
		return false, err
	}
	r.bumpNotify()
	return true, nil
}

func (r *bridgeRingFile) pushDataFrames(kind byte, id uint32, payload []byte, maxPayload int) (int, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	written := 0
	frames := 0
	if len(payload) == 0 {
		return 0, nil
	}
	if maxPayload <= 0 || maxPayload+5 > bridgeRingFrameMax {
		return 0, fmt.Errorf("invalid shared-ring data payload max %d", maxPayload)
	}
	if err := r.refreshLayout(); err != nil {
		return 0, err
	}
	writeSeq := binary.LittleEndian.Uint64(r.data[bridgeRingWriteSeqOffset : bridgeRingWriteSeqOffset+8])
	readSeq := binary.LittleEndian.Uint64(r.data[bridgeRingReadSeqOffset : bridgeRingReadSeqOffset+8])
	for written < len(payload) && writeSeq-readSeq < uint64(r.capacity) {
		chunk := len(payload) - written
		if chunk > maxPayload {
			chunk = maxPayload
		}
		r.writeSlotFrame(writeSeq, kind, id, payload[written:written+chunk])
		writeSeq++
		written += chunk
		frames++
	}
	if frames > 0 {
		binary.LittleEndian.PutUint64(r.data[bridgeRingWriteSeqOffset:bridgeRingWriteSeqOffset+8], writeSeq)
		if err := syncBridgeRingMapping(r.data); err != nil {
			return written, err
		}
		r.bumpNotify()
	}
	return written, nil
}

func (r *bridgeRingFile) writeSlotFrame(writeSeq uint64, kind byte, id uint32, payload []byte) {
	frameLen := 5 + len(payload)
	offset := r.slotOffset(writeSeq)
	slot := r.data[offset : offset+bridgeRingSlotHead]
	for i := range slot {
		slot[i] = 0
	}
	binary.LittleEndian.PutUint64(slot[0:8], writeSeq+1)
	binary.LittleEndian.PutUint32(slot[8:12], uint32(frameLen))
	frame := r.data[offset+bridgeRingSlotHead : offset+bridgeRingSlotHead+frameLen]
	frame[0] = kind
	binary.BigEndian.PutUint32(frame[1:5], id)
	if len(payload) > 0 {
		copy(frame[5:], payload)
	}
}

func (r *bridgeRingFile) popBatch(frames []*pooledRingFrame) (int, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if err := r.refreshLayout(); err != nil {
		return 0, err
	}
	writeSeq := binary.LittleEndian.Uint64(r.data[bridgeRingWriteSeqOffset : bridgeRingWriteSeqOffset+8])
	readSeq := binary.LittleEndian.Uint64(r.data[bridgeRingReadSeqOffset : bridgeRingReadSeqOffset+8])
	if len(frames) == 0 {
		return 0, nil
	}
	if readSeq >= writeSeq {
		return 0, nil
	}
	count := 0
	for count < len(frames) && readSeq < writeSeq {
		offset := r.slotOffset(readSeq)
		slotHead := r.data[offset : offset+bridgeRingSlotHead]
		if binary.LittleEndian.Uint64(slotHead[0:8]) != readSeq+1 {
			break
		}
		n := binary.LittleEndian.Uint32(slotHead[8:12])
		if n > bridgeRingFrameMax {
			for i := 0; i < count; i++ {
				releaseRingFrame(frames[i])
				frames[i] = nil
			}
			return 0, fmt.Errorf("invalid shared-ring slot length %d", n)
		}
		frame := acquireRingFrame()
		if n > 0 {
			if int(n) > len(frame.storage) {
				releaseRingFrame(frame)
				for i := 0; i < count; i++ {
					releaseRingFrame(frames[i])
					frames[i] = nil
				}
				return 0, fmt.Errorf("shared-ring receive buffer too small: need=%d have=%d", n, len(frame.storage))
			}
			copy(frame.storage[:n], r.data[offset+bridgeRingSlotHead:offset+bridgeRingSlotHead+int(n)])
		}
		frame.data = frame.storage[:n]
		frames[count] = frame
		count++
		readSeq++
	}
	if count > 0 {
		binary.LittleEndian.PutUint64(r.data[bridgeRingReadSeqOffset:bridgeRingReadSeqOffset+8], readSeq)
		r.bumpNotify()
	}
	return count, nil
}

func (r *bridgeRingFile) refreshLayout() error {
	if len(r.data) < bridgeRingHeaderSize {
		return errors.New("shared-ring mmap is not initialized")
	}
	if binary.LittleEndian.Uint32(r.data[:4]) != bridgeRingMagic {
		return errors.New("invalid shared-ring magic")
	}
	if binary.LittleEndian.Uint32(r.data[4:8]) != bridgeRingVersion {
		return errors.New("invalid shared-ring version")
	}
	r.capacity = binary.LittleEndian.Uint32(r.data[8:12])
	r.slotSize = binary.LittleEndian.Uint32(r.data[12:16])
	if r.capacity == 0 || r.slotSize < bridgeRingSlotHead {
		return errors.New("invalid shared-ring layout")
	}
	r.readHeaderFields()
	return nil
}

// readHeaderFields snapshots the volatile header fields from the live mmap.
// Pure read, no state-machine side effects (it does NOT touch
// attachedGeneration); the attach/reattach steps own epoch transitions.
func (r *bridgeRingFile) readHeaderFields() {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return
	}
	r.flags = binary.LittleEndian.Uint32(r.data[bridgeRingFlagsOffset : bridgeRingFlagsOffset+4])
	r.generation = binary.LittleEndian.Uint64(r.data[bridgeRingGenerationOffset : bridgeRingGenerationOffset+8])
	r.writerRole = binary.LittleEndian.Uint32(r.data[bridgeRingWriterRoleOffset : bridgeRingWriterRoleOffset+4])
	r.readerRole = binary.LittleEndian.Uint32(r.data[bridgeRingReaderRoleOffset : bridgeRingReaderRoleOffset+4])
}

// generationChanged reports whether the peer published a new epoch since we
// attached. It re-reads the live generation so a single call is authoritative.
func (r *bridgeRingFile) generationChanged() bool {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return false
	}
	cur := binary.LittleEndian.Uint64(r.data[bridgeRingGenerationOffset : bridgeRingGenerationOffset+8])
	return r.attachedGeneration != 0 && cur != 0 && cur != r.attachedGeneration
}

// attach is the join step of the generation/attach protocol: adopt the current
// header epoch as our baseline and publish our readiness + liveness. Idempotent
// within an epoch; safe to re-run on every reattach. No fd/mmap churn.
func (r *bridgeRingFile) attach() {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return
	}
	r.readHeaderFields()
	r.attachedGeneration = r.generation
	flags := binary.LittleEndian.Uint32(r.data[bridgeRingFlagsOffset : bridgeRingFlagsOffset+4])
	flags |= bridgeRingFlagOwnerReady | r.localReadyFlag
	binary.LittleEndian.PutUint32(r.data[bridgeRingFlagsOffset:bridgeRingFlagsOffset+4], flags)
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&r.data[r.localAttachOff])), 1)
}

// ensureAttached is the steady-state, hot-path keep-alive of our membership. It
// reattaches if the peer bumped the generation, otherwise it only re-asserts the
// attach bit when it has been cleared — no unconditional header writes per frame.
// Returns true when an epoch transition (reattach) happened.
func (r *bridgeRingFile) ensureAttached() bool {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return false
	}
	cur := binary.LittleEndian.Uint64(r.data[bridgeRingGenerationOffset : bridgeRingGenerationOffset+8])
	if r.attachedGeneration == 0 || (cur != 0 && cur != r.attachedGeneration) {
		r.attach()
		return r.attachedGeneration != 0
	}
	if atomic.LoadUint32((*uint32)(unsafe.Pointer(&r.data[r.localAttachOff]))) == 0 {
		atomic.StoreUint32((*uint32)(unsafe.Pointer(&r.data[r.localAttachOff])), 1)
	}
	return false
}

func (r *bridgeRingFile) peerReady() bool {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return false
	}
	flags := binary.LittleEndian.Uint32(r.data[bridgeRingFlagsOffset : bridgeRingFlagsOffset+4])
	attached := atomic.LoadUint32((*uint32)(unsafe.Pointer(&r.data[r.peerAttachOff])))
	return (flags&r.peerReadyFlag) != 0 && attached != 0
}

func (r *bridgeRingFile) markDetached() {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return
	}
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&r.data[r.localAttachOff])), 0)
}

func (r *bridgeRingFile) slotOffset(seq uint64) int {
	idx := seq % uint64(r.capacity)
	return bridgeRingHeaderSize + int(idx)*int(r.slotSize)
}

func (r *bridgeRingFile) notifyPtr() *uint32 {
	if r == nil || len(r.data) < bridgeRingNotifyOffset+4 {
		return nil
	}
	return (*uint32)(unsafe.Pointer(&r.data[bridgeRingNotifyOffset]))
}

func (r *bridgeRingFile) notifySeq() uint32 {
	ptr := r.notifyPtr()
	if ptr == nil {
		return 0
	}
	return atomic.LoadUint32(ptr)
}

func (r *bridgeRingFile) hasReadable() bool {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return false
	}
	writeSeq := binary.LittleEndian.Uint64(r.data[bridgeRingWriteSeqOffset : bridgeRingWriteSeqOffset+8])
	readSeq := binary.LittleEndian.Uint64(r.data[bridgeRingReadSeqOffset : bridgeRingReadSeqOffset+8])
	return readSeq < writeSeq
}

func (r *bridgeRingFile) hasWritable() bool {
	if r == nil || len(r.data) < bridgeRingHeaderSize {
		return false
	}
	capacity := binary.LittleEndian.Uint32(r.data[8:12])
	writeSeq := binary.LittleEndian.Uint64(r.data[bridgeRingWriteSeqOffset : bridgeRingWriteSeqOffset+8])
	readSeq := binary.LittleEndian.Uint64(r.data[bridgeRingReadSeqOffset : bridgeRingReadSeqOffset+8])
	return capacity > 0 && writeSeq-readSeq < uint64(capacity)
}

func (r *bridgeRingFile) bumpNotify() {
	ptr := r.notifyPtr()
	if ptr == nil {
		return
	}
	atomic.AddUint32(ptr, 1)
	ringWakeUint32(ptr)
}

func (r *bridgeRingFile) waitForChange(done <-chan struct{}, observed uint32, timeout time.Duration) {
	ptr := r.notifyPtr()
	if ptr == nil {
		sleepOrDone(done, timeout)
		return
	}
	if atomic.LoadUint32(ptr) != observed {
		return
	}
	if !ringNotifyHasKernelWake() && timeout > 200*time.Microsecond {
		timeout = 200 * time.Microsecond
	}
	ringWaitUint32(ptr, observed, timeout)
	select {
	case <-done:
	default:
	}
}

func sleepOrDone(done <-chan struct{}, timeout time.Duration) {
	if timeout <= 0 {
		return
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case <-done:
	case <-timer.C:
	}
}

func encodeRingOpenPayload(proto byte, req coremodel.OpenRequest) ([]byte, error) {
	return buildRingOpenBody(proto, req)
}

func decodeRingStatus(payload []byte) error {
	if len(payload) < 3 {
		return errors.New("shared-ring status too short")
	}
	ok := payload[0] == 1
	msgLen := int(binary.BigEndian.Uint16(payload[1:3]))
	if len(payload) < 3+msgLen {
		return errors.New("shared-ring status truncated")
	}
	if ok {
		return nil
	}
	msg := strings.TrimSpace(string(payload[3 : 3+msgLen]))
	if msg == "" {
		msg = "xgw shared-ring bridge rejected"
	}
	return errors.New(msg)
}

func (c *sharedRingConn) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	for c.readFrame == nil || c.readOff >= len(c.readFrame.data) {
		if c.readFrame != nil {
			releaseRingFrame(c.readFrame)
			c.readFrame = nil
			c.readOff = 0
		}
		timeout := c.readTimeout()
		var timer <-chan time.Time
		if timeout > 0 {
			timer = time.After(timeout)
		}
		select {
		case frame, ok := <-c.readCh:
			if !ok {
				return 0, io.EOF
			}
			if frame == nil || len(frame.data) == 0 {
				releaseRingFrame(frame)
				continue
			}
			c.readFrame = frame
			c.readOff = 0
		case <-timer:
			return 0, os.ErrDeadlineExceeded
		}
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

func (c *sharedRingConn) Write(p []byte) (int, error) {
	if c.proto == bridgeProtoUDP {
		if err := c.writeUDPFrontBytes(p); err != nil {
			return 0, err
		}
		return len(p), nil
	}
	written := 0
	for written < len(p) {
		n, err := c.transport.writeDataFrames(bridgeRingKindTCPData, c.id, p[written:])
		if err != nil {
			return written, err
		}
		written += n
	}
	return len(p), nil
}

func (c *sharedRingConn) Close() error {
	var err error
	c.closeOnce.Do(func() {
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		err = c.transport.writeFrameCtx(ctx, bridgeRingKindClose, c.id, nil)
		c.transport.mu.Lock()
		delete(c.transport.conns, c.id)
		c.transport.mu.Unlock()
		c.closeLocal()
	})
	return err
}

func (c *sharedRingConn) CloseWrite() error {
	if c.proto != bridgeProtoTCP {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	return c.transport.writeFrameCtx(ctx, bridgeRingKindHalfClose, c.id, nil)
}

func (c *sharedRingConn) LocalAddr() net.Addr  { return bridgeAddr("shared-ring") }
func (c *sharedRingConn) RemoteAddr() net.Addr { return bridgeAddr(c.target) }

func (c *sharedRingConn) SetDeadline(t time.Time) error {
	c.deadline.Store(t)
	return nil
}

func (c *sharedRingConn) SetReadDeadline(t time.Time) error {
	c.deadline.Store(t)
	return nil
}

func (c *sharedRingConn) SetWriteDeadline(time.Time) error { return nil }

// deliver hands a downstream frame to the consumer. It blocks (with done
// cancellation) instead of dropping on a full readCh: dropping a data frame
// here truncates the TCP byte stream, the exact bootstrap-breaking failure we
// must avoid. Backpressure flows back to the ring reader instead of spinning on
// a local sleep retry.
func (c *sharedRingConn) deliver(frame *pooledRingFrame) (ok bool) {
	if c.closed.Load() {
		releaseRingFrame(frame)
		return false
	}
	delivered := false
	defer func() {
		if recover() != nil && !delivered {
			releaseRingFrame(frame)
			ok = false
		}
	}()
	select {
	case c.readCh <- frame:
		delivered = true
		return true
	case <-c.done:
		releaseRingFrame(frame)
		return false
	}
}

func (c *sharedRingConn) closeLocal() {
	c.localOnce.Do(func() {
		c.closed.Store(true)
		close(c.done)
		c.readCloseOnce.Do(func() {
			close(c.readCh)
		})
	})
}

// closeReadHalf delivers downstream EOF without tearing down the write half,
// used when the peer signals a TCP half-close so the phone-facing side gets a
// clean FIN once buffered frames drain.
func (c *sharedRingConn) closeReadHalf() {
	c.readCloseOnce.Do(func() {
		close(c.readCh)
	})
}

func (c *sharedRingConn) readTimeout() time.Duration {
	v := c.deadline.Load()
	if v == nil {
		return 0
	}
	deadline, ok := v.(time.Time)
	if !ok || deadline.IsZero() {
		return 0
	}
	left := time.Until(deadline)
	if left <= 0 {
		return time.Nanosecond
	}
	return left
}

func (c *sharedRingConn) writeUDPFrontBytes(p []byte) error {
	c.udpRx = append(c.udpRx, p...)
	for {
		if len(c.udpRx) < 4 {
			return nil
		}
		addrLen := int(binary.BigEndian.Uint16(c.udpRx[:2]))
		if addrLen == 0 || addrLen > 65535 {
			return errors.New("invalid shared-ring udp addr length")
		}
		if len(c.udpRx) < 4+addrLen {
			return nil
		}
		dataLen := int(binary.BigEndian.Uint16(c.udpRx[2+addrLen : 4+addrLen]))
		total := 4 + addrLen + dataLen
		if len(c.udpRx) < total {
			return nil
		}
		if err := c.transport.writeFrame(bridgeRingKindUDPData, c.id, c.udpRx[:total]); err != nil {
			return err
		}
		copy(c.udpRx, c.udpRx[total:])
		c.udpRx = c.udpRx[:len(c.udpRx)-total]
	}
}

type bridgeAddr string

func (a bridgeAddr) Network() string { return "xgw-bridge" }
func (a bridgeAddr) String() string  { return string(a) }
