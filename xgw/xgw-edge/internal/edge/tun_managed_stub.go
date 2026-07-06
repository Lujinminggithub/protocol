//go:build !linux

package edge

import (
	"context"
	"errors"
)

func (m *ManagedClient) localTUNProxy(ctx context.Context, tunName string, tunCIDR string) error {
	_ = ctx
	_ = tunName
	_ = tunCIDR
	return errors.New("managed tun proxy is only implemented for linux")
}
