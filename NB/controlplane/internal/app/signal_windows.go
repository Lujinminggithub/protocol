//go:build windows

package app

import "os"

func reloadSignal() os.Signal { return os.Interrupt }
