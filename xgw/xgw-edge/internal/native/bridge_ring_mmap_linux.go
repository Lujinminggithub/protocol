//go:build linux

package native

import (
	"os"
	"sync/atomic"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	linuxFutexWait = 0
	linuxFutexWake = 1
)

func mapBridgeRingFile(file *os.File, size int64) ([]byte, error) {
	return unix.Mmap(int(file.Fd()), 0, int(size), unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
}

func unmapBridgeRingFile(data []byte) error {
	return unix.Munmap(data)
}

// syncBridgeRingMapping is intentionally a no-op for the shared-ring IPC path.
// The ring lives in a MAP_SHARED mapping, so stores are visible to the peer
// process immediately without msync; msync only forces disk write-back. The C
// peer (src/bridge.c) never calls msync, and calling MS_SYNC on every frame
// here flushed the whole ~64MB mapping per packet, pegging CPU and collapsing
// throughput. Cross-process ordering is provided by the write_seq publish +
// futex notify, not by msync.
func syncBridgeRingMapping(data []byte) error {
	_ = data
	return nil
}

func ringWaitUint32(addr *uint32, observed uint32, timeout time.Duration) {
	if atomic.LoadUint32(addr) != observed {
		return
	}
	ts := unix.NsecToTimespec(timeout.Nanoseconds())
	_, _, errno := unix.Syscall6(
		unix.SYS_FUTEX,
		uintptr(unsafe.Pointer(addr)),
		uintptr(linuxFutexWait),
		uintptr(observed),
		uintptr(unsafe.Pointer(&ts)),
		0,
		0,
	)
	if errno == unix.EAGAIN || errno == unix.ETIMEDOUT || errno == unix.EINTR {
		return
	}
}

func ringWakeUint32(addr *uint32) {
	_, _, _ = unix.Syscall6(
		unix.SYS_FUTEX,
		uintptr(unsafe.Pointer(addr)),
		uintptr(linuxFutexWake),
		1,
		0,
		0,
		0,
	)
}

func ringNotifyHasKernelWake() bool { return true }
