package transportprofile

import (
	"errors"
	"fmt"
	"math"
	"strings"
)

const SchemaVersion = 1

type ICMP struct {
	LossPct  float64 `json:"loss_pct"`
	RTTAvgMS float64 `json:"rtt_avg_ms"`
	RTTMaxMS float64 `json:"rtt_max_ms"`
	MDevMS   float64 `json:"rtt_mdev_ms"`
}

type MTUEvidence struct {
	MaxIPMTU int `json:"max_ip_mtu"`
}

type QUICEvidence struct {
	WindowsValid        int     `json:"windows_valid"`
	PacketsObserved     int64   `json:"packets_observed"`
	RTTP95MS            float64 `json:"rtt_p95_ms"`
	JitterP95MS         float64 `json:"jitter_p95_ms"`
	EffectiveLossP95Pct float64 `json:"effective_loss_p95_pct"`
	ReorderGapMax       int     `json:"reorder_gap_max"`
	ReorderDelayMaxMS   float64 `json:"reorder_delay_max_ms"`
	ReorderWindows      int     `json:"reorder_percentile_windows"`
	ReorderGapP95       int     `json:"reorder_gap_p95"`
	ReorderDelayP95MS   float64 `json:"reorder_delay_p95_ms"`
	QUICIPMTUProven     int     `json:"quic_ip_mtu_proven"`
}

type SegmentEvidence struct {
	ICMP      ICMP          `json:"icmp"`
	MTU       MTUEvidence   `json:"mtu"`
	QUIC      QUICEvidence  `json:"quic"`
	Candidate LinkCandidate `json:"candidate"`
}

type LinkCandidate struct {
	Confidence     string  `json:"confidence"`
	CC             string  `json:"cc"`
	CWinMaxBytes   *uint64 `json:"cwin_max_bytes"`
	MTUMax         *int    `json:"mtu_max"`
	ReorderGap     int     `json:"reorder_gap"`
	ReorderDelayUS int64   `json:"reorder_delay_us"`
}

type Probe struct {
	SchemaVersion  int `json:"schema_version"`
	ServicePackage struct {
		QualificationMbps float64 `json:"qualification_mbps"`
	} `json:"service_package"`
	Segments map[string]SegmentEvidence `json:"segments"`
}

type Link struct {
	CC               string  `json:"cc"`
	BBROptions       string  `json:"bbr_options,omitempty"`
	CWinMaxBytes     uint64  `json:"cwin_max_bytes,omitempty"`
	MTUMax           int     `json:"mtu_max"`
	ReorderGap       int     `json:"reorder_gap"`
	ReorderDelayUS   int64   `json:"reorder_delay_us"`
	UDPGSO           bool    `json:"udp_gso"`
	FECObserve       bool    `json:"fec_observe"`
	FECActive        bool    `json:"fec_active"`
	UDPFECAdaptive   bool    `json:"udp_fec_adaptive"`
	UDPFECK          int     `json:"udp_fec_k"`
	UDPFECHoldUS     int64   `json:"udp_fec_hold_us"`
	TargetMbps       float64 `json:"target_mbps"`
	TargetRateBPS    uint64  `json:"target_rate_bps"`
	SeedRTTUS        int64   `json:"seed_rtt_us"`
	StartupCWinBytes uint64  `json:"startup_cwin_bytes"`
	Confidence       string  `json:"confidence"`
}

type Segment struct {
	Source Link `json:"source"`
	Target Link `json:"target"`
}

type Profile struct {
	SchemaVersion int                `json:"schema_version"`
	LineID        string             `json:"line_id"`
	Generation    uint64             `json:"generation"`
	Segments      map[string]Segment `json:"segments"`
}

type RoleProfile struct {
	SchemaVersion int    `json:"schema_version"`
	LineID        string `json:"line_id"`
	Generation    uint64 `json:"generation"`
	Role          string `json:"role"`
	Ingress       *Link  `json:"ingress,omitempty"`
	Egress        *Link  `json:"egress,omitempty"`
}

func validLineID(value string) bool {
	if len(value) == 0 || len(value) > 64 {
		return false
	}
	for _, character := range value {
		if !(character >= 'a' && character <= 'z' || character >= 'A' && character <= 'Z' ||
			character >= '0' && character <= '9' || strings.ContainsRune("._-", character)) {
			return false
		}
	}
	return true
}

