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
	if profile.Segments["middle_exit"].Source.BBROptions != "Q0.0001:F0.25:" {
		t.Fatalf("long-haul BBR recovery floor missing: %#v", profile.Segments["middle_exit"].Source)
	}
	if profile.Segments["middle_exit"].Source.FECActive {
		t.Fatal("automatic generation must not enable active FEC")
	}
	if profile.Segments["entry_middle"].Source.MTUMax != 1452 {
		t.Fatalf("QUIC and DF evidence should retain proven IP MTU: %+v", profile.Segments["entry_middle"].Source)
	}
}

func TestGenerateDoesNotTurnLoadedShortHopIntoBBR(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {ICMP: ICMP{RTTAvgMS: 4.4}, QUIC: QUICEvidence{RTTP95MS: 45.7, JitterP95MS: 46.5, PacketsObserved: 172992, WindowsValid: 18, ReorderGapMax: 4, ReorderDelayMaxMS: 220.6}},
		"middle_exit":  {ICMP: ICMP{RTTAvgMS: 202.3}, QUIC: QUICEvidence{RTTP95MS: 186.1, JitterP95MS: 1.7, EffectiveLossP95Pct: .052, PacketsObserved: 145717, WindowsValid: 17}},
	}}
	probe.ServicePackage.QualificationMbps = 12.5
	profile, err := Generate("gz-hk-kz", 2, 10, probe)
	if err != nil {
		t.Fatal(err)
	}
	entry := profile.Segments["entry_middle"].Source
	middle := profile.Segments["middle_exit"].Source
	if entry.CC != "cubic" || entry.CWinMaxBytes == 0 {
		t.Fatalf("loaded short hop must stay window-bounded CUBIC: %+v", entry)
	}
	if entry.TargetMbps != 10 || middle.TargetMbps != 10 {
		t.Fatalf("qualification headroom leaked into runtime target: entry=%g middle=%g", entry.TargetMbps, middle.TargetMbps)
	}
	if entry.ReorderGap != 13 || entry.ReorderDelayUS != 296000 {
		t.Fatalf("unexpected evidence-derived entry reorder floors: %+v", entry)
	}
	if middle.ReorderGap != 8 || middle.ReorderDelayUS != 20000 {
		t.Fatalf("clean long hop retained RTT-sized reorder floors: %+v", middle)
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
