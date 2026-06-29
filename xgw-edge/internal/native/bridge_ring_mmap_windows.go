//go:build windows

package native

import (
	"os"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

func mapBridgeRingFile(file *os.File, size int64) ([]byte, error) {
	handle, err := syscall.CreateFileMapping(syscall.Handle(file.Fd()), nil, syscall.PAGE_READWRITE, uint32(size>>32), uint32(size), nil)
	if err != nil {
		return nil, err
	}
	defer syscall.CloseHandle(handle)
	addr, err := syscall.MapViewOfFile(handle, syscall.FILE_MAP_READ|syscall.FILE_MAP_WRITE, 0, 0, uintptr(size))
	if err != nil {
		return nil, err
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(addr)), int(size)), nil
}

func unmapBridgeRingFile(data []byte) error {
	if len(data) == 0 {
		return nil
	}
	return syscall.UnmapViewOfFile(uintptr(unsafe.Pointer(&data[0])))
}

// syncBridgeRingMapping is intentionally a no-op; see the linux variant for the
// rationale. The mapping is shared with the peer process, so views are coherent
// without flushing. FlushViewOfFile per frame forced disk write-back of the
// whole mapping and collapsed throughput.
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
