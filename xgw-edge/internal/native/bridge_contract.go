package native

import (
	"encoding/binary"
	"errors"
	"net"

	"github.com/local/xgw-edge/internal/coremodel"
)

const (
	bridgeMetaLen        = 12
	bridgeSessionMetaLen = 100
)

func buildBridgeMeta(flow coremodel.FlowRequest, session coremodel.SessionSemantic) []byte {
	meta := make([]byte, bridgeMetaLen+bridgeSessionMetaLen)
	meta[0] = flow.WireClass()
	meta[1] = flow.WirePriority()
	meta[2] = flow.WireBudgetFlags()
	meta[3] = flow.WirePreferredCopies()
	binary.BigEndian.PutUint32(meta[4:8], flow.WireReadTimeoutMs())
	binary.BigEndian.PutUint32(meta[8:12], flow.WireIdleAfterFirstByteMs())
	copy(meta[12:52], []byte(session.SessionID))
	copy(meta[52:68], []byte(session.Frontend))
	copy(meta[68:92], []byte(session.RouteName))
	copy(meta[92:108], []byte(session.LineID))
	return meta
}

func buildBridgeOpenBody(proto byte, req coremodel.OpenRequest, udpExtended bool) ([]byte, error) {
	host, portText, err := net.SplitHostPort(req.Flow.Target)
	if err != nil {
		return nil, err
	}
	portAddr, err := net.ResolveTCPAddr("tcp", net.JoinHostPort("127.0.0.1", portText))
	if err != nil || portAddr == nil {
		return nil, errors.New("resolve target port failed")
	}
	if len(host) == 0 || len(host) > 65535 {
		return nil, errors.New("invalid target host length")
	}
	meta := buildBridgeMeta(req.Flow, req.Session)
	if udpExtended {
		body := make([]byte, 2+1+2+len(host)+2+len(meta))
		binary.BigEndian.PutUint16(body[:2], 0)
		body[2] = proto
		binary.BigEndian.PutUint16(body[3:5], uint16(len(host)))
		copy(body[5:], host)
		binary.BigEndian.PutUint16(body[5+len(host):], uint16(portAddr.Port))
		copy(body[5+len(host)+2:], meta)
		return body, nil
	}
	body := make([]byte, 2+len(host)+2+len(meta))
	binary.BigEndian.PutUint16(body[:2], uint16(len(host)))
	copy(body[2:], host)
	binary.BigEndian.PutUint16(body[2+len(host):], uint16(portAddr.Port))
	copy(body[2+len(host)+2:], meta)
	return body, nil
}

func buildRingOpenBody(proto byte, req coremodel.OpenRequest) ([]byte, error) {
	host, portText, err := net.SplitHostPort(req.Flow.Target)
	if err != nil {
		return nil, err
	}
	portAddr, err := net.ResolveTCPAddr("tcp", net.JoinHostPort("127.0.0.1", portText))
	if err != nil || portAddr == nil {
		return nil, errors.New("resolve target port failed")
	}
	if len(host) == 0 || len(host) > 65535 {
		return nil, errors.New("invalid target host length")
	}
	meta := buildBridgeMeta(req.Flow, req.Session)
	body := make([]byte, 1+2+len(host)+2+len(meta))
	body[0] = proto
	binary.BigEndian.PutUint16(body[1:3], uint16(len(host)))
	copy(body[3:], host)
	binary.BigEndian.PutUint16(body[3+len(host):], uint16(portAddr.Port))
	copy(body[3+len(host)+2:], meta)
	return body, nil
}
