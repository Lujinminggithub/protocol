package coremodel

import (
	"net"
	"strconv"
	"strings"
	"time"
)

// FlowKind 表示核心会话模型里的流类型。
type FlowKind string

const (
	FlowKindTCP FlowKind = "tcp"
	FlowKindUDP FlowKind = "udp"
)

// PriorityClass 是统一调度视角下的优先级。
type PriorityClass uint8

const (
	PriorityBulk PriorityClass = iota
	PriorityDefault
	PriorityInteractive
	PriorityCritical
)

func (p PriorityClass) String() string {
	switch p {
	case PriorityCritical:
		return "critical"
	case PriorityInteractive:
		return "interactive"
	case PriorityDefault:
		return "default"
	default:
		return "bulk"
	}
}

// FlowClass 用于把不同目标归入统一会话模型的语义类别。
type FlowClass string

const (
	FlowClassBulkTraffic    FlowClass = "bulk"
	FlowClassInteractive    FlowClass = "interactive"
	FlowClassControlPlane   FlowClass = "control-plane"
	FlowClassMediaRealtime  FlowClass = "media-realtime"
	FlowClassMediaBootstrap FlowClass = "media-bootstrap"
	FlowClassProbeWeb       FlowClass = "probe-web"
	FlowClassProbePush      FlowClass = "probe-push"
	FlowClassLoginCritical  FlowClass = "login-critical"
	FlowClassCompatFallback FlowClass = "compat-fallback"
)

type ClassifierBudget struct {
	ReadTimeout         time.Duration
	IdleAfterFirstByte  time.Duration
	PreferredCopies     int
	PreferReducedFEC    bool
	PreferFastAck       bool
	MaxBufferedBytes    int
	PreferIndependentIO bool
}

type ClassifierRule struct {
	Name            string
	Kind            FlowKind
	Ports           []uint16
	HostContainsAny []string
	Class           FlowClass
	Priority        PriorityClass
	Budget          ClassifierBudget
}

type FlowClassifier struct {
	Enabled        bool
	Rules          []ClassifierRule
	FallbackClass  FlowClass
	FallbackPriority PriorityClass
	FallbackBudget ClassifierBudget
}

const (
	WireClassBulk byte = iota
	WireClassInteractive
	WireClassControlPlane
	WireClassMediaRealtime
	WireClassProbeWeb
	WireClassProbePush
	WireClassLoginCritical
	WireClassCompatFallback
	WireClassMediaBootstrap
)

const (
	BudgetFlagReducedFEC    byte = 1 << 0
	BudgetFlagFastACK       byte = 1 << 1
	BudgetFlagIndependentIO byte = 1 << 2
)

// StreamBudget 定义每条流的资源预算与生命周期倾向。
type StreamBudget struct {
	ReadTimeout         time.Duration
	IdleAfterFirstByte  time.Duration
	PreferredCopies     int
	PreferReducedFEC    bool
	PreferFastAck       bool
	MaxBufferedBytes    int
	PreferIndependentIO bool
}

// ACKSemantic / CCSemantic / FECSemantic 统一表达前后端共享的语义。
type ACKSemantic struct {
	PreferImmediate bool
	FeedbackFloor   time.Duration
}

type CCSemantic struct {
	Mode               string
	PriorityAware      bool
	PerStreamBudgeting bool
}

type FECSemantic struct {
	Mode                   string
	Auto                   bool
	MaxParityShards        uint32
	AllowBypassForCritical bool
}

// SessionSemantic 是 canonical xgw core session model 的共享定义。
type SessionSemantic struct {
	SessionID string
	Frontend  string
	RouteName string
	LineID    string

	ACK ACKSemantic
	CC  CCSemantic
	FEC FECSemantic
}

// OpenRequest is the canonical backend-open request shared by all frontends.
// Compatibility fronts may translate access protocols differently, but they
// must hand the backend the same flow/session semantics.
type OpenRequest struct {
	Flow    FlowRequest
	Session SessionSemantic
}

func DefaultSessionSemantic(frontend string) SessionSemantic {
	return SessionSemantic{
		Frontend: frontend,
		ACK: ACKSemantic{
			PreferImmediate: false,
			FeedbackFloor:   8 * time.Millisecond,
		},
		CC: CCSemantic{
			Mode:               "bbr",
			PriorityAware:      true,
			PerStreamBudgeting: true,
		},
		FEC: FECSemantic{
			Mode:                   "adaptive",
			Auto:                   true,
			MaxParityShards:        3,
			AllowBypassForCritical: true,
		},
	}
}

