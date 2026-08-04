package transportprofile

import (
	"strings"
	"testing"
)

func candidate(cc string, cwin uint64, mtu, gap int, delayUS int64) LinkCandidate {
	result := LinkCandidate{Confidence: "load-qualified", CC: cc, MTUMax: &mtu,
		ReorderGap: gap, ReorderDelayUS: delayUS}
	if cc == "cubic" {
		result.CWinMaxBytes = &cwin
	}
	return result
}

func TestGenerateSeparatesPhysicalSegments(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {ICMP: ICMP{RTTAvgMS: 4.4}, MTU: MTUEvidence{MaxIPMTU: 1452}, QUIC: QUICEvidence{RTTP95MS: 5, JitterP95MS: 1, PacketsObserved: 12000, WindowsValid: 6, QUICIPMTUProven: 1452}, Candidate: candidate("cubic", 524288, 1452, 16, 20000)},
		"middle_exit":  {ICMP: ICMP{RTTAvgMS: 202}, MTU: MTUEvidence{MaxIPMTU: 1452}, QUIC: QUICEvidence{RTTP95MS: 205, JitterP95MS: 8, PacketsObserved: 14000, WindowsValid: 7, QUICIPMTUProven: 1452}, Candidate: candidate("bbr", 0, 1452, 16, 20000)},
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
		"entry_middle": {ICMP: ICMP{RTTAvgMS: 4.4}, QUIC: QUICEvidence{RTTP95MS: 45.7, JitterP95MS: 46.5, PacketsObserved: 172992, WindowsValid: 18, ReorderGapMax: 4, ReorderDelayMaxMS: 220.6}, Candidate: candidate("cubic", 524288, 1404, 128, 450000)},
		"middle_exit":  {ICMP: ICMP{RTTAvgMS: 202.3}, QUIC: QUICEvidence{RTTP95MS: 186.1, JitterP95MS: 1.7, EffectiveLossP95Pct: .052, PacketsObserved: 145717, WindowsValid: 17}, Candidate: candidate("bbr", 0, 1404, 128, 450000)},
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
	if entry.ReorderGap != 128 || entry.ReorderDelayUS != 450000 {
		t.Fatalf("probe candidate safety envelope was not preserved: %+v", entry)
	}
	if middle.ReorderGap != 128 || middle.ReorderDelayUS != 450000 {
		t.Fatalf("long-haul safety envelope was not preserved: %+v", middle)
	}
}

func TestGenerateUsesConservativeDFOnlyMTU(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {MTU: MTUEvidence{MaxIPMTU: 1452}, Candidate: candidate("bbr", 0, 1404, 8, 20000)},
		"middle_exit":  {MTU: MTUEvidence{MaxIPMTU: 1452}, Candidate: candidate("bbr", 0, 1404, 8, 20000)},
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
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {Candidate: candidate("bbr", 0, 1404, 8, 20000)},
		"middle_exit":  {Candidate: candidate("bbr", 0, 1404, 8, 20000)},
	}}
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

func TestGeneratePreservesKZProbeCandidateInsteadOfRecalculatingIt(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {ICMP: ICMP{RTTAvgMS: 4.4}, QUIC: QUICEvidence{RTTP95MS: 6.937,
			PacketsObserved: 220868, WindowsValid: 18, ReorderGapMax: 5,
			ReorderDelayMaxMS: 14.334}, Candidate: candidate("cubic", 524288, 1404, 128, 450000)},
		"middle_exit": {ICMP: ICMP{RTTAvgMS: 202.3}, QUIC: QUICEvidence{RTTP95MS: 286.274,
			EffectiveLossP95Pct: 2.404, PacketsObserved: 170244, WindowsValid: 19,
			ReorderGapMax: 7, ReorderDelayMaxMS: 295.66}, Candidate: candidate("bbr", 0, 1404, 128, 573000)},
	}}
	profile, err := Generate("gz-hk-kz-00001", 2, 5, probe)
	if err != nil {
		t.Fatal(err)
	}
	middle := profile.Segments["middle_exit"].Source
	if middle.ReorderGap != 128 || middle.ReorderDelayUS != 573000 {
		t.Fatalf("KZ load-qualified candidate was discarded: %+v", middle)
	}
}

func TestGenerateRejectsCandidateBelowEvidenceFloor(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {Candidate: candidate("bbr", 0, 1404, 8, 20000)},
		"middle_exit": {QUIC: QUICEvidence{ReorderGapMax: 98, ReorderDelayMaxMS: 345},
			Candidate: candidate("bbr", 0, 1404, 16, 390000)},
	}}
	if _, err := Generate("gz-hk-kz-00001", 2, 5, probe); err == nil {
		t.Fatal("candidate below observed reorder floor was accepted")
	}
}

func TestGenerateRejectsUnversionedOrInvalidEvidence(t *testing.T) {
	probe := Probe{SchemaVersion: 1, Segments: map[string]SegmentEvidence{"entry_middle": {}, "middle_exit": {}}}
	if _, err := Generate("line-1", 1, 5, probe); err == nil {
		t.Fatal("unversioned evidence was accepted")
	}
	probe.SchemaVersion = 2
	probe.Segments["middle_exit"] = SegmentEvidence{QUIC: QUICEvidence{EffectiveLossP95Pct: 101}, Candidate: candidate("bbr", 0, 1404, 8, 20000)}
	if _, err := Generate("line-1", 1, 5, probe); err == nil {
		t.Fatal("invalid loss evidence was accepted")
	}
}
