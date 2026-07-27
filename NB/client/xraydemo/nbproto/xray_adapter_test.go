package nbproto

import "testing"

func TestFromXrayFlow(t *testing.T) {
	m, err := FromXrayFlow(Flow{SessionID: 1, FlowID: 2, Network: "udp",
		Target: "live.example:443", Route: "H:middle.example:4443", Realtime: true})
	if err != nil || m.TrafficClass != ClassRealtime || m.Flags&FlagAllowFEC == 0 {
		t.Fatalf("unexpected adapter result: %#v %v", m, err)
	}
	if _, err := FromXrayFlow(Flow{Network: "icmp"}); err == nil {
		t.Fatal("unsupported network accepted")
	}
}
