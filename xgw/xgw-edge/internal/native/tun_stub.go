//go:build !linux

package native

import (
	"context"
	"errors"
)

func (m *ManagedClient) LocalTUNProxy(ctx context.Context, tunName string, tunCIDR string) error {
	_ = ctx
	_ = tunName
	_ = tunCIDR
	return errors.New("native tun proxy is only implemented for linux")
}
