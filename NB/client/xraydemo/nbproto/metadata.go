package nbproto

import (
	"encoding/binary"
	"errors"
)

const (
	Magic      uint32 = 0x4e424d32
	Version    byte   = 1
	FlowOpen   byte   = 1
	HeaderSize        = 48

	FlagAllowFEC       uint32 = 1 << 0
	FlagAllowMultipath uint32 = 1 << 1
	FlagIdempotent     uint32 = 1 << 2
	allFlags                  = FlagAllowFEC | FlagAllowMultipath | FlagIdempotent

	ClassReliable byte = 1
	ClassRealtime byte = 2
	ClassBulk     byte = 3
	ClassControl  byte = 4

	PathAny       byte = 0
	PathStable    byte = 1
	PathLowJitter byte = 2
	PathRedundant byte = 3
)

type Metadata struct {
	Flags          uint32 `json:"flags"`
	TrafficClass   byte   `json:"traffic_class"`
	Priority       byte   `json:"priority"`
	PathPreference byte   `json:"path_preference"`
	SessionID      uint64 `json:"session_id"`
	FlowID         uint64 `json:"flow_id"`
	DeadlineMS     uint32 `json:"deadline_ms"`
	PolicyID       uint32 `json:"policy_id"`
	Route          string `json:"route"`
	Target         string `json:"target"`
	BusinessTag    string `json:"business_tag,omitempty"`
}

func validText(value string, maximum int, optional, tag bool) bool {
	if len(value) == 0 {
		return optional
	}
	if len(value) > maximum {
		return false
	}
	for i := range len(value) {
		c := value[i]
		if tag {
			if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
				return false
			}
		} else if c < 0x21 || c > 0x7e {
			return false
		}
	}
	return true
}

func (m Metadata) Validate() error {
	if m.Flags&^allFlags != 0 || m.TrafficClass < ClassReliable || m.TrafficClass > ClassControl ||
		m.Priority > 7 || m.PathPreference > PathRedundant || m.SessionID == 0 || m.FlowID == 0 ||
		m.DeadlineMS > 60000 {
		return errors.New("invalid metadata fields")
	}
	if m.Flags&FlagAllowFEC != 0 && m.TrafficClass != ClassRealtime {
		return errors.New("FEC requires realtime class")
	}
	if m.TrafficClass == ClassRealtime && m.DeadlineMS == 0 {
		return errors.New("realtime class requires deadline")
	}
	if m.PathPreference == PathRedundant && m.Flags&FlagAllowMultipath == 0 {
		return errors.New("redundant path requires multipath")
	}
	if !validText(m.Route, 300, false, false) || !validText(m.Target, 255, false, false) ||
		!validText(m.BusinessTag, 31, true, true) {
		return errors.New("invalid metadata text")
	}
	return nil
}

func Encode(m Metadata) ([]byte, error) {
	if err := m.Validate(); err != nil {
		return nil, err
	}
	total := HeaderSize + len(m.Route) + len(m.Target) + len(m.BusinessTag)
	wire := make([]byte, total)
	binary.BigEndian.PutUint32(wire[0:4], Magic)
	wire[4], wire[5] = Version, FlowOpen
	binary.BigEndian.PutUint16(wire[6:8], HeaderSize)
	binary.BigEndian.PutUint32(wire[8:12], m.Flags)
	wire[12], wire[13], wire[14] = m.TrafficClass, m.Priority, m.PathPreference
	binary.BigEndian.PutUint64(wire[16:24], m.SessionID)
	binary.BigEndian.PutUint64(wire[24:32], m.FlowID)
	binary.BigEndian.PutUint32(wire[32:36], m.DeadlineMS)
	binary.BigEndian.PutUint32(wire[36:40], m.PolicyID)
	binary.BigEndian.PutUint16(wire[40:42], uint16(len(m.Route)))
	binary.BigEndian.PutUint16(wire[42:44], uint16(len(m.Target)))
	binary.BigEndian.PutUint16(wire[44:46], uint16(len(m.BusinessTag)))
	offset := HeaderSize
	offset += copy(wire[offset:], m.Route)
	offset += copy(wire[offset:], m.Target)
	copy(wire[offset:], m.BusinessTag)
	return wire, nil
}

func Decode(wire []byte) (Metadata, error) {
	if len(wire) < HeaderSize || binary.BigEndian.Uint32(wire[0:4]) != Magic ||
		wire[4] != Version || wire[5] != FlowOpen || binary.BigEndian.Uint16(wire[6:8]) != HeaderSize ||
		wire[15] != 0 || binary.BigEndian.Uint16(wire[46:48]) != 0 {
		return Metadata{}, errors.New("invalid metadata header")
	}
	routeLen := int(binary.BigEndian.Uint16(wire[40:42]))
	targetLen := int(binary.BigEndian.Uint16(wire[42:44]))
	tagLen := int(binary.BigEndian.Uint16(wire[44:46]))
	if routeLen < 1 || routeLen > 300 || targetLen < 1 || targetLen > 255 || tagLen > 31 ||
		len(wire) != HeaderSize+routeLen+targetLen+tagLen {
		return Metadata{}, errors.New("invalid metadata length")
	}
	offset := HeaderSize
	m := Metadata{
		Flags:          binary.BigEndian.Uint32(wire[8:12]),
		TrafficClass:   wire[12],
		Priority:       wire[13],
		PathPreference: wire[14],
		SessionID:      binary.BigEndian.Uint64(wire[16:24]),
		FlowID:         binary.BigEndian.Uint64(wire[24:32]),
		DeadlineMS:     binary.BigEndian.Uint32(wire[32:36]),
		PolicyID:       binary.BigEndian.Uint32(wire[36:40]),
		Route:          string(wire[offset : offset+routeLen]),
	}
	offset += routeLen
	m.Target = string(wire[offset : offset+targetLen])
	offset += targetLen
	m.BusinessTag = string(wire[offset : offset+tagLen])
	if err := m.Validate(); err != nil {
		return Metadata{}, err
	}
	return m, nil
}