// FlowRequest 是前端入口和原生客户端共同消费的统一开流请求。
type FlowRequest struct {
	Kind     FlowKind
	Target   string
	Host     string
	Port     uint16
	Class    FlowClass
	Priority PriorityClass
	Budget   StreamBudget
}

func (f FlowRequest) WireClass() byte {
	switch f.Class {
	case FlowClassInteractive:
		return WireClassInteractive
	case FlowClassControlPlane:
		return WireClassControlPlane
	case FlowClassMediaRealtime:
		return WireClassMediaRealtime
	case FlowClassMediaBootstrap:
		return WireClassMediaBootstrap
	case FlowClassProbeWeb:
		return WireClassProbeWeb
	case FlowClassProbePush:
		return WireClassProbePush
	case FlowClassLoginCritical:
		return WireClassLoginCritical
	case FlowClassCompatFallback:
		return WireClassCompatFallback
	default:
		return WireClassBulk
	}
}

func (f FlowRequest) WirePriority() byte {
	return byte(f.Priority)
}

func (f FlowRequest) WireBudgetFlags() byte {
	var flags byte
	if f.Budget.PreferReducedFEC {
		flags |= BudgetFlagReducedFEC
	}
	if f.Budget.PreferFastAck {
		flags |= BudgetFlagFastACK
	}
	if f.Budget.PreferIndependentIO {
		flags |= BudgetFlagIndependentIO
	}
	return flags
}

func clampDurationMillis32(d time.Duration) uint32 {
	if d <= 0 {
		return 0
	}
	ms := d.Milliseconds()
	if ms > int64(^uint32(0)) {
		return ^uint32(0)
	}
	return uint32(ms)
}

func (f FlowRequest) WireReadTimeoutMs() uint32 {
	return clampDurationMillis32(f.Budget.ReadTimeout)
}

func (f FlowRequest) WireIdleAfterFirstByteMs() uint32 {
	return clampDurationMillis32(f.Budget.IdleAfterFirstByte)
}

func (f FlowRequest) WirePreferredCopies() byte {
	if f.Budget.PreferredCopies <= 0 {
		return 1
	}
	if f.Budget.PreferredCopies > 255 {
		return 255
	}
	return byte(f.Budget.PreferredCopies)
}

func ParseTarget(target string) (string, uint16) {
	host, portText, err := net.SplitHostPort(target)
	if err != nil {
		return target, 0
	}
	port, err := strconv.ParseUint(portText, 10, 16)
	if err != nil {
		return host, 0
	}
	return host, uint16(port)
}

