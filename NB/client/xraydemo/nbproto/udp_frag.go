package nbproto

import (
	"encoding/binary"
	"errors"
)

const (
	UDPMagic            uint32 = 0x4e425544
	UDPVersion          byte   = 1
	UDPTypeClientToExit byte   = 1
	UDPTypeExitToClient byte   = 2
	UDPHeaderSize              = 24
	UDPFragmentPayload         = 1000
	UDPMaxPayload              = 65507
	UDPMaxRoute                = 299
	UDPMaxFragments            = 66
	UDPReassemblySlots         = 8
)

// UDPFragment is compatible with the server-side NBUD v1 framing in src/nb_udp.c.
type UDPFragment struct {
	Type          byte
	SessionID     uint32
	Sequence      uint32
	FragmentIndex uint16
	FragmentCount uint16
	TotalLength   uint16
	Route         string
	Payload       []byte
}

func FragmentUDP(frameType byte, sessionID, sequence uint32, route string, payload []byte) ([][]byte, error) {
	if (frameType != UDPTypeClientToExit && frameType != UDPTypeExitToClient) || sessionID == 0 ||
		len(route) == 0 || len(route) > UDPMaxRoute || len(payload) == 0 || len(payload) > UDPMaxPayload {
		return nil, errors.New("invalid UDP fragment input")
	}
	count := (len(payload) + UDPFragmentPayload - 1) / UDPFragmentPayload
	frames := make([][]byte, 0, count)
	for index, offset := 0, 0; offset < len(payload); index, offset = index+1, offset+UDPFragmentPayload {
		end := min(offset+UDPFragmentPayload, len(payload))
		frame := make([]byte, UDPHeaderSize+len(route)+end-offset)
		binary.BigEndian.PutUint32(frame[0:4], UDPMagic)
		frame[4], frame[5] = UDPVersion, frameType
		binary.BigEndian.PutUint16(frame[6:8], UDPHeaderSize)
		binary.BigEndian.PutUint32(frame[8:12], sessionID)
		binary.BigEndian.PutUint32(frame[12:16], sequence)
		binary.BigEndian.PutUint16(frame[16:18], uint16(index))
		binary.BigEndian.PutUint16(frame[18:20], uint16(count))
		binary.BigEndian.PutUint16(frame[20:22], uint16(len(payload)))
		binary.BigEndian.PutUint16(frame[22:24], uint16(len(route)))
		copy(frame[UDPHeaderSize:], route)
		copy(frame[UDPHeaderSize+len(route):], payload[offset:end])
		frames = append(frames, frame)
	}
	return frames, nil
}

func DecodeUDPFragment(frame []byte) (UDPFragment, error) {
	if len(frame) <= UDPHeaderSize || binary.BigEndian.Uint32(frame[0:4]) != UDPMagic ||
		frame[4] != UDPVersion || binary.BigEndian.Uint16(frame[6:8]) != UDPHeaderSize {
		return UDPFragment{}, errors.New("invalid UDP fragment header")
	}
	f := UDPFragment{
		Type: frame[5], SessionID: binary.BigEndian.Uint32(frame[8:12]),
		Sequence:      binary.BigEndian.Uint32(frame[12:16]),
		FragmentIndex: binary.BigEndian.Uint16(frame[16:18]),
		FragmentCount: binary.BigEndian.Uint16(frame[18:20]),
		TotalLength:   binary.BigEndian.Uint16(frame[20:22]),
	}
	routeLength := int(binary.BigEndian.Uint16(frame[22:24]))
	expectedCount := (int(f.TotalLength) + UDPFragmentPayload - 1) / UDPFragmentPayload
	if (f.Type != UDPTypeClientToExit && f.Type != UDPTypeExitToClient) || f.SessionID == 0 ||
		f.FragmentCount == 0 || f.FragmentCount > UDPMaxFragments || int(f.FragmentCount) != expectedCount ||
		f.FragmentIndex >= f.FragmentCount ||
		f.TotalLength == 0 || int(f.TotalLength) > UDPMaxPayload || routeLength == 0 || routeLength > UDPMaxRoute ||
		len(frame) <= UDPHeaderSize+routeLength {
		return UDPFragment{}, errors.New("invalid UDP fragment fields")
	}
	payloadLength := len(frame) - UDPHeaderSize - routeLength
	offset := int(f.FragmentIndex) * UDPFragmentPayload
	if payloadLength > UDPFragmentPayload || offset+payloadLength > int(f.TotalLength) ||
		(f.FragmentIndex+1 < f.FragmentCount && payloadLength != UDPFragmentPayload) ||
		(f.FragmentIndex+1 == f.FragmentCount && offset+payloadLength != int(f.TotalLength)) {
		return UDPFragment{}, errors.New("invalid UDP fragment length")
	}
	f.Route = string(frame[UDPHeaderSize : UDPHeaderSize+routeLength])
	f.Payload = frame[UDPHeaderSize+routeLength:]
	return f, nil
}

type udpAssembly struct {
	sequence uint32
	route    string
	total    uint16
	count    uint16
	updated  uint64
	received uint16
	seen     [2]uint64
	data     []byte
}

// UDPReassembler bounds incomplete datagrams by slot count and caller-provided expiry.
type UDPReassembler struct {
	slots     []udpAssembly
	timeoutUS uint64
}

func NewUDPReassembler(timeoutUS uint64) *UDPReassembler {
	return &UDPReassembler{slots: make([]udpAssembly, UDPReassemblySlots), timeoutUS: timeoutUS}
}

func (r *UDPReassembler) Feed(frame []byte, nowUS uint64) ([]byte, string, bool, error) {
	f, err := DecodeUDPFragment(frame)
	if err != nil {
		return nil, "", false, err
	}
	index := -1
	oldest := 0
	for i := range r.slots {
		s := &r.slots[i]
		if len(s.data) > 0 && r.timeoutUS > 0 && nowUS > s.updated && nowUS-s.updated >= r.timeoutUS {
			*s = udpAssembly{}
		}
		if len(s.data) > 0 && s.sequence == f.Sequence {
			index = i
			break
		}
		if len(s.data) == 0 {
			index = i
			break
		}
		if r.slots[i].updated < r.slots[oldest].updated {
			oldest = i
		}
	}
	if index < 0 {
		index = oldest
	}
	s := &r.slots[index]
	if len(s.data) == 0 || s.sequence != f.Sequence {
		*s = udpAssembly{sequence: f.Sequence, route: f.Route, total: f.TotalLength,
			count: f.FragmentCount, updated: nowUS, data: make([]byte, f.TotalLength)}
	}
	if s.route != f.Route || s.total != f.TotalLength || s.count != f.FragmentCount {
		*s = udpAssembly{}
		return nil, "", false, errors.New("inconsistent UDP fragments")
	}
	word, bit := f.FragmentIndex/64, uint64(1)<<(f.FragmentIndex%64)
	if s.seen[word]&bit == 0 {
		copy(s.data[int(f.FragmentIndex)*UDPFragmentPayload:], f.Payload)
		s.seen[word] |= bit
		s.received++
	}
	s.updated = nowUS
	if s.received != s.count {
		return nil, "", false, nil
	}
	payload, route := s.data, s.route
	*s = udpAssembly{}
	return payload, route, true, nil
}