func ceil64K(value float64) uint64 {
	return uint64(math.Ceil(value/65536.0)) * 65536
}

func minPositive(values ...int) int {
	selected := 0
	for _, value := range values {
		if value > 0 && (selected == 0 || value < selected) {
			selected = value
		}
	}
	return selected
}

func calculateLink(evidence SegmentEvidence, targetMbps float64) Link {
	loadedRTT := evidence.QUIC.RTTP95MS
	if loadedRTT <= 0 {
		loadedRTT = evidence.ICMP.RTTAvgMS
	}
	if loadedRTT <= 0 {
		loadedRTT = 250
	}
	pathRTT := evidence.ICMP.RTTAvgMS
	if pathRTT <= 0 {
		pathRTT = loadedRTT
	}
	targetRateBPS := uint64(math.Round(targetMbps * 1_000_000))
	seedRTTUS := int64(math.Round(pathRTT * 1000))
	startupBDP := targetMbps * 2 * 1_000_000 / 8 * pathRTT / 1000
	startupCWin := ceil64K(math.Max(256*1024, math.Min(64*1024*1024, startupBDP)))
	loss := evidence.QUIC.EffectiveLossP95Pct
	if evidence.QUIC.PacketsObserved == 0 {
		loss = evidence.ICMP.LossPct
	}
	cc := "bbr"
	// Loaded QUIC RTT/jitter includes queueing created by the validation load.
	// It must not turn a short physical segment into an uncapped BBR segment.
	if pathRTT <= 30 && evidence.ICMP.LossPct <= 1 && loss <= 1 {
		cc = "cubic"
	}
	bdp := targetMbps * 1_000_000 / 8 * loadedRTT / 1000
	cwin := ceil64K(math.Max(256*1024, math.Min(8*1024*1024, bdp*2)))
	reorderGap := evidence.QUIC.ReorderGapMax
	reorderDelayMS := evidence.QUIC.ReorderDelayMaxMS
	if evidence.QUIC.ReorderWindows > 0 {
		reorderGap = evidence.QUIC.ReorderGapP95
		reorderDelayMS = evidence.QUIC.ReorderDelayP95MS
	}
	gap := int(math.Ceil(float64(reorderGap))) + 8
	if gap < 8 {
		gap = 8
	}
	if gap > 64 {
		gap = 64
	}
	delayCapMS := 120.0
	if pathRTT <= 30 {
		delayCapMS = 80
	}
	delayMS := math.Min(delayCapMS,
		math.Max(20, reorderDelayMS+3*evidence.QUIC.JitterP95MS))
	delayUS := int64(math.Ceil(delayMS) * 1000)
	quicMTU := evidence.QUIC.QUICIPMTUProven
	dfMTU := evidence.MTU.MaxIPMTU
	mtu := 0
	if quicMTU > 0 {
		// QUIC evidence already reports the proven IP MTU. When both probes
		// agree, subtracting the UDP/IP overhead again is overly conservative.
		mtu = minPositive(quicMTU, dfMTU)
	} else if dfMTU > 0 {
		mtu = dfMTU - 48
	}
	if mtu < 1280 {
		mtu = 1404
	}
	if mtu > 1452 {
		mtu = 1452
	}
	confidence := "provisional-conservative"
	if evidence.QUIC.PacketsObserved >= 10_000 && evidence.QUIC.WindowsValid >= 6 {
		confidence = "load-qualified"
	}
	link := Link{CC: cc, MTUMax: mtu, ReorderGap: gap, ReorderDelayUS: delayUS,
		UDPGSO: false, FECObserve: true, FECActive: false,
		UDPFECAdaptive: true, UDPFECK: 8, UDPFECHoldUS: 2000,
		TargetMbps: targetMbps, TargetRateBPS: targetRateBPS,
		SeedRTTUS: seedRTTUS, StartupCWinBytes: startupCWin,
		Confidence: confidence}
	if cc == "cubic" {
		link.CWinMaxBytes = cwin
	} else {
		link.BBROptions = "Q0.0001:"
		if pathRTT >= 100 {
			link.BBROptions = "Q0.0001:F0.25:"
		}
	}
	return link
}

func selectedCandidate(evidence SegmentEvidence, targetMbps float64) (Link, error) {
	result := calculateLink(evidence, targetMbps)
	if result.Confidence != "load-qualified" {
		return Link{}, errors.New("probe evidence has no load-qualified transport candidate")
	}
	return result, nil
}

