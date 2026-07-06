package native

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/config"
	"github.com/local/xgw-edge/internal/coremodel"
)

// ringDebug gates the verbose shared-ring trace prints (sharedring.*). They are
// per-connection-open hot-path logs that write straight to stdout; left always
// on they peg CPU and stall the data path under load. Enable with
// XGW_RING_DEBUG=1 when diagnosing the bridge.
var ringDebug = os.Getenv("XGW_RING_DEBUG") != ""

func ringTracef(format string, args ...any) {
	if ringDebug {
		fmt.Printf(format, args...)
	}
}


const (
	connectTypeBridge = "bridge"
	connectTypeDirect = "direct"

	bridgeTransportTCP  = "tcp"
	bridgeTransportUnix = "unix"
	bridgeTransportRing = "shared-ring"
	bridgeTransportSHM  = "shm-direct"

	bridgeProtoTCP byte = 1
	bridgeProtoUDP byte = 2
)

var bridgeCopyBufPool = sync.Pool{
	New: func() any {
		buf := make([]byte, 64<<10)
		return &buf
	},
}

// BridgeTransport is the Go-side local adapter contract.
// Different implementations may use TCP socket, shared ring, shared memory, or
// direct call in the future, but they must all carry the same canonical
// OpenRequest into the backend core.
type BridgeTransport interface {
	OpenTCP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error)
	OpenUDP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error)
	Close() error
}

// LocalAdapter is the implementation-facing ABI boundary under BridgeTransport.
// Current socket/ring bridges still expose net.Conn for compatibility, while a
// future direct/shared-memory adapter can satisfy the same semantic contract
// without preserving the transport details.
type LocalAdapter interface {
	OpenTCP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error)
	OpenUDP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error)
	Close() error
}

type socketBridgeTransport struct {
	transport string
	tcpAddr   string
	unixPath  string
}

func NewBridgeTransport(cfg config.ServerConfig) (BridgeTransport, error) {
	return newBridgeTransport(cfg)
}

func newBridgeTransport(cfg config.ServerConfig) (LocalAdapter, error) {
	transport := strings.TrimSpace(cfg.BridgeTransport)
	switch transport {
	case "", bridgeTransportTCP:
		if cfg.BridgeTCPAddr == "" {
			return nil, errors.New("bridge_tcp_addr is required when bridge_transport=tcp")
		}
		return &socketBridgeTransport{transport: bridgeTransportTCP, tcpAddr: cfg.BridgeTCPAddr}, nil
	case bridgeTransportUnix:
		if cfg.BridgeUnixPath == "" {
			return nil, errors.New("bridge_unix_path is required when bridge_transport=unix")
		}
		return &socketBridgeTransport{transport: bridgeTransportUnix, unixPath: cfg.BridgeUnixPath}, nil
	case bridgeTransportRing, "shared_ring", "ring":
		if cfg.BridgeRingPath == "" {
			return nil, errors.New("bridge_ring_path is required when bridge_transport=shared-ring")
		}
		return newSharedRingBridgeTransport(cfg.BridgeRingPath)
	case bridgeTransportSHM, "shm_direct", "shared-memory-direct":
		if cfg.BridgeRingPath == "" {
			return nil, errors.New("bridge_ring_path is required when bridge_transport=shm-direct")
		}
		return newShmDirectAdapter(cfg.BridgeRingPath)
	default:
		return nil, fmt.Errorf("unsupported bridge_transport %q", transport)
	}
}

func (t *socketBridgeTransport) OpenTCP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error) {
	_ = ctx
	return dialBridgeForTCP(t.transport, t.tcpAddr, t.unixPath, req)
}

func (t *socketBridgeTransport) OpenUDP(ctx context.Context, req coremodel.OpenRequest) (net.Conn, error) {
	_ = ctx
	return dialBridgeForUDP(t.transport, t.tcpAddr, t.unixPath, req)
}

func (t *socketBridgeTransport) Close() error { return nil }

func dialBridgeTCP(bridgeAddr string, target string) (net.Conn, error) {
	conn, err := dialBridgeConn(bridgeTransportTCP, bridgeAddr, "", target, false)
	if err != nil {
		return nil, err
	}
	if err := writeBridgeRequest(conn, coremodel.OpenRequest{
		Flow:    coremodel.ClassifyFlow(coremodel.FlowKindTCP, target),
		Session: coremodel.DefaultSessionSemantic("legacy-bridge"),
	}); err != nil {
		_ = conn.Close()
		return nil, err
	}
	if err := readBridgeStatus(conn); err != nil {
		_ = conn.Close()
		return nil, err
	}
	return conn, nil
}

