//go:build !windows && !linux

package native

import (
	"os"
	"sync/atomic"
	"time"

	"golang.org/x/sys/unix"
)

func mapBridgeRingFile(file *os.File, size int64) ([]byte, error) {
	return unix.Mmap(int(file.Fd()), 0, int(size), unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
}

func unmapBridgeRingFile(data []byte) error {
	return unix.Munmap(data)
}

// syncBridgeRingMapping is intentionally a no-op; see the linux variant for the
// rationale. MAP_SHARED stores are visible to the peer without msync, and the C
// peer never calls msync. Forcing MS_SYNC per frame here flushed the whole
// mapping per packet and collapsed throughput.
func syncBridgeRingMapping(data []byte) error {
	_ = data
	return nil
}

func ringWaitUint32(addr *uint32, observed uint32, timeout time.Duration) {
	if atomic.LoadUint32(addr) != observed {
		return
	}
	time.Sleep(timeout)
}

func ringWakeUint32(*uint32) {}

func ringNotifyHasKernelWake() bool { return false }
