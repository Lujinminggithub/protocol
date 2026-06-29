//go:build ignore

package edge

import (
	"context"
	"net"
	"sync"
	"time"

	"github.com/local/xgw-edge/internal/proxy"
	"github.com/quic-go/quic-go"
)

type hy2UDPSession struct {
	id          uint32
	originalAddr string
	targetAddr   string
	conn        *net.UDPConn
	lastSeen    time.Time
	closed      bool
}

type hy2UDPServer struct {
	conn        *quic.Conn
	logger      interface{ Infof(string, ...any); Warnf(string, ...any) }
	mu          sync.RWMutex
	sessions    map[uint32]*hy2UDPSession
	idleTimeout time.Duration
}

func newHY2UDPServer(conn *quic.Conn, logger interface{ Infof(string, ...any); Warnf(string, ...any) }, idleTimeout time.Duration) *hy2UDPServer {
	s := &hy2UDPServer{
		conn:        conn,
		logger:      logger,
		sessions:    make(map[uint32]*hy2UDPSession),
		idleTimeout: idleTimeout,
	}
	go s.cleanupLoop()
	return s
}

func (s *hy2UDPServer) cleanupLoop() {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		now := time.Now()
		var stale []*hy2UDPSession
		s.mu.RLock()
		for _, sess := range s.sessions {
			if now.Sub(sess.lastSeen) > s.idleTimeout {
				stale = append(stale, sess)
			}
		}
		s.mu.RUnlock()
		for _, sess := range stale {
			s.closeSession(sess.id, nil)
		}
	}
}

func (s *hy2UDPServer) closeSession(id uint32, err error) {
	s.mu.Lock()
	sess, ok := s.sessions[id]
	if ok {
		delete(s.sessions, id)
	}
	s.mu.Unlock()
	if !ok || sess.closed {
		return
	}
	sess.closed = true
	if sess.conn != nil {
		_ = sess.conn.Close()
	}
	if err != nil {
		s.logger.Warnf("hy2 udp session closed id=%d err=%v", id, err)
	} else {
		s.logger.Infof("hy2 udp session closed id=%d", id)
	}
}

func (s *hy2UDPServer) handleDatagram(msg []byte) error {
	udpMsg, err := proxy.ParseUDPMessage(msg)
	if err != nil {
		s.logger.Warnf("hy2 udp parse failed len=%d err=%v", len(msg), err)
		return err
	}
	s.logger.Infof("hy2 udp datagram recv session=%d packet=%d frag=%d/%d addr=%s data_len=%d",
		udpMsg.SessionID, udpMsg.PacketID, udpMsg.FragID, udpMsg.FragCount, udpMsg.Addr, len(udpMsg.Data))
	sess, err := s.ensureSession(udpMsg)
	if err != nil {
		return err
	}
	sess.lastSeen = time.Now()
	_, err = sess.conn.Write(udpMsg.Data)
	return err
}

func (s *hy2UDPServer) ensureSession(msg *proxy.UDPMessage) (*hy2UDPSession, error) {
	s.mu.RLock()
	existing := s.sessions[msg.SessionID]
	s.mu.RUnlock()
	if existing != nil {
		return existing, nil
	}

	target, err := net.ResolveUDPAddr("udp", msg.Addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.DialUDP("udp", nil, target)
	if err != nil {
		return nil, err
	}
	sess := &hy2UDPSession{
		id:           msg.SessionID,
		originalAddr: msg.Addr,
		targetAddr:   msg.Addr,
		conn:         conn,
		lastSeen:     time.Now(),
	}
	s.mu.Lock()
	s.sessions[msg.SessionID] = sess
	s.mu.Unlock()
	s.logger.Infof("hy2 udp session new id=%d target=%s", sess.id, sess.targetAddr)
	go s.receiveLoop(sess)
	return sess, nil
}

func (s *hy2UDPServer) receiveLoop(sess *hy2UDPSession) {
	buf := make([]byte, 64<<10)
	for {
		n, err := sess.conn.Read(buf)
		if err != nil {
			s.closeSession(sess.id, err)
			return
		}
		sess.lastSeen = time.Now()
		resp := (&proxy.UDPMessage{
			SessionID: sess.id,
			PacketID:  0,
			FragID:    0,
			FragCount: 1,
			Addr:      sess.originalAddr,
			Data:      append([]byte(nil), buf[:n]...),
		}).Serialize()
		s.logger.Infof("hy2 udp datagram send session=%d addr=%s data_len=%d", sess.id, sess.originalAddr, n)
		if err := s.conn.SendDatagram(resp); err != nil {
			s.closeSession(sess.id, err)
			return
		}
	}
}

func (s *hy2UDPServer) run(ctx context.Context) {
	for {
		msg, err := s.conn.ReceiveDatagram(ctx)
		if err != nil {
			return
		}
		if err := s.handleDatagram(msg); err != nil {
			s.logger.Warnf("hy2 udp datagram handle failed: %v", err)
		}
	}
}
