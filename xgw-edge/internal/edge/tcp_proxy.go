package edge

import (
	"context"
	"errors"
	"io"
	"net"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/proxy"
	"github.com/quic-go/quic-go"
)

func (s *ClientSession) DialTCP(ctx context.Context, target string) (net.Conn, error) {
	safeTarget := target
	stream, err := s.Conn.OpenStreamSync(ctx)
	if err != nil {
		return nil, err
	}
	if err := proxy.WriteTCPRequest(stream, target); err != nil {
		_ = stream.Close()
		return nil, err
	}
	ok, msg, err := proxy.ReadTCPResponse(stream)
	if err != nil {
		_ = stream.Close()
		return nil, err
	}
	if !ok {
		_ = stream.Close()
		if msg == "" {
			msg = "tcp proxy rejected"
		}
		return nil, errors.New(msg)
	}
	_ = safeTarget
	return &quicTCPConn{stream: stream, local: s.Conn.LocalAddr(), remote: s.Conn.RemoteAddr()}, nil
}

type quicTCPConn struct {
	stream *quic.Stream
	local  net.Addr
	remote net.Addr
}

func (c *quicTCPConn) Read(p []byte) (int, error)  { return c.stream.Read(p) }
func (c *quicTCPConn) Write(p []byte) (int, error) { return c.stream.Write(p) }
func (c *quicTCPConn) Close() error {
	c.stream.CancelRead(0)
	return c.stream.Close()
}

// CloseWrite half-closes the QUIC stream's send side (sends STREAM FIN) while
// leaving the receive side open, so an upstream EOF propagates as a clean FIN.
func (c *quicTCPConn) CloseWrite() error { return c.stream.Close() }
func (c *quicTCPConn) LocalAddr() net.Addr                { return c.local }
func (c *quicTCPConn) RemoteAddr() net.Addr               { return c.remote }
func (c *quicTCPConn) SetDeadline(t time.Time) error      { return c.stream.SetDeadline(t) }
func (c *quicTCPConn) SetReadDeadline(t time.Time) error  { return c.stream.SetReadDeadline(t) }
func (c *quicTCPConn) SetWriteDeadline(t time.Time) error { return c.stream.SetWriteDeadline(t) }

func proxyTwoWay(a net.Conn, b net.Conn) {
	type halfCloser interface{ CloseWrite() error }
	var wg sync.WaitGroup
	copyHalf := func(dst net.Conn, src net.Conn) {
		defer wg.Done()
		_, _ = io.Copy(dst, src)
		// Propagate the source EOF as a clean FIN to the destination's write
		// half instead of slamming the whole connection shut. HTTP responses
		// whose body ends at connection close (chunked tail / Connection:
		// close) rely on a real FIN; a hard reset is seen as TLS truncation
		// and makes clients like the TikTok bootstrap treat the response as
		// failed. Fall back to a deadline poke only when half-close is
		// unsupported.
		if hc, ok := dst.(halfCloser); ok {
			_ = hc.CloseWrite()
		} else {
			_ = dst.SetDeadline(time.Now())
		}
	}
	wg.Add(2)
	go copyHalf(a, b)
	go copyHalf(b, a)
	wg.Wait()
	_ = a.Close()
	_ = b.Close()
}
