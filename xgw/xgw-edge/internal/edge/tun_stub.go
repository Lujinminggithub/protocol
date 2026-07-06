//go:build !linux

package edge

import (
	"context"
	"errors"
)

func (s *ClientSession) LocalTUNProxy(ctx context.Context, tunName string, tunCIDR string) error {
	_ = ctx
	_ = tunName
	_ = tunCIDR
	return errors.New("tun proxy is only implemented for linux in xgw-edge for now")
}
