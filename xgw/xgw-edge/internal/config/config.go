package config

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/local/xgw-edge/internal/coremodel"
)

type PoolNode struct {
	Name           string   `json:"name"`
	Address        string   `json:"address"`
	Region         string   `json:"region"`
	Role           string   `json:"role"`
	Protocol       string   `json:"protocol"`
	ProbeProtocol  string   `json:"probe_protocol"`
	ProbeAddr      string   `json:"probe_addr"`
	ProbePayload   string   `json:"probe_payload"`
	ProbeSamples   int      `json:"probe_samples"`
	Priority       int      `json:"priority"`
	Weight         int      `json:"weight"`
	BasePenalty    int      `json:"base_penalty"`
	CapacityMbps   int      `json:"capacity_mbps"`
	MaxSessions    int      `json:"max_sessions"`
	Tags           []string `json:"tags"`
	HealthURL      string   `json:"health_url"`
	BackendUDPAddr string   `json:"backend_udp_addr"`
}

type LineHop struct {
	Name           string `json:"name"`
	Role           string `json:"role"`
	PublicAddr     string `json:"public_addr"`
	PrivateAddr    string `json:"private_addr"`
	BackendUDPAddr string `json:"backend_udp_addr"`
}

type LineConfig struct {
	ID             string    `json:"id"`
	Name           string    `json:"name"`
	Address        string    `json:"address"`
	Region         string    `json:"region"`
	Protocol       string    `json:"protocol"`
	ProbeProtocol  string    `json:"probe_protocol"`
	Priority       int       `json:"priority"`
	Weight         int       `json:"weight"`
	BasePenalty    int       `json:"base_penalty"`
	CapacityMbps   int       `json:"capacity_mbps"`
	MaxSessions    int       `json:"max_sessions"`
	Tags           []string  `json:"tags"`
	HealthURL      string    `json:"health_url"`
	ProbeAddr      string    `json:"probe_addr"`
	ProbePayload   string    `json:"probe_payload"`
	ProbeSamples   int       `json:"probe_samples"`
	BackendUDPAddr string    `json:"backend_udp_addr"`
	Hops           []LineHop `json:"hops"`
}

type PoolConfig struct {
	Strategy              string       `json:"strategy"`
	ProbeInterval         Duration     `json:"probe_interval"`
	ProbeTimeout          Duration     `json:"probe_timeout"`
	ProbeSamples          int          `json:"probe_samples"`
	ScoreHalfLife         Duration     `json:"score_half_life"`
	BackendMetricsPath    string       `json:"backend_metrics_path"`
	PreferRegions         []string     `json:"prefer_regions"`
	RequiredTags          []string     `json:"required_tags"`
	EnableCapacityAware   bool         `json:"enable_capacity_aware"`
	EnableLatencyAware    bool         `json:"enable_latency_aware"`
	EnableJitterAware     bool         `json:"enable_jitter_aware"`
	EnableLossAware       bool         `json:"enable_loss_aware"`
	EnableBandwidthAware  bool         `json:"enable_bandwidth_aware"`
	EnableSessionPressure bool         `json:"enable_session_pressure"`
	Nodes                 []PoolNode   `json:"nodes"`
	Lines                 []LineConfig `json:"lines"`
}

type FlowClassifierBudget struct {
	ReadTimeoutMillis        int  `json:"read_timeout_millis"`
	IdleAfterFirstByteMillis int  `json:"idle_after_first_byte_millis"`
	PreferredCopies          int  `json:"preferred_copies"`
	PreferReducedFEC         bool `json:"prefer_reduced_fec"`
	PreferFastACK            bool `json:"prefer_fast_ack"`
	MaxBufferedBytes         int  `json:"max_buffered_bytes"`
	PreferIndependentIO      bool `json:"prefer_independent_io"`
}

type FlowClassifierRule struct {
	Name             string               `json:"name"`
	Kind             string               `json:"kind"`
	Ports            []uint16             `json:"ports"`
	HostContainsAny  []string             `json:"host_contains_any"`
	FlowClass        string               `json:"flow_class"`
	Priority         string               `json:"priority"`
	Budget           FlowClassifierBudget `json:"budget"`
}

