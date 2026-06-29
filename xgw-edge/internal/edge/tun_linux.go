//go:build linux

package edge

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

func (s *ClientSession) LocalTUNProxy(ctx context.Context, tunName string, tunCIDR string) error {
	return localTUNProxySession(ctx, s, tunName, tunCIDR)
}

func (m *ManagedClient) localTUNProxy(ctx context.Context, tunName string, tunCIDR string) error {
	fd, name, err := openTUN(tunName)
	if err != nil {
		m.logger.Errorf("managed tun open failed name=%s err=%v", tunName, err)
		return err
	}
	defer unix.Close(fd)

	if tunCIDR != "" {
		if err := configureTUN(name, tunCIDR); err != nil {
			m.logger.Errorf("managed tun configure failed name=%s cidr=%s err=%v", name, tunCIDR, err)
			return err
		}
	}
	m.logger.Infof("managed tun ready name=%s cidr=%s", name, tunCIDR)
	go func() {
		for {
			msg, err := m.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			_, _ = unix.Write(fd, msg)
		}
	}()

	buf := make([]byte, 64<<10)
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		_ = unix.SetNonblock(fd, false)
		n, err := unix.Read(fd, buf)
		if err != nil {
			if err == unix.EINTR {
				continue
			}
			return err
		}
		if n <= 0 {
			time.Sleep(10 * time.Millisecond)
			continue
		}
		if err := m.SendDatagram(ctx, append([]byte(nil), buf[:n]...)); err != nil {
			return err
		}
	}
}

func localTUNProxySession(ctx context.Context, s *ClientSession, tunName string, tunCIDR string) error {
	fd, name, err := openTUN(tunName)
	if err != nil {
		return err
	}
	defer unix.Close(fd)

	if tunCIDR != "" {
		if err := configureTUN(name, tunCIDR); err != nil {
			return err
		}
	}

	go func() {
		buf := make([]byte, 64<<10)
		for {
			msg, err := s.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			copy(buf, msg)
			_, _ = unix.Write(fd, buf[:len(msg)])
		}
	}()

	buf := make([]byte, 64<<10)
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		_ = unix.SetNonblock(fd, false)
		n, err := unix.Read(fd, buf)
		if err != nil {
			if err == unix.EINTR {
				continue
			}
			return err
		}
		if n <= 0 {
			time.Sleep(10 * time.Millisecond)
			continue
		}
		if err := s.SendDatagram(ctx, append([]byte(nil), buf[:n]...)); err != nil {
			return err
		}
	}
}

func openTUN(requested string) (int, string, error) {
	fd, err := unix.Open("/dev/net/tun", os.O_RDWR, 0)
	if err != nil {
		return -1, "", err
	}
	var ifr [unix.IFNAMSIZ + 64]byte
	name := requested
	if name == "" {
		name = "xgwedge0"
	}
	copy(ifr[:unix.IFNAMSIZ], []byte(name))
	flags := uint16(unix.IFF_TUN | unix.IFF_NO_PI)
	ifr[unix.IFNAMSIZ] = byte(flags)
	ifr[unix.IFNAMSIZ+1] = byte(flags >> 8)
	_, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(unix.TUNSETIFF), uintptr(unsafe.Pointer(&ifr[0])))
	if errno != 0 {
		_ = unix.Close(fd)
		return -1, "", errno
	}
	actual := cString(ifr[:unix.IFNAMSIZ])
	return fd, actual, nil
}

func configureTUN(name string, cidr string) error {
	if err := exec.Command("ip", "link", "set", "dev", name, "up").Run(); err != nil {
		return fmt.Errorf("ip link up: %w", err)
	}
	if err := exec.Command("ip", "addr", "replace", cidr, "dev", name).Run(); err != nil {
		return fmt.Errorf("ip addr replace: %w", err)
	}
	return nil
}

func cString(buf []byte) string {
	n := 0
	for n < len(buf) && buf[n] != 0 {
		n++
	}
	return string(buf[:n])
}