func ClassifyFlow(kind FlowKind, target string) FlowRequest {
	host, port := ParseTarget(target)
	hostLC := strings.ToLower(strings.TrimSpace(host))
	req := FlowRequest{
		Kind:     kind,
		Target:   target,
		Host:     host,
		Port:     port,
		Class:    FlowClassBulkTraffic,
		Priority: PriorityBulk,
		Budget: StreamBudget{
			ReadTimeout:         10 * time.Minute,
			IdleAfterFirstByte:  15 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    false,
			PreferFastAck:       true,
			MaxBufferedBytes:    2 << 20,
			PreferIndependentIO: true,
		},
	}

	switch {
	case kind == FlowKindUDP && (port == 3478 || port == 3479 || port == 5349):
		req.Class = FlowClassControlPlane
		req.Priority = PriorityCritical
		req.Budget = StreamBudget{
			ReadTimeout:         3 * time.Minute,
			IdleAfterFirstByte:  3 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    true,
			PreferFastAck:       true,
			MaxBufferedBytes:    512 << 10,
			PreferIndependentIO: true,
		}
	case kind == FlowKindUDP && (port == 443 || port == 8443 || port == 8801 || port == 1935):
		req.Class = FlowClassMediaRealtime
		req.Priority = PriorityInteractive
		req.Budget = StreamBudget{
			ReadTimeout:         15 * time.Minute,
			IdleAfterFirstByte:  20 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    false,
			PreferFastAck:       false,
			MaxBufferedBytes:    8 << 20,
			PreferIndependentIO: false,
		}
	case strings.Contains(hostLC, "frontier.tiktokv.com"),
		strings.Contains(hostLC, "webcast-boot.tiktokv.com"),
		strings.Contains(hostLC, "pitaya-boot.tiktokv.com"),
		strings.Contains(hostLC, "api-boot.tiktokv.com"),
		strings.Contains(hostLC, "api-core-boot.tiktokv.com"),
		strings.Contains(hostLC, "mon-boot.tiktokv.com"),
		strings.Contains(hostLC, "mssdk-boot.tiktokv.com"),
		strings.Contains(hostLC, "jsb-boot.tiktokv.com"),
		strings.Contains(hostLC, "gecko-boot.tiktokv.com"),
		strings.Contains(hostLC, "bsync-va.tiktokv.com"),
		strings.Contains(hostLC, "pitayacdn.tiktokcdn.com"),
		strings.Contains(hostLC, "pkgcdn.pitaya-clientai.com"),
		strings.Contains(hostLC, "pitaya-clientai.com"):
		req.Class = FlowClassMediaBootstrap
		req.Priority = PriorityInteractive
		req.Budget = StreamBudget{
			ReadTimeout:         12 * time.Minute,
			IdleAfterFirstByte:  15 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    false,
			PreferFastAck:       true,
			MaxBufferedBytes:    4 << 20,
			PreferIndependentIO: true,
		}
	case strings.Contains(hostLC, "ip.sb"):
		req.Class = FlowClassProbeWeb
		req.Priority = PriorityCritical
		req.Budget = StreamBudget{
			ReadTimeout:         5 * time.Minute,
			IdleAfterFirstByte:  5 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    true,
			PreferFastAck:       true,
			MaxBufferedBytes:    1 << 20,
			PreferIndependentIO: true,
		}
	case strings.Contains(hostLC, "google.com"),
		strings.Contains(hostLC, "ocsp2.apple.com"),
		strings.Contains(hostLC, "updates.cdn-apple.com"),
		strings.Contains(hostLC, "gdmf.apple.com"),
		strings.Contains(hostLC, "bag.itunes.apple.com"),
		strings.Contains(hostLC, "itunes.apple.com"),
		strings.Contains(hostLC, "iphone-ld.apple.com"),
		strings.Contains(hostLC, "configuration.ls.apple.com"),
		strings.Contains(hostLC, "init.push.apple.com"),
		strings.Contains(hostLC, "gspe1-ssl.ls.apple.com"),
		strings.Contains(hostLC, "pagead2.googlesyndication.com"):
		req.Class = FlowClassProbeWeb
		req.Priority = PriorityCritical
		req.Budget = StreamBudget{
			ReadTimeout:         5 * time.Minute,
			IdleAfterFirstByte:  5 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    true,
			PreferFastAck:       true,
			MaxBufferedBytes:    1 << 20,
			PreferIndependentIO: true,
		}
	case port == 5223 && strings.Contains(hostLC, "courier.push.apple.com"),
		strings.Contains(hostLC, "time.apple.com"):
		req.Class = FlowClassProbePush
		req.Priority = PriorityInteractive
		req.Budget = StreamBudget{
			ReadTimeout:         5 * time.Minute,
			IdleAfterFirstByte:  10 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    true,
			PreferFastAck:       true,
			MaxBufferedBytes:    1 << 20,
			PreferIndependentIO: true,
		}
	case strings.Contains(hostLC, "pitayacdn.tiktokcdn.com"),
		strings.Contains(hostLC, "pkgcdn.pitaya-clientai.com"),
		strings.Contains(hostLC, "pitaya-clientai.com"):
		req.Class = FlowClassBulkTraffic
		req.Priority = PriorityDefault
		req.Budget = StreamBudget{
			ReadTimeout:         10 * time.Minute,
			IdleAfterFirstByte:  15 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    false,
			PreferFastAck:       true,
			MaxBufferedBytes:    4 << 20,
			PreferIndependentIO: true,
		}
	case strings.Contains(hostLC, "tnc-boot.tiktokv.com"),
		strings.Contains(hostLC, "vcs-boot.tiktokv.com"),
		strings.Contains(hostLC, "log-boot.tiktokv.com"),
		strings.Contains(hostLC, "inapp.tiktokv.com"),
		strings.Contains(hostLC, "sf16-website-login.neutral.ttwstatic.com"),
		strings.Contains(hostLC, "api16-normal-"):
		req.Class = FlowClassLoginCritical
		req.Priority = PriorityCritical
		req.Budget = StreamBudget{
			ReadTimeout:         10 * time.Minute,
			IdleAfterFirstByte:  15 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    true,
			PreferFastAck:       true,
			MaxBufferedBytes:    2 << 20,
			PreferIndependentIO: true,
		}
	case strings.Contains(hostLC, "tiktok"),
		strings.Contains(hostLC, "tiktokcdn"),
		strings.Contains(hostLC, "ttwstatic"),
		strings.Contains(hostLC, "pitaya-clientai"):
		req.Class = FlowClassInteractive
		req.Priority = PriorityInteractive
		req.Budget = StreamBudget{
			ReadTimeout:         10 * time.Minute,
			IdleAfterFirstByte:  15 * time.Minute,
			PreferredCopies:     1,
			PreferReducedFEC:    false,
			PreferFastAck:       true,
			MaxBufferedBytes:    2 << 20,
			PreferIndependentIO: true,
		}
	default:
		req.Class = FlowClassCompatFallback
		req.Priority = PriorityDefault
	}
	return req
}