func dialBridgeForTCP(transport string, tcpAddr string, unixPath string, req coremodel.OpenRequest) (net.Conn, error) {
	conn, err := dialBridgeConn(transport, tcpAddr, unixPath, req.Flow.Target, false)
	if err != nil {
		return nil, err
	}
	if err := writeBridgeRequest(conn, req); err != nil {
		_ = conn.Close()
		return nil, err
	}
	if err := readBridgeStatus(conn); err != nil {
		_ = conn.Close()
		return nil, err
	}
	return conn, nil
}

func dialBridgeForUDP(transport string, tcpAddr string, unixPath string, req coremodel.OpenRequest) (net.Conn, error) {
	conn, err := dialBridgeConn(transport, tcpAddr, unixPath, req.Flow.Target, true)
	if err != nil {
		return nil, err
	}
	if err := writeBridgeExtendedRequest(conn, bridgeProtoUDP, req); err != nil {
		_ = conn.Close()
		return nil, err
	}
	if err := readBridgeStatus(conn); err != nil {
		_ = conn.Close()
		return nil, err
	}
	return conn, nil
}

func dialBridgeConn(transport string, tcpAddr string, unixPath string, target string, udp bool) (net.Conn, error) {
	_ = target
	_ = udp
	switch transport {
	case "", bridgeTransportTCP:
		return net.DialTimeout("tcp", tcpAddr, defaultDialTimeout)
	case bridgeTransportUnix:
		dialer := net.Dialer{Timeout: defaultDialTimeout, KeepAlive: 30 * time.Second}
		return dialer.Dial("unix", unixPath)
	default:
		return nil, fmt.Errorf("unsupported bridge_transport %q", transport)
	}
}

func writeBridgeRequest(conn net.Conn, req coremodel.OpenRequest) error {
	body, err := buildBridgeOpenBody(bridgeProtoTCP, req, false)
	if err != nil {
		return err
	}
	_, err = conn.Write(body)
	return err
}

func writeBridgeExtendedRequest(conn net.Conn, proto byte, req coremodel.OpenRequest) error {
	body, err := buildBridgeOpenBody(proto, req, true)
	if err != nil {
		return err
	}
	_, err = conn.Write(body)
	return err
}

func readBridgeStatus(conn net.Conn) error {
	head := make([]byte, 3)
	if _, err := io.ReadFull(conn, head); err != nil {
		return err
	}
	msgLen := int(binary.BigEndian.Uint16(head[1:3]))
	msg := make([]byte, msgLen)
	if msgLen > 0 {
		if _, err := io.ReadFull(conn, msg); err != nil {
			return err
		}
	}
	if head[0] == 0 {
		if msgLen == 0 {
			return errors.New("xgw bridge rejected")
		}
		return errors.New(string(msg))
	}
	return nil
}

func writeBridgeDatagram(conn net.Conn, addr string, data []byte) error {
	if len(addr) == 0 || len(addr) > 65535 {
		return errors.New("invalid udp bridge addr length")
	}
	if len(data) > 65535 {
		return errors.New("udp bridge datagram too large")
	}
	frame := make([]byte, 2+len(addr)+2+len(data))
	binary.BigEndian.PutUint16(frame[:2], uint16(len(addr)))
	copy(frame[2:], addr)
	binary.BigEndian.PutUint16(frame[2+len(addr):], uint16(len(data)))
	copy(frame[4+len(addr):], data)
	_, err := conn.Write(frame)
	return err
}

func readBridgeDatagram(conn net.Conn) (string, []byte, error) {
	head := make([]byte, 2)
	if _, err := io.ReadFull(conn, head); err != nil {
		return "", nil, err
	}
	addrLen := int(binary.BigEndian.Uint16(head))
	if addrLen == 0 || addrLen > 65535 {
		return "", nil, errors.New("invalid udp bridge addr length")
	}
	addr := make([]byte, addrLen)
	if _, err := io.ReadFull(conn, addr); err != nil {
		return "", nil, err
	}
	lenBuf := make([]byte, 2)
	if _, err := io.ReadFull(conn, lenBuf); err != nil {
		return "", nil, err
	}
	dataLen := int(binary.BigEndian.Uint16(lenBuf))
	data := make([]byte, dataLen)
	if dataLen > 0 {
		if _, err := io.ReadFull(conn, data); err != nil {
			return "", nil, err
		}
	}
	return string(addr), data, nil
}

func copyWithBridgePool(dst io.Writer, src io.Reader) (int64, error) {
	bufp := bridgeCopyBufPool.Get().(*[]byte)
	defer bridgeCopyBufPool.Put(bufp)
	return io.CopyBuffer(dst, src, *bufp)
}
