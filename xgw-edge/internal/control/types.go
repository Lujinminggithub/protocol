package control

type AuthRequest struct {
	Token                string   `json:"token"`
	NodeID               string   `json:"node_id"`
	RequestedNode        string   `json:"requested_node"`
	RequestedCongestion  []string `json:"requested_congestion"`
	AdvertisedRxMbps     uint64   `json:"advertised_rx_mbps"`
	AdvertisedTxMbps     uint64   `json:"advertised_tx_mbps"`
	RequestedServerNames []string `json:"requested_server_names"`
	RequestedTags        []string `json:"requested_tags"`
}

type RouteCandidate struct {
	Name           string     `json:"name"`
	LineID         string     `json:"line_id,omitempty"`
	Address        string     `json:"address"`
	Region         string     `json:"region"`
	Protocol       string     `json:"protocol"`
	Score          float64    `json:"score"`
	Reasons        []string   `json:"reasons"`
	BackendUDPAddr string     `json:"backend_udp_addr"`
	Hops           []RouteHop `json:"hops,omitempty"`
}

type RouteHop struct {
	Name           string `json:"name"`
	Role           string `json:"role"`
	PublicAddr     string `json:"public_addr,omitempty"`
	PrivateAddr    string `json:"private_addr,omitempty"`
	BackendUDPAddr string `json:"backend_udp_addr,omitempty"`
}

type StreamBudgetSnapshot struct {
	FlowClass            string `json:"flow_class"`
	Priority             string `json:"priority"`
	PreferredCopies      int    `json:"preferred_copies"`
	ReadTimeoutMillis    int    `json:"read_timeout_millis"`
	IdleAfterFirstByteMs int    `json:"idle_after_first_byte_millis"`
	ReducedFEC           bool   `json:"reduced_fec"`
	FastACK              bool   `json:"fast_ack"`
	IndependentIO        bool   `json:"independent_io"`
}

type SchedulerSnapshot struct {
	Mode                 string `json:"mode"`
	PerStreamAccounting  bool   `json:"per_stream_accounting"`
	DeficitRoundRobin    bool   `json:"deficit_round_robin"`
	StarvationProtection bool   `json:"starvation_protection"`
	SessionCCCoupled     bool   `json:"session_cc_coupled"`
}

type AuthResponse struct {
	OK                   bool                 `json:"ok"`
	SessionID            string               `json:"session_id"`
	SelectedCongestion   string               `json:"selected_congestion"`
	AdvertisedRxMbps     uint64               `json:"advertised_rx_mbps"`
	AdvertisedTxMbps     uint64               `json:"advertised_tx_mbps"`
	SelectedRoute        RouteCandidate       `json:"selected_route"`
	AlternateRoutes      []RouteCandidate     `json:"alternate_routes,omitempty"`
	KeepaliveSec         int                  `json:"keepalive_sec"`
	IdleTimeoutSec       int                  `json:"idle_timeout_sec"`
	AllowMigration       bool                 `json:"allow_migration"`
	AllowDatagrams       bool                 `json:"allow_datagrams"`
	UnauthorizedFallback bool                 `json:"unauthorized_fallback"`
	DefaultBudget        StreamBudgetSnapshot `json:"default_budget"`
	Scheduler            SchedulerSnapshot    `json:"scheduler"`
}

type Keepalive struct {
	SessionID       string `json:"session_id"`
	LatestRTTMillis int64  `json:"latest_rtt_millis"`
	LossPPM         int64  `json:"loss_ppm"`
	RxMbps          uint64 `json:"rx_mbps"`
	TxMbps          uint64 `json:"tx_mbps"`
	InFlightBytes   uint64 `json:"inflight_bytes"`
	SendCreditBytes uint64 `json:"send_credit_bytes"`
	AckCreditFrames uint32 `json:"ack_credit_frames"`
}

type RouteUpdate struct {
	SessionID          string               `json:"session_id"`
	SelectedRoute      RouteCandidate       `json:"selected_route"`
	AlternateRoutes    []RouteCandidate     `json:"alternate_routes,omitempty"`
	SelectedCongestion string               `json:"selected_congestion"`
	ReconnectHint      string               `json:"reconnect_hint"`
	Reason             string               `json:"reason"`
	DefaultBudget      StreamBudgetSnapshot `json:"default_budget"`
	Scheduler          SchedulerSnapshot    `json:"scheduler"`
}
