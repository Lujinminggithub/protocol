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
	if profile.Segments["middle_exit"].Source.UDPFECAdaptive ||
		profile.Segments["middle_exit"].Source.UDPFECK != 8 ||
		profile.Segments["middle_exit"].Source.UDPFECHoldUS != 2000 {
		t.Fatalf("adaptive UDP FEC capability missing: %#v", profile.Segments["middle_exit"].Source)
	}
	if profile.SchemaVersion != 2 || profile.Segments["middle_exit"].Source.UDPFECMode != "nb-yfe2-optional" {
		t.Fatalf("NB YFE2 optional profile missing: %#v", profile.Segments["middle_exit"].Source)
	}
	if profile.Segments["entry_middle"].Source.MTUMax != 1452 {
		t.Fatalf("QUIC and DF evidence should retain proven IP MTU: %+v", profile.Segments["entry_middle"].Source)
	}
}

func TestGenerateRejectsFailedProbeAdmission(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {QUIC: QUICEvidence{PacketsObserved: 10000, WindowsValid: 6},
			Candidate: candidate("cubic", 524288, 1452, 16, 20000)},
		"middle_exit": {QUIC: QUICEvidence{PacketsObserved: 10000, WindowsValid: 6},
			Candidate: candidate("bbr", 0, 1452, 16, 20000)},
	}}
	probe.Admission.Status = "rejected"
	probe.Admission.Reasons = []string{"insufficient-downlink"}
	if _, err := Generate("gz-hk-uk", 1, 10, probe); err == nil ||
		!strings.Contains(err.Error(), "insufficient-downlink") {
		t.Fatalf("rejected admission generated a profile: %v", err)
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
	if entry.TargetRateBPS != 10_000_000 || middle.TargetRateBPS != 10_000_000 {
		t.Fatalf("runtime target rate missing: entry=%d middle=%d", entry.TargetRateBPS, middle.TargetRateBPS)
	}
	if entry.SeedRTTUS != 4400 || middle.SeedRTTUS != 186100 {
		t.Fatalf("QUIC RTT seed missing: entry=%d middle=%d", entry.SeedRTTUS, middle.SeedRTTUS)
	}
	if middle.StartupCWinBytes != 589824 {
		t.Fatalf("2x live wire startup BDP=%d, want 589824", middle.StartupCWinBytes)
	}
	if entry.ReorderGap > 64 || entry.ReorderDelayUS > 80000 {
		t.Fatalf("short-hop reorder envelope is not bounded: %+v", entry)
	}
	if middle.ReorderGap > 64 || middle.ReorderDelayUS > 120000 {
		t.Fatalf("long-hop reorder envelope is not bounded: %+v", middle)
	}
}

func TestGenerateUsesConservativeDFOnlyMTU(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {MTU: MTUEvidence{MaxIPMTU: 1452}, QUIC: QUICEvidence{PacketsObserved: 10000, WindowsValid: 6}, Candidate: candidate("bbr", 0, 1404, 8, 20000)},
		"middle_exit":  {MTU: MTUEvidence{MaxIPMTU: 1452}, QUIC: QUICEvidence{PacketsObserved: 10000, WindowsValid: 6}, Candidate: candidate("bbr", 0, 1404, 8, 20000)},
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
		"entry_middle": {QUIC: QUICEvidence{PacketsObserved: 10000, WindowsValid: 6}, Candidate: candidate("bbr", 0, 1404, 8, 20000)},
		"middle_exit":  {QUIC: QUICEvidence{PacketsObserved: 10000, WindowsValid: 6}, Candidate: candidate("bbr", 0, 1404, 8, 20000)},
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
	for _, expected := range []string{"schema=1", "role=middle", "ingress.cc=", "egress.cc=", "generation=9",
		"egress.udp_fec_adaptive=false", "egress.udp_fec_k=8", "egress.udp_fec_hold_us=2000",
		"egress.target_rate_bps=5000000", "egress.seed_rtt_us=250000",
		"egress.startup_cwin_bytes=393216"} {
		if !strings.Contains(text, expected) {
			t.Fatalf("missing %q in %s", expected, text)
		}
	}
	entry, err := profile.Role("entry")
	if err != nil {
		t.Fatal(err)
	}
	entryData, _, err := Render(entry)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(entryData), "nb-yfe2-optional") {
		t.Fatalf("entry unexpectedly enabled YFE2: %s", entryData)
	}
	if strings.Contains(text, "udp_fec_mode=") {
		t.Fatalf("schema-1 wire profile contains schema-2 FEC mode: %s", text)
	}
}

func TestGenerateBoundsKZReorderInsteadOfPreservingProbeCandidate(t *testing.T) {
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
	if middle.SeedRTTUS != 286274 || middle.StartupCWinBytes != 458752 {
		t.Fatalf("long-haul seed must use QUIC RTT and 2x average wire BDP: %+v", middle)
	}
	if middle.ReorderGap > 64 || middle.ReorderDelayUS > 120000 {
		t.Fatalf("KZ reorder envelope is not bounded for live traffic: %+v", middle)
	}
}

func TestGenerateUsesRobustEvidenceInsteadOfTransientMaximum(t *testing.T) {
	probe := Probe{SchemaVersion: 2, Segments: map[string]SegmentEvidence{
		"entry_middle": {QUIC: QUICEvidence{PacketsObserved: 12000, WindowsValid: 6}, Candidate: candidate("bbr", 0, 1404, 8, 20000)},
		"middle_exit": {ICMP: ICMP{RTTAvgMS: 203}, QUIC: QUICEvidence{PacketsObserved: 12000, WindowsValid: 7,
			JitterP95MS: 8, ReorderGapMax: 98, ReorderDelayMaxMS: 345,
			ReorderWindows: 7, ReorderGapP95: 7, ReorderDelayP95MS: 25},
			Candidate: candidate("bbr", 0, 1404, 16, 390000)},
	}}
	profile, err := Generate("gz-hk-kz-00001", 2, 5, probe)
	if err != nil {
		t.Fatal(err)
	}
	middle := profile.Segments["middle_exit"].Source
	if middle.ReorderGap != 15 || middle.ReorderDelayUS != 49000 {
		t.Fatalf("robust evidence was not used: %+v", middle)
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

func TestRenderUsesBackwardCompatibleWireSchema(t *testing.T) {
	profile, fingerprint, err := Render(RoleProfile{
		SchemaVersion: 2, LineID: "line-1", Generation: 3, Role: "middle",
		Ingress: &Link{CC: "cubic", MTUMax: 1404, ReorderGap: 8, ReorderDelayUS: 20000,
			FECObserve: true, UDPFECAdaptive: true, UDPFECMode: "off", CWinMaxBytes: 262144},
		Egress: &Link{CC: "bbr", MTUMax: 1404, ReorderGap: 8, ReorderDelayUS: 20000,
			FECObserve: true, UDPFECAdaptive: true, UDPFECMode: "nb-yfe2-optional"},
	})
	if err != nil || fingerprint == 0 {
		t.Fatalf("render failed: err=%v fingerprint=%x", err, fingerprint)
	}
	text := string(profile)
	if !strings.HasPrefix(text, "schema=1\n") || strings.Contains(text, "udp_fec_mode=") {
		t.Fatalf("wire profile was not downgraded safely: %s", text)
	}
}
