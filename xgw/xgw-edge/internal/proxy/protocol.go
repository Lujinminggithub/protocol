package proxy

import (
	"encoding/binary"
	"errors"
	"io"
)

const (
	FrameTypeTCPRequest uint64 = 0x401
	MaxAddressLength           = 2048
	MaxMessageLength           = 2048
)

func writeVarint(buf []byte, v uint64) int {
	switch {
	case v <= 63:
		buf[0] = byte(v)
		return 1
	case v <= 16383:
		buf[0] = byte(v>>8) | 0x40
		buf[1] = byte(v)
		return 2
	case v <= 1073741823:
		buf[0] = byte(v>>24) | 0x80
		buf[1] = byte(v >> 16)
		buf[2] = byte(v >> 8)
		buf[3] = byte(v)
		return 4
	default:
		buf[0] = byte(v>>56) | 0xc0
		buf[1] = byte(v >> 48)
		buf[2] = byte(v >> 40)
		buf[3] = byte(v >> 32)
		buf[4] = byte(v >> 24)
		buf[5] = byte(v >> 16)
		buf[6] = byte(v >> 8)
		buf[7] = byte(v)
		return 8
	}
}

func readVarint(r io.Reader) (uint64, error) {
	var first [1]byte
	if _, err := io.ReadFull(r, first[:]); err != nil {
		return 0, err
	}
	prefix := first[0] >> 6
	var n int
	switch prefix {
	case 0:
		return uint64(first[0] & 0x3f), nil
	case 1:
		n = 2
	case 2:
		n = 4
	default:
		n = 8
	}
	buf := make([]byte, n)
	buf[0] = first[0] & 0x3f
	if _, err := io.ReadFull(r, buf[1:]); err != nil {
		return 0, err
	}
	var out uint64
	for _, b := range buf {
		out = (out << 8) | uint64(b)
	}
	return out, nil
}

func ReadTCPRequestAny(r io.Reader) (string, error) {
	first, err := readVarint(r)
	if err != nil {
		return "", err
	}
	if first == FrameTypeTCPRequest {
		return ReadTCPRequestPayload(r)
	}
	if first == 0 || first > MaxAddressLength {
		return "", errors.New("invalid tcp request prefix")
	}
	buf := make([]byte, first)
	if _, err := io.ReadFull(r, buf); err != nil {
		return "", err
	}
	return string(buf), nil
}

func WriteTCPRequest(w io.Writer, addr string) error {
	if len(addr) == 0 || len(addr) > MaxAddressLength {
		return errors.New("invalid target addr")
	}
	buf := make([]byte, 16+len(addr))
	n := writeVarint(buf, FrameTypeTCPRequest)
	n += writeVarint(buf[n:], uint64(len(addr)))
	copy(buf[n:], addr)
	n += len(addr)
	_, err := w.Write(buf[:n])
	return err
}

func ReadTCPRequest(r io.Reader) (string, error) {
	frameType, err := readVarint(r)
	if err != nil {
		return "", err
	}
	if frameType != FrameTypeTCPRequest {
		return "", errors.New("unexpected frame type")
	}
	addrLen, err := readVarint(r)
	if err != nil {
		return "", err
	}
	if addrLen == 0 || addrLen > MaxAddressLength {
		return "", errors.New("invalid address length")
	}
	buf := make([]byte, addrLen)
	if _, err := io.ReadFull(r, buf); err != nil {
		return "", err
	}
	return string(buf), nil
}

func ReadTCPRequestPayload(r io.Reader) (string, error) {
	addrLen, err := readVarint(r)
	if err != nil {
		return "", err
	}
	if addrLen == 0 || addrLen > MaxAddressLength {
		return "", errors.New("invalid address length")
	}
	buf := make([]byte, addrLen)
	if _, err := io.ReadFull(r, buf); err != nil {
		return "", err
	}
	return string(buf), nil
}

func WriteTCPResponse(w io.Writer, ok bool, msg string) error {
	if len(msg) > MaxMessageLength {
		return errors.New("message too large")
	}
	buf := make([]byte, 16+len(msg))
	if ok {
		buf[0] = 0
	} else {
		buf[0] = 1
	}
	n := 1
	n += writeVarint(buf[n:], uint64(len(msg)))
	copy(buf[n:], msg)
	n += len(msg)
	_, err := w.Write(buf[:n])
	return err
}

func ReadTCPResponse(r io.Reader) (bool, string, error) {
	var status [1]byte
	if _, err := io.ReadFull(r, status[:]); err != nil {
		return false, "", err
	}
	msgLen, err := readVarint(r)
	if err != nil {
		return false, "", err
	}
	if msgLen > MaxMessageLength {
		return false, "", errors.New("invalid response message length")
	}
	buf := make([]byte, msgLen)
	if _, err := io.ReadFull(r, buf); err != nil {
		return false, "", err
	}
	return status[0] == 0, string(buf), nil
}

func SerializeUDPMessage(sessionID uint32, packetID uint16, addr string, data []byte) []byte {
	buf := make([]byte, 8+16+len(addr)+len(data))
	binary.BigEndian.PutUint32(buf[0:], sessionID)
	binary.BigEndian.PutUint16(buf[4:], packetID)
	buf[6] = 0
	buf[7] = 1
	n := 8
	n += writeVarint(buf[n:], uint64(len(addr)))
	copy(buf[n:], addr)
	n += len(addr)
	copy(buf[n:], data)
	n += len(data)
	return buf[:n]
}