func ClassifyFlowWithRules(kind FlowKind, target string, fc *FlowClassifier) FlowRequest {
	host, port := ParseTarget(target)
	hostLC := strings.ToLower(strings.TrimSpace(host))
	if fc == nil || !fc.Enabled || len(fc.Rules) == 0 {
		return ClassifyFlow(kind, target)
	}
	for _, rule := range fc.Rules {
		if rule.Kind != "" && rule.Kind != kind {
			continue
		}
		if len(rule.Ports) > 0 {
			matchedPort := false
			for _, p := range rule.Ports {
				if p == port {
					matchedPort = true
					break
				}
			}
			if !matchedPort {
				continue
			}
		}
		if len(rule.HostContainsAny) > 0 {
			matchedHost := false
			for _, token := range rule.HostContainsAny {
				if strings.Contains(hostLC, strings.ToLower(strings.TrimSpace(token))) {
					matchedHost = true
					break
				}
			}
			if !matchedHost {
				continue
			}
		}
		return FlowRequest{
			Kind:     kind,
			Target:   target,
			Host:     host,
			Port:     port,
			Class:    rule.Class,
			Priority: rule.Priority,
			Budget: StreamBudget{
				ReadTimeout:         rule.Budget.ReadTimeout,
				IdleAfterFirstByte:  rule.Budget.IdleAfterFirstByte,
				PreferredCopies:     rule.Budget.PreferredCopies,
				PreferReducedFEC:    rule.Budget.PreferReducedFEC,
				PreferFastAck:       rule.Budget.PreferFastAck,
				MaxBufferedBytes:    rule.Budget.MaxBufferedBytes,
				PreferIndependentIO: rule.Budget.PreferIndependentIO,
			},
		}
	}
	if kind == FlowKindTCP {
		switch port {
		case 80, 443, 5223:
			return FlowRequest{
				Kind:     kind,
				Target:   target,
				Host:     host,
				Port:     port,
				Class:    FlowClassInteractive,
				Priority: PriorityInteractive,
				Budget: StreamBudget{
					ReadTimeout:         5 * time.Minute,
					IdleAfterFirstByte:  5 * time.Minute,
					PreferredCopies:     1,
					PreferReducedFEC:    true,
					PreferFastAck:       true,
					MaxBufferedBytes:    1 << 20,
					PreferIndependentIO: true,
				},
			}
		}
	}
	if kind == FlowKindUDP && port == 123 {
		return FlowRequest{
			Kind:     kind,
			Target:   target,
			Host:     host,
			Port:     port,
			Class:    FlowClassProbePush,
			Priority: PriorityInteractive,
			Budget: StreamBudget{
				ReadTimeout:         5 * time.Minute,
				IdleAfterFirstByte:  10 * time.Minute,
				PreferredCopies:     1,
				PreferReducedFEC:    true,
				PreferFastAck:       true,
				MaxBufferedBytes:    1 << 20,
				PreferIndependentIO: true,
			},
		}
	}
	return FlowRequest{
		Kind:     kind,
		Target:   target,
		Host:     host,
		Port:     port,
		Class:    fc.FallbackClass,
		Priority: fc.FallbackPriority,
		Budget: StreamBudget{
			ReadTimeout:         fc.FallbackBudget.ReadTimeout,
			IdleAfterFirstByte:  fc.FallbackBudget.IdleAfterFirstByte,
			PreferredCopies:     fc.FallbackBudget.PreferredCopies,
			PreferReducedFEC:    fc.FallbackBudget.PreferReducedFEC,
			PreferFastAck:       fc.FallbackBudget.PreferFastAck,
			MaxBufferedBytes:    fc.FallbackBudget.MaxBufferedBytes,
			PreferIndependentIO: fc.FallbackBudget.PreferIndependentIO,
		},
	}
}

// BridgeCompatFlow is kept as a compatibility alias during the migration to a
// single canonical xgw core session model. Compatibility frontends must not
// maintain an independent traffic-control policy; they should reuse the same
// flow semantics as native xgw endpoints.
func BridgeCompatFlow(kind FlowKind, target string) FlowRequest {
	req := ClassifyFlow(kind, target)
	req.Budget.PreferIndependentIO = true
	if req.Budget.ReadTimeout < 10*time.Minute {
		req.Budget.ReadTimeout = 10 * time.Minute
	}
	if req.Budget.IdleAfterFirstByte < 15*time.Minute {
		req.Budget.IdleAfterFirstByte = 15 * time.Minute
	}
	return req
}
