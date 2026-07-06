package native

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
)

const (
	ModeXGWNative = "xgw-native"
	DefaultALPN   = "xgw/1"

	streamKindAuth      byte = 1
	streamKindTCP       byte = 2
	streamKindKeepalive byte = 3
	streamKindRoute     byte = 4
)

const maxNativeJSON = 64 << 10
const maxNativeDatagram = 64 << 10

type Datagram struct {
	Target  string
	Payload []byte
}

func writeJSONMessage(w io.Writer, kind byte, value any) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if len(data) > maxNativeJSON {
		return errors.New("native json message too large")
	}
	head := [5]byte{kind}
	binary.BigEndian.PutUint32(head[1:], uint32(len(data)))
	if _, err := w.Write(head[:]); err != nil {
		return err
	}
	_, err = w.Write(data)
	return err
}

func readJSONMessage(r io.Reader, wantKind byte, value any) error {
	var head [5]byte
	if _, err := io.ReadFull(r, head[:]); err != nil {
		return err
	}
	if head[0] != wantKind {
		return errors.New("unexpected native stream kind")
	}
	msgLen := binary.BigEndian.Uint32(head[1:])
	if msgLen == 0 || msgLen > maxNativeJSON {
		return errors.New("invalid native json length")
	}
	data := make([]byte, msgLen)
	if _, err := io.ReadFull(r, data); err != nil {
		return err
	}
	return json.Unmarshal(data, value)
}

func writeStreamKind(w io.Writer, kind byte) error {
	_, err := w.Write([]byte{kind})
	return err
}

func readStreamKind(r io.Reader) (byte, error) {
	var kind [1]byte
	_, err := io.ReadFull(r, kind[:])
	return kind[0], err
}

func encodeDatagram(target string, payload []byte) ([]byte, error) {
	if target == "" || len(target) > 65535 {
		return nil, errors.New("invalid native datagram target")
	}
	if len(payload) > 65535 {
		return nil, errors.New("native datagram payload too large")
	}
	out := make([]byte, 4+len(target)+len(payload))
	binary.BigEndian.PutUint16(out[:2], uint16(len(target)))
	copy(out[2:], target)
	binary.BigEndian.PutUint16(out[2+len(target):], uint16(len(payload)))
	copy(out[4+len(target):], payload)
	return out, nil
}

func decodeDatagram(packet []byte) (Datagram, error) {
	if len(packet) < 4 || len(packet) > maxNativeDatagram {
		return Datagram{}, errors.New("invalid native datagram length")
	}
	targetLen := int(binary.BigEndian.Uint16(packet[:2]))
	if targetLen == 0 || len(packet) < 4+targetLen {
		return Datagram{}, errors.New("invalid native datagram target length")
	}
	payloadLen := int(binary.BigEndian.Uint16(packet[2+targetLen : 4+targetLen]))
	if len(packet) != 4+targetLen+payloadLen {
		return Datagram{}, errors.New("invalid native datagram payload length")
	}
	return Datagram{
		Target:  string(packet[2 : 2+targetLen]),
		Payload: append([]byte(nil), packet[4+targetLen:]...),
	}, nil
}