type FlowClassifierConfig struct {
	Enabled       bool                 `json:"enabled"`
	Rules         []FlowClassifierRule `json:"rules"`
	FallbackClass string               `json:"fallback_class"`
	FallbackPrio  string               `json:"fallback_priority"`
	FallbackBudget FlowClassifierBudget `json:"fallback_budget"`
}

type MasqueradeConfig struct {
	UpstreamURL        string   `json:"upstream_url"`
	ServerNames        []string `json:"server_names"`
	AllowedPaths       []string `json:"allowed_paths"`
	PreferHTTP3        bool     `json:"prefer_http3"`
	FakeServerHeader   string   `json:"fake_server_header"`
	FakePoweredBy      string   `json:"fake_powered_by"`
	UnauthStatusCode   int      `json:"unauth_status_code"`
	UnauthorizedPassth bool     `json:"unauthorized_passthrough"`
}

type CompatibilityConfig struct {
	Mode               string `json:"mode"`
	ShadowrocketName   string `json:"shadowrocket_name"`
	EnableHysteriaAuth bool   `json:"enable_hysteria_auth"`
	EnableHysteriaUDP  bool   `json:"enable_hysteria_udp"`
	EnableHysteriaTCP  bool   `json:"enable_hysteria_tcp"`
	DirectOutboundTCP  bool   `json:"direct_outbound_tcp"`
	DirectOutboundUDP  bool   `json:"direct_outbound_udp"`
}

type NativeConfig struct {
	Mode        string `json:"mode"`
	ALPN        string `json:"alpn"`
	EnableTCP   bool   `json:"enable_tcp"`
	EnableUDP   bool   `json:"enable_udp"`
	RequireAuth bool   `json:"require_auth"`
}

type TLSConfig struct {
	CertFile                 string   `json:"cert_file"`
	KeyFile                  string   `json:"key_file"`
	ClientCAFile             string   `json:"client_ca_file"`
	ALPN                     []string `json:"alpn"`
	InsecureSkipVerify       bool     `json:"insecure_skip_verify"`
	MinVersion               string   `json:"min_version"`
	MaxVersion               string   `json:"max_version"`
	CipherSuites             []uint16 `json:"cipher_suites"`
	EnableSessionTickets     bool     `json:"enable_session_tickets"`
	PreferServerCipherSuites bool     `json:"prefer_server_cipher_suites"`
}

type ControlConfig struct {
	AuthHost            string   `json:"auth_host"`
	AuthPath            string   `json:"auth_path"`
	Token               string   `json:"token"`
	RequireAuth         bool     `json:"require_auth"`
	AdvertisedRxMbps    uint64   `json:"advertised_rx_mbps"`
	AdvertisedTxMbps    uint64   `json:"advertised_tx_mbps"`
	SupportedCongestion []string `json:"supported_congestion"`
	Keepalive           Duration `json:"keepalive"`
	IdleTimeout         Duration `json:"idle_timeout"`
	EnableMigration     bool     `json:"enable_migration"`
	RouteUpdateInterval Duration `json:"route_update_interval"`
	RouteStickinessMin  Duration `json:"route_stickiness_min"`
	RouteSwitchDelta    float64  `json:"route_switch_delta"`
	RouteEscapeMinScore float64  `json:"route_escape_min_score"`
}

type ServerConfig struct {
	Listen                  string              `json:"listen"`
	FrontendMode            string              `json:"frontend_mode"`
	MetricsListen           string              `json:"metrics_listen"`
	LogLevel                string              `json:"log_level"`
	LogFormat               string              `json:"log_format"`
	ConnectType             string              `json:"connect_type"`
	BridgeTransport         string              `json:"bridge_transport"`
	BackendUDPAddr          string              `json:"backend_udp_addr"`
	BridgeTCPAddr           string              `json:"bridge_tcp_addr"`
	BridgeUnixPath          string              `json:"bridge_unix_path"`
	BridgeRingPath          string              `json:"bridge_ring_path"`
	BridgeCopyMode          string              `json:"bridge_copy_mode"`
	BackendRouteControlPath string              `json:"backend_route_control_path"`
	BackendRouteControlMode string              `json:"backend_route_control_mode"`
	FlowClassifier          FlowClassifierConfig `json:"flow_classifier"`
	Pool                    PoolConfig          `json:"pool"`
	Masquerade              MasqueradeConfig    `json:"masquerade"`
	Native                  NativeConfig        `json:"native"`
	Compatibility           CompatibilityConfig `json:"compatibility"`
	TLS                     TLSConfig           `json:"tls"`
	Control                 ControlConfig       `json:"control"`
}

