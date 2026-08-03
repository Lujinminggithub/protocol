package transportprofile

import (
	"fmt"
	"hash/fnv"
	"strconv"
	"strings"
)

func appendLink(lines []string, name string, link *Link) []string {
	if link == nil {
		return lines
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
	lines := []string{"schema=1", "line_id=" + profile.LineID,
		"generation=" + strconv.FormatUint(profile.Generation, 10), "role=" + profile.Role}
	lines = appendLink(lines, "ingress", profile.Ingress)
	lines = appendLink(lines, "egress", profile.Egress)
	data := []byte(strings.Join(lines, "\n") + "\n")
	hash := fnv.New64a()
	_, _ = hash.Write(data)
	return data, hash.Sum64(), nil
}
