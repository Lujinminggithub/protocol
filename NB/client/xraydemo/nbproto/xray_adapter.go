package nbproto

import "errors"

// Flow describes the fields an Xray outbound should extract from its routing context.
type Flow struct {
	SessionID   uint64
	FlowID      uint64
	Network     string
	Target      string
	Route       string
	BusinessTag string
	Realtime    bool
}

func FromXrayFlow(flow Flow) (Metadata, error) {
	if flow.Network != "tcp" && flow.Network != "udp" {
		return Metadata{}, errors.New("unsupported Xray network")
	}
	metadata := Metadata{SessionID: flow.SessionID, FlowID: flow.FlowID, Target: flow.Target,
		Route: flow.Route, BusinessTag: flow.BusinessTag, TrafficClass: ClassReliable,
		Priority: 4, PathPreference: PathStable}
	if flow.Realtime {
		metadata.TrafficClass = ClassRealtime
		metadata.Priority = 6
		metadata.PathPreference = PathLowJitter
		metadata.Flags = FlagAllowFEC | FlagAllowMultipath
		metadata.DeadlineMS = 200
	}
	return metadata, metadata.Validate()
}