type ClientConfig struct {
	ServerURL         string              `json:"server_url"`
	FrontendMode      string              `json:"frontend_mode"`
	ServerName        string              `json:"server_name"`
	BootstrapNode     string              `json:"bootstrap_node"`
	LogLevel          string              `json:"log_level"`
	LogFormat         string              `json:"log_format"`
	LocalSOCKS5Listen string              `json:"local_socks5_listen"`
	LocalHTTPListen   string              `json:"local_http_listen"`
	LocalUDPListen    string              `json:"local_udp_listen"`
	LocalTUNName      string              `json:"local_tun_name"`
	LocalTUNAddress   string              `json:"local_tun_address"`
	LocalMode         string              `json:"local_mode"`
	FlowClassifier    FlowClassifierConfig `json:"flow_classifier"`
	Pool              PoolConfig          `json:"pool"`
	Masquerade        MasqueradeConfig    `json:"masquerade"`
	Native            NativeConfig        `json:"native"`
	Compatibility     CompatibilityConfig `json:"compatibility"`
	TLS               TLSConfig           `json:"tls"`
	Control           ControlConfig       `json:"control"`
	FixedBackendUDP   string              `json:"fixed_backend_udp"`
}

type Duration time.Duration

func (d *Duration) UnmarshalJSON(data []byte) error {
	text := strings.Trim(string(data), "\"")
	if text == "" || text == "null" {
		*d = 0
		return nil
	}
	parsed, err := time.ParseDuration(text)
	if err != nil {
		return fmt.Errorf("parse duration %q: %w", text, err)
	}
	*d = Duration(parsed)
	return nil
}

func (d Duration) Std() time.Duration {
	return time.Duration(d)
}

