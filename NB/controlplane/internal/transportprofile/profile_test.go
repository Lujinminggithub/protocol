package transportprofile

import (
	"strings"
	"testing"
)

func TestGenerateSeparatesPhysicalSegments(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {ICMP: ICMP{RTTAvgMS: 4.4}, MTU: MTUEvidence{MaxIPMTU: 1452}, QUIC: QUICEvidence{RTTP95MS: 5, JitterP95MS: 1, PacketsObserved: 12000, WindowsValid: 6, QUICIPMTUProven: 1452}},
		"middle_exit":  {ICMP: ICMP{RTTAvgMS: 202}, MTU: MTUEvidence{MaxIPMTU: 1452}, QUIC: QUICEvidence{RTTP95MS: 205, JitterP95MS: 8, PacketsObserved: 14000, WindowsValid: 7, QUICIPMTUProven: 1452}},
	}}
	profile, err := Generate("gz-hk-kz", 3, 10, probe)
	if err != nil {
		t.Fatal(err)
	}
	if profile.Segments["entry_middle"].Source.CC != "cubic" || profile.Segments["middle_exit"].Source.CC != "bbr" {
		t.Fatalf("segments were not independently tuned: %#v", profile.Segments)
	}
	if profile.Segments["entry_middle"].Source.ReorderDelayUS >= profile.Segments["middle_exit"].Source.ReorderDelayUS {
		t.Fatal("long-haul segment should have a larger reorder delay")
	}
	if profile.Segments["middle_exit"].Source.FECActive {
		t.Fatal("automatic generation must not enable active FEC")
	}
	if profile.Segments["entry_middle"].Source.MTUMax != 1452 {
		t.Fatalf("QUIC and DF evidence should retain proven IP MTU: %+v", profile.Segments["entry_middle"].Source)
	}
}

func TestGenerateUsesConservativeDFOnlyMTU(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {MTU: MTUEvidence{MaxIPMTU: 1452}},
		"middle_exit":  {MTU: MTUEvidence{MaxIPMTU: 1452}},
	}}
	profile, err := Generate("line-df", 1, 5, probe)
	if err != nil {
		t.Fatal(err)
	}
	if got := profile.Segments["entry_middle"].Source.MTUMax; got != 1404 {
		t.Fatalf("DF-only MTU=%d, want 1404", got)
	}
}

func TestRoleRenderProducesMiddleIngressAndEgress(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{"entry_middle": {}, "middle_exit": {}}}
	profile, err := Generate("line-1", 9, 5, probe)
	if err != nil {
		t.Fatal(err)
	}
	middle, err := profile.Role("middle")
	if err != nil {
		t.Fatal(err)
	}
	data, fingerprint, err := Render(middle)
	if err != nil || fingerprint == 0 {
		t.Fatalf("render failed: %v", err)
	}
	text := string(data)
	for _, expected := range []string{"role=middle", "ingress.cc=", "egress.cc=", "generation=9"} {
		if !strings.Contains(text, expected) {
			t.Fatalf("missing %q in %s", expected, text)
		}
	}
}

func TestGenerateRejectsUnversionedOrInvalidEvidence(t *testing.T) {
	probe := Probe{SchemaVersion: 1, Segments: map[string]SegmentEvidence{"entry_middle": {}, "middle_exit": {}}}
	if _, err := Generate("line-1", 1, 5, probe); err == nil {
		t.Fatal("unversioned evidence was accepted")
	}
	probe.SchemaVersion = 2
	probe.Segments["middle_exit"] = SegmentEvidence{QUIC: QUICEvidence{EffectiveLossP95Pct: 101}}
	if _, err := Generate("line-1", 1, 5, probe); err == nil {
		t.Fatal("invalid loss evidence was accepted")
	}
}
