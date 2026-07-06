package proxy

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

type UDPMessage struct {
	SessionID uint32
	PacketID  uint16
	FragID    uint8
	FragCount uint8
	Addr      string
	Data      []byte
}

func ParseUDPMessage(msg []byte) (*UDPMessage, error) {
	m := &UDPMessage{}
	buf := bytes.NewBuffer(msg)
	if err := binary.Read(buf, binary.BigEndian, &m.SessionID); err != nil {
		return nil, err
	}
	if err := binary.Read(buf, binary.BigEndian, &m.PacketID); err != nil {
		return nil, err
	}
	if err := binary.Read(buf, binary.BigEndian, &m.FragID); err != nil {
		return nil, err
	}
	if err := binary.Read(buf, binary.BigEndian, &m.FragCount); err != nil {
		return nil, err
	}
	lAddr, err := readVarint(buf)
	if err != nil {
		return nil, err
	}
	if lAddr == 0 || lAddr > MaxMessageLength {
		return nil, errors.New("invalid udp addr length")
	}
	bs := buf.Bytes()
	if len(bs) <= int(lAddr) {
		return nil, errors.New("invalid udp message length")
	}
	m.Addr = string(bs[:lAddr])
	m.Data = append([]byte(nil), bs[lAddr:]...)
	return m, nil
}

func (m *UDPMessage) Serialize() []byte {
	return SerializeUDPMessage(m.SessionID, m.PacketID, m.Addr, m.Data)
}

func BuildUDPResponse(sessionID uint32, packetID uint16, addr string, data []byte) []byte {
	return SerializeUDPMessage(sessionID, packetID, addr, data)
}

func FormatAddr(host string, port int) string {
	return fmt.Sprintf("%s:%d", host, port)
}

func ReadRequestMaybeHysteria(r io.Reader) (string, error) {
	return ReadTCPRequestAny(r)
}