func DefaultFlowClassifier() FlowClassifierConfig {
	return FlowClassifierConfig{
		Enabled: true,
		Rules: []FlowClassifierRule{
			{
				Name:      "control-udp-probe-ports",
				Kind:      "udp",
				Ports:     []uint16{3478, 3479, 5349},
				FlowClass: "control-plane",
				Priority:  "critical",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((3 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((3 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         true,
					PreferFastACK:            true,
					MaxBufferedBytes:         512 << 10,
					PreferIndependentIO:      true,
				},
			},
			{
				Name:      "media-udp-realtime-ports",
				Kind:      "udp",
				Ports:     []uint16{443, 8443, 8801, 1935},
				FlowClass: "media-realtime",
				Priority:  "interactive",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((15 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((20 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         false,
					PreferFastACK:            false,
					MaxBufferedBytes:         8 << 20,
					PreferIndependentIO:      false,
				},
			},
			{
				Name:            "probe-web-ip-sb",
				HostContainsAny: []string{"ip.sb"},
				FlowClass:       "probe-web",
				Priority:        "critical",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((5 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((5 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         true,
					PreferFastACK:            true,
					MaxBufferedBytes:         1 << 20,
					PreferIndependentIO:      true,
				},
			},
			{
				Name: "probe-web-google-apple-ocsp",
				HostContainsAny: []string{
					"google.com",
					"ocsp2.apple.com",
					"updates.cdn-apple.com",
					"gdmf.apple.com",
					"bag.itunes.apple.com",
					"itunes.apple.com",
					"iphone-ld.apple.com",
					"configuration.ls.apple.com",
					"init.push.apple.com",
					"gspe1-ssl.ls.apple.com",
					"pagead2.googlesyndication.com",
				},
				FlowClass:       "probe-web",
				Priority:        "critical",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((5 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((5 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         true,
					PreferFastACK:            true,
					MaxBufferedBytes:         1 << 20,
					PreferIndependentIO:      true,
				},
			},
			{
				Name:            "probe-push-apple",
				HostContainsAny: []string{"courier.push.apple.com", "time.apple.com"},
				Ports:           []uint16{123, 443, 5223},
				FlowClass:       "probe-push",
				Priority:        "interactive",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((5 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((10 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         true,
					PreferFastACK:            true,
					MaxBufferedBytes:         1 << 20,
					PreferIndependentIO:      true,
				},
			},
			{
				Name: "tiktok-bulk-cdn",
				// 视频 CDN(大流量下载)必须归 bulk 拥塞域:避免被后面 tiktok-interactive-generic
				// 的 "tiktokcdn" 通配吞成 interactive→control slot,与控制流争同一拥塞域。
				// 现网实测视频分发域名:v9/v16m/v19.tiktokcdn.com、sfNN-va.tiktokcdn.com 等。
				HostContainsAny: []string{
					"pitayacdn.tiktokcdn.com", "pkgcdn.pitaya-clientai.com", "pitaya-clientai.com",
					"v9.tiktokcdn.com", "v16m.tiktokcdn.com", "v19.tiktokcdn.com",
					"v9-", "v16m-", "v19-", "-va.tiktokcdn.com", "sf16-va.tiktokcdn.com",
					"tiktokcdn-us.com", "muscdn.com", "ibyteimg.com", "ipstatp.com",
				},
				FlowClass:       "bulk",
				Priority:        "default",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((10 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((15 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         false,
					PreferFastACK:            true,
					MaxBufferedBytes:         4 << 20,
					PreferIndependentIO:      true,
				},
			},
			{
				Name: "tiktok-control-login-critical",
				HostContainsAny: []string{
					"tnc-boot.tiktokv.com",
					"vcs-boot.tiktokv.com",
					"log-boot.tiktokv.com",
					"inapp.tiktokv.com",
					"sf16-website-login.neutral.ttwstatic.com",
					"api16-normal-",
				},
				FlowClass: "login-critical",
				Priority:  "critical",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((10 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((15 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         true,
					PreferFastACK:            true,
					MaxBufferedBytes:         2 << 20,
					PreferIndependentIO:      true,
				},
			},
			{
				Name: "tiktok-media-bootstrap",
				HostContainsAny: []string{
					"api-boot.tiktokv.com",
					"api-core-boot.tiktokv.com",
					"mon-boot.tiktokv.com",
					"mssdk-boot.tiktokv.com",
					"jsb-boot.tiktokv.com",
					"gecko-boot.tiktokv.com",
					"bsync-va.tiktokv.com",
					"frontier.tiktokv.com",
					"webcast-boot.tiktokv.com",
					"pitaya-boot.tiktokv.com",
					"pitayacdn.tiktokcdn.com",
					"pkgcdn.pitaya-clientai.com",
					"pitaya-clientai.com",
				},
				FlowClass: "media-bootstrap",
				Priority:  "interactive",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((12 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((15 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         false,
					PreferFastACK:            true,
					MaxBufferedBytes:         4 << 20,
					PreferIndependentIO:      true,
				},
			},
			{
				Name:            "tiktok-interactive-generic",
				HostContainsAny: []string{"tiktok", "tiktokcdn", "ttwstatic", "pitaya-clientai"},
				FlowClass:       "interactive",
				Priority:        "interactive",
				Budget: FlowClassifierBudget{
					ReadTimeoutMillis:        int((10 * time.Minute) / time.Millisecond),
					IdleAfterFirstByteMillis: int((15 * time.Minute) / time.Millisecond),
					PreferredCopies:          1,
					PreferReducedFEC:         false,
					PreferFastACK:            true,
					MaxBufferedBytes:         2 << 20,
					PreferIndependentIO:      true,
				},
			},
		},
		FallbackClass: "compat-fallback",
		FallbackPrio:  "default",
		FallbackBudget: FlowClassifierBudget{
			ReadTimeoutMillis:        int((10 * time.Minute) / time.Millisecond),
			IdleAfterFirstByteMillis: int((15 * time.Minute) / time.Millisecond),
			PreferredCopies:          1,
			PreferReducedFEC:         false,
			PreferFastACK:            true,
			MaxBufferedBytes:         2 << 20,
			PreferIndependentIO:      true,
		},
	}
}

func DefaultServer() ServerConfig {
	return ServerConfig{
		Listen:                  ":8443",
		FrontendMode:            "xgw-native",
		MetricsListen:           ":9095",
		LogLevel:                "info",
		LogFormat:               "json",
		ConnectType:             "direct",
		BridgeTransport:         "shm-direct",
		BackendUDPAddr:          "127.0.0.1:51830",
		BridgeTCPAddr:           "127.0.0.1:19080",
		BridgeUnixPath:          "/run/xgw/bridge.sock",
		BridgeRingPath:          "/run/xgw/bridge-ring",
		BridgeCopyMode:          "auto",
		BackendRouteControlPath: "/run/xgw/route-control",
		BackendRouteControlMode: "observe",
		FlowClassifier:          DefaultFlowClassifier(),
		Pool: PoolConfig{
			Strategy:              "weighted-score",
			ProbeInterval:         Duration(10 * time.Second),
			ProbeTimeout:          Duration(3 * time.Second),
			ProbeSamples:          3,
			ScoreHalfLife:         Duration(60 * time.Second),
			EnableCapacityAware:   true,
			EnableLatencyAware:    true,
			EnableJitterAware:     true,
			EnableLossAware:       true,
			EnableBandwidthAware:  true,
			EnableSessionPressure: true,
		},
		Masquerade: MasqueradeConfig{
			UpstreamURL:        "https://www.example.com",
			ServerNames:        []string{"www.example.com"},
			AllowedPaths:       []string{"/", "/assets", "/favicon.ico"},
			PreferHTTP3:        true,
			FakeServerHeader:   "nginx",
			FakePoweredBy:      "",
			UnauthStatusCode:   200,
			UnauthorizedPassth: true,
		},
		Native: NativeConfig{
			Mode:        "xgw-native",
			ALPN:        "xgw/1",
			EnableTCP:   true,
			EnableUDP:   true,
			RequireAuth: true,
		},
		Compatibility: CompatibilityConfig{
			Mode:               "native",
			ShadowrocketName:   "xgw-edge",
			EnableHysteriaAuth: true,
			EnableHysteriaUDP:  true,
			EnableHysteriaTCP:  true,
			DirectOutboundTCP:  false,
			DirectOutboundUDP:  false,
		},
		TLS: TLSConfig{
			ALPN:                 []string{"xgw/1", "h3", "h3-29"},
			MinVersion:           "1.3",
			EnableSessionTickets: true,
		},
		Control: ControlConfig{
			AuthHost:            "hysteria",
			AuthPath:            "/auth",
			RequireAuth:         true,
			AdvertisedRxMbps:    0,
			AdvertisedTxMbps:    0,
			SupportedCongestion: []string{"bbr", "brutal", "reno"},
			Keepalive:           Duration(10 * time.Second),
			IdleTimeout:         Duration(30 * time.Second),
			EnableMigration:     true,
			RouteUpdateInterval: Duration(15 * time.Second),
			RouteStickinessMin:  Duration(20 * time.Second),
			RouteSwitchDelta:    15,
			RouteEscapeMinScore: 40,
		},
	}
}

func DefaultClient() ClientConfig {
	return ClientConfig{
		ServerURL:         "https://127.0.0.1:8443",
		FrontendMode:      "xgw-native",
		ServerName:        "www.example.com",
		LogLevel:          "info",
		LogFormat:         "json",
		LocalSOCKS5Listen: "127.0.0.1:10808",
		LocalHTTPListen:   "127.0.0.1:18080",
		LocalMode:         "udp",
		LocalUDPListen:    "127.0.0.1:0",
		LocalTUNName:      "xgwedge0",
		LocalTUNAddress:   "10.254.0.1/24",
		FlowClassifier:    DefaultFlowClassifier(),
		Pool: PoolConfig{
			Strategy:              "weighted-score",
			ProbeInterval:         Duration(10 * time.Second),
			ProbeTimeout:          Duration(3 * time.Second),
			ProbeSamples:          3,
			ScoreHalfLife:         Duration(60 * time.Second),
			EnableCapacityAware:   true,
			EnableLatencyAware:    true,
			EnableJitterAware:     true,
			EnableLossAware:       true,
			EnableBandwidthAware:  true,
			EnableSessionPressure: true,
		},
		Masquerade: MasqueradeConfig{
			ServerNames:  []string{"www.example.com"},
			AllowedPaths: []string{"/", "/assets", "/favicon.ico"},
			PreferHTTP3:  true,
		},
		Native: NativeConfig{
			Mode:        "xgw-native",
			ALPN:        "xgw/1",
			EnableTCP:   true,
			EnableUDP:   true,
			RequireAuth: true,
		},
		Compatibility: CompatibilityConfig{
			Mode:               "native",
			ShadowrocketName:   "xgw-edge",
			EnableHysteriaAuth: true,
			EnableHysteriaUDP:  true,
			EnableHysteriaTCP:  true,
		},
		TLS: TLSConfig{
			ALPN:                 []string{"xgw/1", "h3", "h3-29"},
			MinVersion:           "1.3",
			EnableSessionTickets: true,
		},
		Control: ControlConfig{
			AuthHost:            "hysteria",
			AuthPath:            "/auth",
			RequireAuth:         true,
			SupportedCongestion: []string{"bbr", "brutal", "reno"},
			Keepalive:           Duration(10 * time.Second),
			IdleTimeout:         Duration(30 * time.Second),
			EnableMigration:     true,
			RouteUpdateInterval: 0,
			RouteStickinessMin:  Duration(20 * time.Second),
			RouteSwitchDelta:    15,
			RouteEscapeMinScore: 40,
		},
	}
}

func LoadServer(path string) (ServerConfig, error) {
	cfg := DefaultServer()
	if path == "" {
		cfg.Normalize()
		return cfg, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return ServerConfig{}, err
	}
	data = bytes.TrimPrefix(data, []byte{0xef, 0xbb, 0xbf})
	if err := json.Unmarshal(data, &cfg); err != nil {
		return ServerConfig{}, err
	}
	cfg.Normalize()
	return cfg, nil
}

func LoadClient(path string) (ClientConfig, error) {
	cfg := DefaultClient()
	if path == "" {
		cfg.Normalize()
		return cfg, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return ClientConfig{}, err
	}
	data = bytes.TrimPrefix(data, []byte{0xef, 0xbb, 0xbf})
	if err := json.Unmarshal(data, &cfg); err != nil {
		return ClientConfig{}, err
	}
	cfg.Normalize()
	return cfg, nil
}

func (c *ServerConfig) Normalize() {
	c.FrontendMode = strings.TrimSpace(c.FrontendMode)
	c.Compatibility.Mode = strings.TrimSpace(c.Compatibility.Mode)
	c.Native.Mode = strings.TrimSpace(c.Native.Mode)
	c.Native.ALPN = strings.TrimSpace(c.Native.ALPN)
	c.ConnectType = strings.TrimSpace(c.ConnectType)
	c.BridgeTransport = strings.TrimSpace(c.BridgeTransport)
	c.BridgeCopyMode = strings.TrimSpace(c.BridgeCopyMode)
	c.BackendRouteControlPath = strings.TrimSpace(c.BackendRouteControlPath)
	c.BackendRouteControlMode = strings.ToLower(strings.TrimSpace(c.BackendRouteControlMode))
	if c.Native.Mode == "" {
		c.Native.Mode = "xgw-native"
	}
	if c.Native.ALPN == "" {
		c.Native.ALPN = "xgw/1"
	}
	if c.FrontendMode == "" {
		if c.Compatibility.Mode == "hy2-official-bridge" {
			c.FrontendMode = "hy2-official-bridge"
		} else {
			c.FrontendMode = c.Native.Mode
		}
	}
	if c.ConnectType == "" {
		if c.FrontendMode == "hy2-official-bridge" {
			c.ConnectType = "bridge"
		} else {
			c.ConnectType = "direct"
		}
	}
	if c.BridgeTransport == "" {
		c.BridgeTransport = "shm-direct"
	}
	if c.BridgeTCPAddr == "" {
		c.BridgeTCPAddr = "127.0.0.1:19080"
	}
	if c.BridgeUnixPath == "" {
		c.BridgeUnixPath = "/run/xgw/bridge.sock"
	}
	if c.BridgeRingPath == "" {
		c.BridgeRingPath = "/run/xgw/bridge-ring"
	}
	if c.BridgeCopyMode == "" {
		c.BridgeCopyMode = "auto"
	}
	if c.BackendRouteControlPath == "" {
		c.BackendRouteControlPath = "/run/xgw/route-control"
	}
	if c.BackendRouteControlMode == "" {
		c.BackendRouteControlMode = "observe"
	}
	if len(c.TLS.ALPN) == 0 {
		c.TLS.ALPN = []string{c.Native.ALPN, "h3", "h3-29"}
	} else if c.FrontendMode != "hy2-official-bridge" && !containsString(c.TLS.ALPN, c.Native.ALPN) {
		c.TLS.ALPN = append([]string{c.Native.ALPN}, c.TLS.ALPN...)
	}
}

func (c *ClientConfig) Normalize() {
	c.FrontendMode = strings.TrimSpace(c.FrontendMode)
	c.Compatibility.Mode = strings.TrimSpace(c.Compatibility.Mode)
	c.Native.Mode = strings.TrimSpace(c.Native.Mode)
	c.Native.ALPN = strings.TrimSpace(c.Native.ALPN)
	if c.Native.Mode == "" {
		c.Native.Mode = "xgw-native"
	}
	if c.Native.ALPN == "" {
		c.Native.ALPN = "xgw/1"
	}
	if c.FrontendMode == "" {
		if c.Compatibility.Mode == "hy2-official-bridge" {
			c.FrontendMode = "hy2-official-bridge"
		} else {
			c.FrontendMode = c.Native.Mode
		}
	}
	if len(c.TLS.ALPN) == 0 {
		c.TLS.ALPN = []string{c.Native.ALPN, "h3", "h3-29"}
	} else if c.FrontendMode != "hy2-official-bridge" && !containsString(c.TLS.ALPN, c.Native.ALPN) {
		c.TLS.ALPN = append([]string{c.Native.ALPN}, c.TLS.ALPN...)
	}
}

func containsString(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}

func parsePriorityClass(value string) coremodel.PriorityClass {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "critical":
		return coremodel.PriorityCritical
	case "interactive":
		return coremodel.PriorityInteractive
	case "default":
		return coremodel.PriorityDefault
	default:
		return coremodel.PriorityBulk
	}
}

func parseFlowClass(value string) coremodel.FlowClass {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "control-plane":
		return coremodel.FlowClassControlPlane
	case "media-realtime":
		return coremodel.FlowClassMediaRealtime
	case "media-bootstrap":
		return coremodel.FlowClassMediaBootstrap
	case "probe-web":
		return coremodel.FlowClassProbeWeb
	case "probe-push":
		return coremodel.FlowClassProbePush
	case "login-critical":
		return coremodel.FlowClassLoginCritical
	case "interactive":
		return coremodel.FlowClassInteractive
	case "compat-fallback":
		return coremodel.FlowClassCompatFallback
	default:
		return coremodel.FlowClassBulkTraffic
	}
}

func (b FlowClassifierBudget) toCore() coremodel.ClassifierBudget {
	return coremodel.ClassifierBudget{
		ReadTimeout:         time.Duration(b.ReadTimeoutMillis) * time.Millisecond,
		IdleAfterFirstByte:  time.Duration(b.IdleAfterFirstByteMillis) * time.Millisecond,
		PreferredCopies:     b.PreferredCopies,
		PreferReducedFEC:    b.PreferReducedFEC,
		PreferFastAck:       b.PreferFastACK,
		MaxBufferedBytes:    b.MaxBufferedBytes,
		PreferIndependentIO: b.PreferIndependentIO,
	}
}

func (f FlowClassifierConfig) ToCore() coremodel.FlowClassifier {
	rules := make([]coremodel.ClassifierRule, 0, len(f.Rules))
	for _, rule := range f.Rules {
		kind := coremodel.FlowKind("")
		if strings.EqualFold(rule.Kind, "tcp") {
			kind = coremodel.FlowKindTCP
		} else if strings.EqualFold(rule.Kind, "udp") {
			kind = coremodel.FlowKindUDP
		}
		rules = append(rules, coremodel.ClassifierRule{
			Name:            rule.Name,
			Kind:            kind,
			Ports:           append([]uint16(nil), rule.Ports...),
			HostContainsAny: append([]string(nil), rule.HostContainsAny...),
			Class:           parseFlowClass(rule.FlowClass),
			Priority:        parsePriorityClass(rule.Priority),
			Budget:          rule.Budget.toCore(),
		})
	}
	return coremodel.FlowClassifier{
		Enabled:          f.Enabled,
		Rules:            rules,
		FallbackClass:    parseFlowClass(f.FallbackClass),
		FallbackPriority: parsePriorityClass(f.FallbackPrio),
		FallbackBudget:   f.FallbackBudget.toCore(),
	}
}
