package transportprofile

import (
	"fmt"
	"hash/fnv"
	"strconv"
	"strings"
)

func appendLink(lines []string, name string, link *Link, wireSchema int) []string {
	if link == nil {
		return lines
	}
	fecMode := link.UDPFECMode
	if fecMode == "" {
		fecMode = "off"
	}
	values := [][2]string{
		{name + ".cc", link.CC},
		{name + ".bbr_options", link.BBROptions},
		{name + ".cwin_max_bytes", strconv.FormatUint(link.CWinMaxBytes, 10)},
		{name + ".mtu_max", strconv.Itoa(link.MTUMax)},
		{name + ".reorder_gap", strconv.Itoa(link.ReorderGap)},
		{name + ".reorder_delay_us", strconv.FormatInt(link.ReorderDelayUS, 10)},
		{name + ".udp_gso", strconv.FormatBool(link.UDPGSO)},
		{name + ".fec_observe", strconv.FormatBool(link.FECObserve)},
		{name + ".fec_active", strconv.FormatBool(link.FECActive)},
		{name + ".udp_fec_adaptive", strconv.FormatBool(link.UDPFECAdaptive)},
		{name + ".udp_fec_k", strconv.Itoa(link.UDPFECK)},
		{name + ".udp_fec_hold_us", strconv.FormatInt(link.UDPFECHoldUS, 10)},
		{name + ".target_rate_bps", strconv.FormatUint(link.TargetRateBPS, 10)},
		{name + ".seed_rtt_us", strconv.FormatInt(link.SeedRTTUS, 10)},
		{name + ".startup_cwin_bytes", strconv.FormatUint(link.StartupCWinBytes, 10)},
	}
	if wireSchema >= 2 {
		values = append(values, [2]string{name + ".udp_fec_mode", fecMode})
	}
	for _, value := range values {
		lines = append(lines, value[0]+"="+value[1])
	}
	return lines
}

func Render(profile RoleProfile) ([]byte, uint64, error) {
	if !validLineID(profile.LineID) || profile.Generation == 0 {
		return nil, 0, fmt.Errorf("invalid role profile")
	}
	// Older production nodes only understand schema 1. The control-plane
	// model remains schema 2, but the wire profile stays backward compatible;
	// schema-2-only FEC mode is intentionally omitted in the downgraded form.
	wireSchema := 1
	lines := []string{"schema=" + strconv.Itoa(wireSchema), "line_id=" + profile.LineID,
		"generation=" + strconv.FormatUint(profile.Generation, 10), "role=" + profile.Role}
	lines = appendLink(lines, "ingress", profile.Ingress, wireSchema)
	lines = appendLink(lines, "egress", profile.Egress, wireSchema)
	data := []byte(strings.Join(lines, "\n") + "\n")
	hash := fnv.New64a()
	_, _ = hash.Write(data)
	return data, hash.Sum64(), nil
}