func validEvidence(evidence SegmentEvidence) bool {
	values := []float64{evidence.ICMP.LossPct, evidence.ICMP.RTTAvgMS, evidence.ICMP.RTTMaxMS,
		evidence.ICMP.MDevMS, evidence.QUIC.RTTP95MS, evidence.QUIC.JitterP95MS,
		evidence.QUIC.EffectiveLossP95Pct, evidence.QUIC.ReorderDelayMaxMS,
		evidence.QUIC.ReorderDelayP95MS}
	for _, value := range values {
		if math.IsNaN(value) || math.IsInf(value, 0) || value < 0 {
			return false
		}
	}
	return evidence.ICMP.LossPct <= 100 && evidence.QUIC.EffectiveLossP95Pct <= 100 &&
		evidence.ICMP.RTTMaxMS <= 60_000 && evidence.QUIC.RTTP95MS <= 60_000 &&
		evidence.QUIC.WindowsValid >= 0 && evidence.QUIC.PacketsObserved >= 0 &&
		evidence.QUIC.ReorderGapMax >= 0 && evidence.QUIC.ReorderGapMax <= 1_000_000 &&
		evidence.QUIC.ReorderWindows >= 0 && evidence.QUIC.ReorderWindows <= evidence.QUIC.WindowsValid &&
		evidence.QUIC.ReorderGapP95 >= 0 && evidence.QUIC.ReorderGapP95 <= 1_000_000 &&
		(evidence.MTU.MaxIPMTU == 0 || evidence.MTU.MaxIPMTU >= 1280 && evidence.MTU.MaxIPMTU <= 9000) &&
		(evidence.QUIC.QUICIPMTUProven == 0 || evidence.QUIC.QUICIPMTUProven >= 1280 && evidence.QUIC.QUICIPMTUProven <= 9000)
}

func Generate(lineID string, generation uint64, committedMbps float64, probe Probe) (Profile, error) {
	if !validLineID(lineID) || generation == 0 || committedMbps < 1 || committedMbps > 1000 || probe.SchemaVersion != 2 {
		return Profile{}, errors.New("invalid transport profile identity or service rate")
	}
	entryEvidence, entryOK := probe.Segments["entry_middle"]
	middleEvidence, middleOK := probe.Segments["middle_exit"]
	if !entryOK || !middleOK {
		return Profile{}, errors.New("probe evidence requires entry_middle and middle_exit segments")
	}
	if !validEvidence(entryEvidence) || !validEvidence(middleEvidence) {
		return Profile{}, errors.New("probe evidence contains invalid transport measurements")
	}
	// qualification_mbps is a validation load target with headroom. It is not
	// a runtime service target and must never oversubscribe the purchased rate.
	target := committedMbps
	entryLink, err := selectedCandidate(entryEvidence, target)
	if err != nil {
		return Profile{}, fmt.Errorf("entry_middle: %w", err)
	}
	middleLink, err := selectedCandidate(middleEvidence, target)
	if err != nil {
		return Profile{}, fmt.Errorf("middle_exit: %w", err)
	}
	return Profile{SchemaVersion: SchemaVersion, LineID: lineID, Generation: generation,
		Segments: map[string]Segment{
			"entry_middle": {Source: entryLink, Target: entryLink},
			"middle_exit":  {Source: middleLink, Target: middleLink},
		}}, nil
}

func (profile Profile) Role(role string) (RoleProfile, error) {
	entry, entryOK := profile.Segments["entry_middle"]
	middle, middleOK := profile.Segments["middle_exit"]
	if profile.SchemaVersion != SchemaVersion || !validLineID(profile.LineID) || profile.Generation == 0 || !entryOK || !middleOK {
		return RoleProfile{}, errors.New("invalid transport profile")
	}
	result := RoleProfile{SchemaVersion: SchemaVersion, LineID: profile.LineID, Generation: profile.Generation, Role: role}
	switch role {
	case "entry":
		result.Egress = &entry.Source
	case "middle":
		result.Ingress, result.Egress = &entry.Target, &middle.Source
	case "exit":
		result.Ingress = &middle.Target
	default:
		return RoleProfile{}, fmt.Errorf("invalid transport role: %s", role)
	}
	return result, nil
}
