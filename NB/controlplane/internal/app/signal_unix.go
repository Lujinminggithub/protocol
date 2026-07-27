//go:build !windows

package app

import (
	"os"
	"syscall"
)

func reloadSignal() os.Signal { return syscall.SIGHUP }
