package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/local/xgw-edge/internal/sim"
	"github.com/lxn/walk"
	. "github.com/lxn/walk/declarative"
	"go.uber.org/zap"
)

type timelineEntry struct {
	Label       string
	SampleIndex int
}

type uiState struct {
	mw *walk.MainWindow

	configEdit             *walk.LineEdit
	scenarioCombo          *walk.ComboBox
	mediaTargetEdit        *walk.LineEdit
	mediaRateEdit          *walk.LineEdit
	mediaPayloadEdit       *walk.LineEdit
	controlUDPTargetEdit   *walk.LineEdit
	controlUDPIntervalEdit *walk.LineEdit
	controlUDPTimeoutEdit  *walk.LineEdit
	controlTCPTargetEdit   *walk.LineEdit
	controlTCPIntervalEdit *walk.LineEdit
	controlTCPTimeoutEdit  *walk.LineEdit
	controlTCPPayloadEdit  *walk.LineEdit
	controlTCPReadEdit     *walk.LineEdit
	burstRateEdit          *walk.LineEdit
	burstDurationEdit      *walk.LineEdit
	enableTCPCheck         *walk.CheckBox

	statusLabel    *walk.Label
	routeText      *walk.TextEdit
	autoText       *walk.TextEdit
	mediaText      *walk.TextEdit
	controlText    *walk.TextEdit
	receiveText    *walk.TextEdit
	chartText      *walk.TextEdit
	summaryText    *walk.TextEdit
	detailText     *walk.TextEdit
	eventListBox   *walk.ListBox
	summaryListBox *walk.ListBox

	startBtn             *walk.PushButton
	stopBtn              *walk.PushButton
	saveBtn              *walk.PushButton
	liveStartBurstBtn    *walk.PushButton
	auctionStartBurstBtn *walk.PushButton
	highFreqBurstBtn     *walk.PushButton
	mediaPeakBurstBtn    *walk.PushButton

	engine         *sim.Engine
	logger         *zap.SugaredLogger
	refreshCtx     context.Context
	refreshStop    context.CancelFunc
	history        []sim.DeltaSnapshot
	lastSnapshot   *sim.Snapshot
	eventEntries   []timelineEntry
	summaryEntries []timelineEntry
}

func main() {
	zl, err := zap.NewDevelopment()
	if err != nil {
		panic(err)
	}
	defer zl.Sync()

	state := &uiState{logger: zl.Sugar()}

	if err := (MainWindow{
		AssignTo: &state.mw,
		Title:    "XGW Windows Local Simulator",
		MinSize:  Size{Width: 1380, Height: 1000},
		Layout:   VBox{},
		Children: []Widget{
			GroupBox{
				Title:  "Base Config",
				Layout: Grid{Columns: 4},
				Children: []Widget{
					Label{Text: "Client Config"},
					LineEdit{AssignTo: &state.configEdit, Text: defaultConfigPath()},
					PushButton{
						Text: "Browse",
						OnClicked: func() {
							dlg := new(walk.FileDialog)
							dlg.Filter = "JSON (*.json)|*.json|All Files (*.*)|*.*"
							dlg.Title = "Select Client Config"
							if ok, _ := dlg.ShowOpen(state.mw); ok {
								state.configEdit.SetText(dlg.FilePath)
							}
						},
					},
					Label{Text: "Use existing xgw-edge client.json"},

					Label{Text: "Scenario"},
					ComboBox{
						AssignTo:     &state.scenarioCombo,
						Model:        []string{"mixed", "media", "control-udp", "control-tcp"},
						CurrentIndex: 3,
						Editable:     false,
					},
					Label{Text: "Status"},
					Label{AssignTo: &state.statusLabel, Text: "Idle"},
				},
			},
			GroupBox{
				Title:  "Media Stream",
				Layout: Grid{Columns: 4},
				Children: []Widget{
					Label{Text: "Media Target"},
					LineEdit{AssignTo: &state.mediaTargetEdit, Text: "106.75.143.135:19000"},
					Label{Text: "Base Rate (Mbps)"},
					LineEdit{AssignTo: &state.mediaRateEdit, Text: "4.0"},

					Label{Text: "Payload Bytes"},
					LineEdit{AssignTo: &state.mediaPayloadEdit, Text: "1100"},
					Label{Text: "Peak Burst Mbps"},
					LineEdit{AssignTo: &state.burstRateEdit, Text: "30"},

					Label{Text: "Burst Duration"},
					LineEdit{AssignTo: &state.burstDurationEdit, Text: "3s"},
					Label{Text: ""},
					Label{Text: "Default baseline: ~4 Mbps, random / manual peak: 30 Mbps"},
				},
			},
			GroupBox{
				Title:  "Control Flow",
				Layout: Grid{Columns: 4},
				Children: []Widget{
					Label{Text: "UDP Echo Target"},
					LineEdit{AssignTo: &state.controlUDPTargetEdit, Text: "106.75.143.135:19000"},
					Label{Text: "UDP Interval"},
					LineEdit{AssignTo: &state.controlUDPIntervalEdit, Text: "2s"},

					Label{Text: "UDP Timeout"},
					LineEdit{AssignTo: &state.controlUDPTimeoutEdit, Text: "3s"},
					CheckBox{AssignTo: &state.enableTCPCheck, Text: "Enable TCP Control Probe", Checked: true},
					Label{Text: ""},

					Label{Text: "TCP Target"},
					LineEdit{AssignTo: &state.controlTCPTargetEdit, Text: "1.1.1.1:80"},
					Label{Text: "TCP Interval"},
					LineEdit{AssignTo: &state.controlTCPIntervalEdit, Text: "5s"},

					Label{Text: "TCP Timeout"},
					LineEdit{AssignTo: &state.controlTCPTimeoutEdit, Text: "5s"},
					Label{Text: "TCP Payload"},
					LineEdit{AssignTo: &state.controlTCPPayloadEdit, Text: ""},

					Label{Text: "TCP Read Bytes"},
					LineEdit{AssignTo: &state.controlTCPReadEdit, Text: "0"},
					Label{Text: ""},
					Label{Text: "Use for open-live / auction / bid control action simulation"},
				},
			},
			Composite{
				Layout: HBox{},
				Children: []Widget{
					PushButton{
						AssignTo: &state.startBtn,
						Text:     "Start",
						OnClicked: func() {
							if err := state.start(); err != nil {
								walk.MsgBox(state.mw, "Start Failed", err.Error(), walk.MsgBoxIconError)
							}
						},
					},
					PushButton{
						AssignTo:  &state.stopBtn,
						Text:      "Stop",
						Enabled:   false,
						OnClicked: func() { state.stop() },
					},
					PushButton{
						AssignTo: &state.saveBtn,
						Text:     "Save Record",
						Enabled:  false,
						OnClicked: func() {
							if err := state.saveRecord(); err != nil {
								walk.MsgBox(state.mw, "Save Failed", err.Error(), walk.MsgBoxIconError)
							} else {
								walk.MsgBox(state.mw, "Saved", "Record exported.", walk.MsgBoxIconInformation)
							}
						},
					},
				},
			},
			GroupBox{
				Title:  "Manual Burst Actions",
				Layout: HBox{},
				Children: []Widget{
					PushButton{AssignTo: &state.liveStartBurstBtn, Text: "Simulate Open Live", Enabled: false, OnClicked: func() { state.triggerBurst("live-start", "manual action: open live") }},
					PushButton{AssignTo: &state.auctionStartBurstBtn, Text: "Simulate Auction Start", Enabled: false, OnClicked: func() { state.triggerBurst("auction-start", "manual action: auction start") }},
					PushButton{AssignTo: &state.highFreqBurstBtn, Text: "High-Freq Small Packet Burst", Enabled: false, OnClicked: func() { state.triggerBurst("high-freq", "manual action: high-frequency small packet burst") }},
					PushButton{AssignTo: &state.mediaPeakBurstBtn, Text: "30Mbps Media Burst", Enabled: false, OnClicked: func() { state.triggerMediaPeakBurst() }},
				},
			},
			Composite{
				Layout: Grid{Columns: 2},
				Children: []Widget{
					GroupBox{Title: "Route / Session", Layout: VBox{}, Children: []Widget{TextEdit{AssignTo: &state.routeText, ReadOnly: true, VScroll: true}}},
					GroupBox{Title: "Auto Scenario", Layout: VBox{}, Children: []Widget{TextEdit{AssignTo: &state.autoText, ReadOnly: true, VScroll: true}}},
					GroupBox{Title: "Media Metrics", Layout: VBox{}, Children: []Widget{TextEdit{AssignTo: &state.mediaText, ReadOnly: true, VScroll: true}}},
					GroupBox{Title: "Control Metrics", Layout: VBox{}, Children: []Widget{TextEdit{AssignTo: &state.controlText, ReadOnly: true, VScroll: true}}},
					GroupBox{Title: "Receive / Overview", Layout: VBox{}, Children: []Widget{TextEdit{AssignTo: &state.receiveText, ReadOnly: true, VScroll: true}}},
					GroupBox{Title: "Mini Charts", Layout: VBox{}, Children: []Widget{TextEdit{AssignTo: &state.chartText, ReadOnly: true, VScroll: true}}},
					GroupBox{Title: "Report Summary", Layout: VBox{}, Children: []Widget{TextEdit{AssignTo: &state.summaryText, ReadOnly: true, VScroll: true}}},
				},
			},
			Composite{
				Layout: Grid{Columns: 2},
				Children: []Widget{
					GroupBox{
						Title:  "Event Timeline (click to jump to sample window)",
						Layout: VBox{},
						Children: []Widget{
							ListBox{
								AssignTo:              &state.eventListBox,
								Model:                 []string{},
								OnCurrentIndexChanged: func() { state.showSelectedTimelineDetail(true) },
							},
						},
					},
					GroupBox{
						Title:  "Summary Windows (click to jump to sample window)",
						Layout: VBox{},
						Children: []Widget{
							ListBox{
								AssignTo:              &state.summaryListBox,
								Model:                 []string{},
								OnCurrentIndexChanged: func() { state.showSelectedTimelineDetail(false) },
							},
						},
					},
					GroupBox{
						Title:  "Sample Window Detail",
						Layout: VBox{},
						Children: []Widget{
							TextEdit{AssignTo: &state.detailText, ReadOnly: true, VScroll: true},
						},
					},
				},
			},
		},
	}.Create()); err != nil {
		panic(err)
	}

	state.routeText.SetText("Waiting...")
	state.autoText.SetText("Waiting...")
	state.mediaText.SetText("Waiting...")
	state.controlText.SetText("Waiting...")
	state.receiveText.SetText("Waiting...")
	state.chartText.SetText("Waiting...")
	state.summaryText.SetText("Waiting...")
	state.detailText.SetText("Click an event or summary item to inspect its sample window.")
	state.mw.Run()
	state.stop()
}

func (s *uiState) start() error {
	s.stop()
	s.history = nil
	s.lastSnapshot = nil
	s.eventEntries = nil
	s.summaryEntries = nil
	s.eventListBox.SetModel([]string{})
	s.summaryListBox.SetModel([]string{})
	s.detailText.SetText("Collecting samples...")

	opts, err := s.collectOptions()
	if err != nil {
		return err
	}
	s.startBtn.SetEnabled(false)
	s.appendEventEntry("simulation started: scenario="+opts.Scenario, -1)
	s.statusLabel.SetText("Connecting...")
	go s.startAsync(opts)
	return nil
}

func (s *uiState) startAsync(opts sim.Options) {
	engine, err := sim.NewEngine(opts, s.logger)
	if err != nil {
		s.mw.Synchronize(func() {
			s.statusLabel.SetText("Start Failed")
			s.startBtn.SetEnabled(true)
			walk.MsgBox(s.mw, "Start Failed", err.Error(), walk.MsgBoxIconError)
		})
		return
	}
	if err := engine.Start(); err != nil {
		s.mw.Synchronize(func() {
			s.statusLabel.SetText("Start Failed")
			s.startBtn.SetEnabled(true)
			s.appendEventEntry("start failed: "+err.Error(), -1)
			walk.MsgBox(s.mw, "Start Failed", err.Error(), walk.MsgBoxIconError)
		})
		return
	}
	s.mw.Synchronize(func() {
		s.engine = engine
		s.statusLabel.SetText("Running")
		s.stopBtn.SetEnabled(true)
		s.saveBtn.SetEnabled(true)
		s.liveStartBurstBtn.SetEnabled(true)
		s.auctionStartBurstBtn.SetEnabled(true)
		s.highFreqBurstBtn.SetEnabled(true)
		s.mediaPeakBurstBtn.SetEnabled(true)
		s.refreshCtx, s.refreshStop = context.WithCancel(context.Background())
		go s.refreshLoop()
	})
}

func (s *uiState) stop() {
	if s.refreshStop != nil {
		s.refreshStop()
		s.refreshStop = nil
	}
	if s.engine != nil {
		s.engine.Stop()
		s.engine = nil
	}
	s.statusLabel.SetText("Stopped")
	s.startBtn.SetEnabled(true)
	s.stopBtn.SetEnabled(false)
	s.liveStartBurstBtn.SetEnabled(false)
	s.auctionStartBurstBtn.SetEnabled(false)
	s.highFreqBurstBtn.SetEnabled(false)
	s.mediaPeakBurstBtn.SetEnabled(false)
	s.saveBtn.SetEnabled(len(s.history) > 0)
}

func (s *uiState) refreshLoop() {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()
	last := time.Now()
	for {
		select {
		case <-s.refreshCtx.Done():
			return
		case now := <-ticker.C:
			if s.engine == nil {
				return
			}
			delta := now.Sub(last)
			last = now
			snap := s.engine.DeltaSnapshot(delta)
			s.mw.Synchronize(func() {
				s.captureEvents(snap)
				s.appendHistory(snap)
				s.renderSnapshot(snap)
			})
		}
	}
}

func (s *uiState) renderSnapshot(snap sim.DeltaSnapshot) {
	total := snap.Total
	uptime := total.Now.Sub(total.Start).Round(time.Second)
	s.statusLabel.SetText("Running | " + uptime.String())

	s.routeText.SetText(fmt.Sprintf(
		"Time: %s\r\nUptime: %s\r\n\r\nSession: %s\r\nRoute: %s\r\nLine: %s\r\nAddress: %s\r\nCongestion: %s\r\nSwitches: %d\r\nLast Switch: %s\r\nLast Route Reason: %s\r\n\r\nFlow Class: %s\r\nBudget Priority: %s\r\nPreferred Copies: %d\r\nRead Timeout: %d ms\r\nIdle After First Byte: %d ms\r\nReduced FEC: %t\r\nFast ACK: %t\r\nIndependent IO: %t\r\n\r\nScheduler Mode: %s\r\nPer-Stream Accounting: %t\r\nDRR: %t\r\nStarvation Protect: %t\r\nSession CC Coupled: %t\r\n\r\nKeepalive Inflight: %d B\r\nKeepalive Send Credit: %d B\r\nKeepalive ACK Debt: %d\r\n\r\nBurst: %s\r\nBurst Active: %t\r\nBurst Rate: %.2f Mbps\r\nBurst Ends: %s",
		total.Now.Format(time.RFC3339),
		uptime,
		emptyDash(total.Route.SessionID),
		emptyDash(total.Route.RouteName),
		emptyDash(total.Route.LineID),
		emptyDash(total.Route.Address),
		emptyDash(total.Route.Congestion),
		total.Route.SwitchCount,
		formatTime(total.Route.LastSwitchAt),
		emptyDash(total.Route.LastRouteReason),
		emptyDash(total.Route.DefaultBudget.FlowClass),
		emptyDash(total.Route.DefaultBudget.Priority),
		total.Route.DefaultBudget.PreferredCopies,
		total.Route.DefaultBudget.ReadTimeoutMillis,
		total.Route.DefaultBudget.IdleAfterFirstByteMs,
		total.Route.DefaultBudget.ReducedFEC,
		total.Route.DefaultBudget.FastACK,
		total.Route.DefaultBudget.IndependentIO,
		emptyDash(total.Route.Scheduler.Mode),
		total.Route.Scheduler.PerStreamAccounting,
		total.Route.Scheduler.DeficitRoundRobin,
		total.Route.Scheduler.StarvationProtection,
		total.Route.Scheduler.SessionCCCoupled,
		total.Route.Keepalive.InFlightBytes,
		total.Route.Keepalive.SendCreditBytes,
		total.Route.Keepalive.AckCreditFrames,
		emptyDash(total.Burst.Label),
		total.Burst.Active,
		total.Burst.RateMbps,
		formatTime(total.Burst.EndsAt),
	))

	s.autoText.SetText(fmt.Sprintf(
		"Auto Mixed Enabled: %t\r\nCurrent Random Event: %s\r\nNext Random Event: %s\r\nNext Event At: %s\r\nCountdown: %s\r\nLast Triggered At: %s",
		total.AutoScenario.Enabled,
		emptyDash(total.AutoScenario.CurrentEvent),
		emptyDash(total.AutoScenario.NextEvent),
		formatTime(total.AutoScenario.NextEventAt),
		formatCountdown(total.Now, total.AutoScenario.NextEventAt),
		formatTime(total.AutoScenario.LastTriggeredAt),
	))

	s.mediaText.SetText(fmt.Sprintf(
		"Target: %s\r\nConfigured Base Rate: %.2f Mbps\r\nPayload: %d B\r\n\r\nSent Packets: %d\r\nSent Bytes: %d\r\nCurrent Tx: %.2f Mbps\r\nSend Errors: %d",
		emptyDash(total.MediaTarget),
		total.MediaRateMbps,
		total.MediaPayload,
		total.MediaPackets,
		total.MediaBytes,
		snap.MediaBps/1_000_000.0,
		total.MediaErrors,
	))

	s.controlText.SetText(fmt.Sprintf(
		"UDP Target: %s\r\nUDP ok/fail: %d / %d\r\nUDP avg/min/max: %s / %s / %s\r\nUDP last: %s\r\nUDP last_err: %s\r\nUDP last_ok: %s\r\nUDP last_fail: %s\r\n\r\nTCP Target: %s\r\nTCP ok/fail: %d / %d\r\nTCP avg/min/max: %s / %s / %s\r\nTCP last: %s\r\nTCP last_err: %s\r\nTCP last_ok: %s\r\nTCP last_fail: %s",
		emptyDash(total.ControlUDPTarget),
		total.ControlUDP.OK, total.ControlUDP.Fail,
		formatDuration(total.ControlUDP.AvgRTT), formatDuration(total.ControlUDP.MinRTT), formatDuration(total.ControlUDP.MaxRTT),
		formatDuration(total.ControlUDP.LastRTT),
		emptyDash(total.ControlUDP.LastErr),
		formatTime(total.ControlUDP.LastOKAt),
		formatTime(total.ControlUDP.LastFailAt),
		emptyDash(total.ControlTCPTarget),
		total.ControlTCP.OK, total.ControlTCP.Fail,
		formatDuration(total.ControlTCP.AvgRTT), formatDuration(total.ControlTCP.MinRTT), formatDuration(total.ControlTCP.MaxRTT),
		formatDuration(total.ControlTCP.LastRTT),
		emptyDash(total.ControlTCP.LastErr),
		formatTime(total.ControlTCP.LastOKAt),
		formatTime(total.ControlTCP.LastFailAt),
	))

	s.receiveText.SetText(fmt.Sprintf(
		"Scenario: %s\r\nUDP Rx Packets: %d\r\nUDP Rx Bytes: %d\r\nCurrent Rx: %.2f Mbps\r\nUnknown Replies: %d\r\n\r\nRuntime Accounting:\r\n- Flow Class: %s\r\n- ACK Debt: %d\r\n- Send Credit: %d B\r\n- Inflight: %d B\r\n\r\nNotes:\r\n- control fail rising usually means control probe echo did not return in time\r\n- media tx well below configured rate means throughput or scheduling needs more inspection\r\n- when switches increase, watch control fail and RTT spike together",
		total.Scenario,
		total.UDPRecvPackets,
		total.UDPRecvBytes,
		snap.UDPRecvBps/1_000_000.0,
		total.UnknownRecv,
		emptyDash(total.Route.DefaultBudget.FlowClass),
		total.Route.Keepalive.AckCreditFrames,
		total.Route.Keepalive.SendCreditBytes,
		total.Route.Keepalive.InFlightBytes,
	))

	s.chartText.SetText(s.renderCharts())
	s.summaryText.SetText(s.buildSummary())
}

func (s *uiState) collectOptions() (sim.Options, error) {
	mediaRate, err := strconv.ParseFloat(strings.TrimSpace(s.mediaRateEdit.Text()), 64)
	if err != nil {
		return sim.Options{}, fmt.Errorf("invalid media rate: %w", err)
	}
	mediaPayload, err := strconv.Atoi(strings.TrimSpace(s.mediaPayloadEdit.Text()))
	if err != nil {
		return sim.Options{}, fmt.Errorf("invalid media payload bytes: %w", err)
	}
	controlUDPInterval, err := time.ParseDuration(strings.TrimSpace(s.controlUDPIntervalEdit.Text()))
	if err != nil {
		return sim.Options{}, fmt.Errorf("invalid UDP interval: %w", err)
	}
	controlUDPTimeout, err := time.ParseDuration(strings.TrimSpace(s.controlUDPTimeoutEdit.Text()))
	if err != nil {
		return sim.Options{}, fmt.Errorf("invalid UDP timeout: %w", err)
	}
	controlTCPInterval, err := time.ParseDuration(strings.TrimSpace(s.controlTCPIntervalEdit.Text()))
	if err != nil {
		return sim.Options{}, fmt.Errorf("invalid TCP interval: %w", err)
	}
	controlTCPTimeout, err := time.ParseDuration(strings.TrimSpace(s.controlTCPTimeoutEdit.Text()))
	if err != nil {
		return sim.Options{}, fmt.Errorf("invalid TCP timeout: %w", err)
	}
	controlTCPRead, err := strconv.Atoi(strings.TrimSpace(s.controlTCPReadEdit.Text()))
	if err != nil {
		return sim.Options{}, fmt.Errorf("invalid TCP read bytes: %w", err)
	}
	return sim.Options{
		ConfigPath:         strings.TrimSpace(s.configEdit.Text()),
		ConnectTimeout:     12 * time.Second,
		Scenario:           s.scenarioCombo.Text(),
		MediaTarget:        strings.TrimSpace(s.mediaTargetEdit.Text()),
		MediaRateMbps:      mediaRate,
		MediaPayloadBytes:  mediaPayload,
		ControlUDPTarget:   strings.TrimSpace(s.controlUDPTargetEdit.Text()),
		ControlUDPInterval: controlUDPInterval,
		ControlUDPTimeout:  controlUDPTimeout,
		ControlTCPEnabled:  s.enableTCPCheck.Checked(),
		ControlTCPTarget:   strings.TrimSpace(s.controlTCPTargetEdit.Text()),
		ControlTCPInterval: controlTCPInterval,
		ControlTCPTimeout:  controlTCPTimeout,
		ControlTCPPayload:  s.controlTCPPayloadEdit.Text(),
		ControlTCPRead:     controlTCPRead,
	}, nil
}

func (s *uiState) triggerBurst(kind, eventText string) {
	if s.engine == nil {
		return
	}
	s.engine.TriggerControlBurst(kind)
	s.appendEventEntry(eventText, len(s.history)-1)
}

func (s *uiState) triggerMediaPeakBurst() {
	if s.engine == nil {
		return
	}
	rate, err := strconv.ParseFloat(strings.TrimSpace(s.burstRateEdit.Text()), 64)
	if err != nil {
		walk.MsgBox(s.mw, "Invalid Burst Rate", err.Error(), walk.MsgBoxIconError)
		return
	}
	dur, err := time.ParseDuration(strings.TrimSpace(s.burstDurationEdit.Text()))
	if err != nil {
		walk.MsgBox(s.mw, "Invalid Burst Duration", err.Error(), walk.MsgBoxIconError)
		return
	}
	s.engine.TriggerMediaBurst("manual media peak", rate, dur)
	s.appendEventEntry(fmt.Sprintf("manual action: media peak burst %.2f Mbps for %s", rate, dur), len(s.history)-1)
}

func (s *uiState) appendHistory(snap sim.DeltaSnapshot) {
	s.history = append(s.history, snap)
	if len(s.history) > 600 {
		s.history = s.history[len(s.history)-600:]
	}
	current := snap.Total
	s.lastSnapshot = &current
}

func (s *uiState) appendEventEntry(msg string, sampleIndex int) {
	line := fmt.Sprintf("%s  %s", time.Now().Format("15:04:05"), msg)
	s.eventEntries = append(s.eventEntries, timelineEntry{Label: line, SampleIndex: max(sampleIndex, 0)})
	if len(s.eventEntries) > 300 {
		s.eventEntries = s.eventEntries[len(s.eventEntries)-300:]
	}
	s.refreshEventModels()
}

func (s *uiState) appendSummaryEntry(msg string, sampleIndex int) {
	s.summaryEntries = append(s.summaryEntries, timelineEntry{Label: msg, SampleIndex: max(sampleIndex, 0)})
	if len(s.summaryEntries) > 80 {
		s.summaryEntries = s.summaryEntries[len(s.summaryEntries)-80:]
	}
	s.refreshEventModels()
}

func (s *uiState) refreshEventModels() {
	eventLabels := make([]string, 0, len(s.eventEntries))
	for _, e := range s.eventEntries {
		eventLabels = append(eventLabels, e.Label)
	}
	summaryLabels := make([]string, 0, len(s.summaryEntries))
	for _, e := range s.summaryEntries {
		summaryLabels = append(summaryLabels, e.Label)
	}
	s.eventListBox.SetModel(eventLabels)
	s.summaryListBox.SetModel(summaryLabels)
}

func (s *uiState) captureEvents(snap sim.DeltaSnapshot) {
	cur := snap.Total
	prev := s.lastSnapshot
	if prev == nil {
		return
	}
	sampleIndex := len(s.history)
	if cur.Route.SwitchCount > prev.Route.SwitchCount {
		s.appendEventEntry(fmt.Sprintf("route switch: %s -> %s (line=%s)", emptyDash(prev.Route.RouteName), emptyDash(cur.Route.RouteName), emptyDash(cur.Route.LineID)), sampleIndex)
	}
	if cur.ControlUDP.Fail > prev.ControlUDP.Fail {
		s.appendEventEntry(fmt.Sprintf("control UDP fail increased: %d -> %d, last_err=%s", prev.ControlUDP.Fail, cur.ControlUDP.Fail, emptyDash(cur.ControlUDP.LastErr)), sampleIndex)
	}
	if cur.ControlTCP.Fail > prev.ControlTCP.Fail {
		s.appendEventEntry(fmt.Sprintf("control TCP fail increased: %d -> %d, last_err=%s", prev.ControlTCP.Fail, cur.ControlTCP.Fail, emptyDash(cur.ControlTCP.LastErr)), sampleIndex)
	}
	if cur.MediaErrors > prev.MediaErrors {
		s.appendEventEntry(fmt.Sprintf("media send errors increased: %d -> %d", prev.MediaErrors, cur.MediaErrors), sampleIndex)
	}
	if cur.UnknownRecv > prev.UnknownRecv {
		s.appendEventEntry(fmt.Sprintf("unknown replies increased: %d -> %d", prev.UnknownRecv, cur.UnknownRecv), sampleIndex)
	}
	if cur.Burst.Active && (!prev.Burst.Active || cur.Burst.Label != prev.Burst.Label) {
		s.appendEventEntry(fmt.Sprintf("burst active: %s %.2f Mbps until %s", emptyDash(cur.Burst.Label), cur.Burst.RateMbps, formatTime(cur.Burst.EndsAt)), sampleIndex)
	}
	if snap.MediaBps > 0 && cur.MediaRateMbps > 0 {
		actualMbps := snap.MediaBps / 1_000_000.0
		if actualMbps < cur.MediaRateMbps*0.6 {
			s.appendEventEntry(fmt.Sprintf("media throughput low: %.2f Mbps / %.2f Mbps", actualMbps, cur.MediaRateMbps), sampleIndex)
		}
	}
	s.rebuildSummaryEntries()
}

func (s *uiState) rebuildSummaryEntries() {
	s.summaryEntries = nil
	if len(s.history) < 2 {
		s.refreshEventModels()
		return
	}
	base := s.history[0].Total
	worstIdx := 0
	worstScore := -1.0
	for idx, h := range s.history {
		score := 0.0
		actualMbps := h.MediaBps / 1_000_000.0
		if h.Total.MediaRateMbps > 0 && actualMbps < h.Total.MediaRateMbps*0.6 {
			score += 2
		}
		score += float64(h.Total.ControlUDP.Fail-base.ControlUDP.Fail) * 0.2
		score += float64(h.Total.ControlTCP.Fail-base.ControlTCP.Fail) * 0.2
		score += durationMs(h.Total.ControlUDP.LastRTT) / 300.0
		score += durationMs(h.Total.ControlTCP.LastRTT) / 300.0
		if score > worstScore {
			worstScore = score
			worstIdx = idx
		}
	}
	last := s.history[len(s.history)-1].Total
	s.appendSummaryEntry(fmt.Sprintf("%s  unstable-window  around %s", time.Now().Format("15:04:05"), s.history[worstIdx].Total.Now.Format("15:04:05")), worstIdx)
	if last.ControlUDP.Fail > base.ControlUDP.Fail {
		s.appendSummaryEntry(fmt.Sprintf("%s  udp-control-fail-rise  +%d", time.Now().Format("15:04:05"), last.ControlUDP.Fail-base.ControlUDP.Fail), len(s.history)-1)
	}
	if last.ControlTCP.Fail > base.ControlTCP.Fail {
		s.appendSummaryEntry(fmt.Sprintf("%s  tcp-control-fail-rise  +%d", time.Now().Format("15:04:05"), last.ControlTCP.Fail-base.ControlTCP.Fail), len(s.history)-1)
	}
	mediaCollapseIdx := s.findLatestMediaCollapse()
	if mediaCollapseIdx >= 0 {
		s.appendSummaryEntry(fmt.Sprintf("%s  media-collapse  around %s", time.Now().Format("15:04:05"), s.history[mediaCollapseIdx].Total.Now.Format("15:04:05")), mediaCollapseIdx)
	}
}

func (s *uiState) findLatestMediaCollapse() int {
	for i := len(s.history) - 1; i >= 0; i-- {
		h := s.history[i]
		actualMbps := h.MediaBps / 1_000_000.0
		if h.Total.MediaRateMbps > 0 && actualMbps < h.Total.MediaRateMbps*0.6 {
			return i
		}
	}
	return -1
}

func (s *uiState) renderCharts() string {
	if len(s.history) == 0 {
		return "No data"
	}
	recent := s.history
	if len(recent) > 60 {
		recent = recent[len(recent)-60:]
	}
	var media []float64
	var udpRTT []float64
	var tcpRTT []float64
	var rx []float64
	for _, h := range recent {
		media = append(media, h.MediaBps/1_000_000.0)
		rx = append(rx, h.UDPRecvBps/1_000_000.0)
		udpRTT = append(udpRTT, durationMs(h.Total.ControlUDP.LastRTT))
		tcpRTT = append(tcpRTT, durationMs(h.Total.ControlTCP.LastRTT))
	}
	return strings.Join([]string{
		"Media Tx Mbps: " + sparkline(media),
		"UDP Rx Mbps:   " + sparkline(rx),
		"UDP RTT ms:    " + sparkline(udpRTT),
		"TCP RTT ms:    " + sparkline(tcpRTT),
		"",
		"Tips:",
		"- Higher sparkline means relatively higher value in recent samples",
		"- Media / UDP Rx trends help identify throughput collapse",
		"- UDP / TCP RTT trends help identify control flow spikes",
	}, "\r\n")
}

func (s *uiState) buildSummary() string {
	if len(s.history) < 2 {
		return "Need more samples to build summary."
	}
	first := s.history[0].Total
	last := s.history[len(s.history)-1].Total
	udpFailRise := last.ControlUDP.Fail - first.ControlUDP.Fail
	tcpFailRise := last.ControlTCP.Fail - first.ControlTCP.Fail

	mediaCollapseCount := 0
	longestCollapse := 0
	currentCollapse := 0
	for _, h := range s.history {
		actualMbps := h.MediaBps / 1_000_000.0
		if h.Total.MediaRateMbps > 0 && actualMbps < h.Total.MediaRateMbps*0.6 {
			mediaCollapseCount++
			currentCollapse++
			if currentCollapse > longestCollapse {
				longestCollapse = currentCollapse
			}
		} else {
			currentCollapse = 0
		}
	}

	controlStatus := "stable"
	if udpFailRise > 0 || tcpFailRise > 0 {
		controlStatus = "failures increased"
	}
	mediaStatus := "stable"
	if mediaCollapseCount > 0 {
		mediaStatus = fmt.Sprintf("collapse detected (%d samples, longest %d s)", mediaCollapseCount, longestCollapse)
	}

	return fmt.Sprintf(
		"Control Summary\r\n- UDP control fail increase: %d\r\n- TCP control fail increase: %d\r\n- Control result: %s\r\n\r\nMedia Summary\r\n- Media collapse: %s\r\n- Final media bytes: %d\r\n- Final media packets: %d\r\n\r\nRoute Summary\r\n- Switch count: %d\r\n- Last route: %s / %s\r\n- Last reason: %s\r\n- Flow class / priority: %s / %s\r\n- Scheduler: %s\r\n- Inflight / send_credit / ack_debt: %d / %d / %d",
		udpFailRise,
		tcpFailRise,
		controlStatus,
		mediaStatus,
		last.MediaBytes,
		last.MediaPackets,
		last.Route.SwitchCount,
		emptyDash(last.Route.RouteName),
		emptyDash(last.Route.LineID),
		emptyDash(last.Route.LastRouteReason),
		emptyDash(last.Route.DefaultBudget.FlowClass),
		emptyDash(last.Route.DefaultBudget.Priority),
		emptyDash(last.Route.Scheduler.Mode),
		last.Route.Keepalive.InFlightBytes,
		last.Route.Keepalive.SendCreditBytes,
		last.Route.Keepalive.AckCreditFrames,
	)
}

func (s *uiState) showSelectedTimelineDetail(fromEvent bool) {
	var entry *timelineEntry
	if fromEvent {
		idx := s.eventListBox.CurrentIndex()
		if idx < 0 || idx >= len(s.eventEntries) {
			return
		}
		entry = &s.eventEntries[idx]
	} else {
		idx := s.summaryListBox.CurrentIndex()
		if idx < 0 || idx >= len(s.summaryEntries) {
			return
		}
		entry = &s.summaryEntries[idx]
	}
	if entry == nil || len(s.history) == 0 {
		return
	}
	s.detailText.SetText(s.buildWindowDetail(entry.SampleIndex))
}

func (s *uiState) buildWindowDetail(center int) string {
	if len(s.history) == 0 {
		return "No samples."
	}
	if center < 0 {
		center = 0
	}
	if center >= len(s.history) {
		center = len(s.history) - 1
	}
	start := center - 3
	end := center + 3
	if start < 0 {
		start = 0
	}
	if end >= len(s.history) {
		end = len(s.history) - 1
	}
	var lines []string
	lines = append(lines, fmt.Sprintf("Sample window around index=%d (%s)", center, s.history[center].Total.Now.Format("15:04:05")))
	lines = append(lines, "")
	for i := start; i <= end; i++ {
		h := s.history[i]
		lines = append(lines, fmt.Sprintf(
			"[%03d] %s  media_tx=%.2f Mbps  udp_rx=%.2f Mbps  udp_fail=%d  tcp_fail=%d  udp_last=%s  tcp_last=%s  burst=%s  flow=%s  ack_debt=%d  inflight=%d  credit=%d",
			i,
			h.Total.Now.Format("15:04:05"),
			h.MediaBps/1_000_000.0,
			h.UDPRecvBps/1_000_000.0,
			h.Total.ControlUDP.Fail,
			h.Total.ControlTCP.Fail,
			formatDuration(h.Total.ControlUDP.LastRTT),
			formatDuration(h.Total.ControlTCP.LastRTT),
			emptyDash(h.Total.Burst.Label),
			emptyDash(h.Total.Route.DefaultBudget.FlowClass),
			h.Total.Route.Keepalive.AckCreditFrames,
			h.Total.Route.Keepalive.InFlightBytes,
			h.Total.Route.Keepalive.SendCreditBytes,
		))
	}
	return strings.Join(lines, "\r\n")
}

func durationMs(d time.Duration) float64 {
	if d <= 0 {
		return 0
	}
	return float64(d) / float64(time.Millisecond)
}

func sparkline(values []float64) string {
	const blocks = "▁▂▃▄▅▆▇█"
	if len(values) == 0 {
		return "-"
	}
	minV, maxV := values[0], values[0]
	for _, v := range values {
		if v < minV {
			minV = v
		}
		if v > maxV {
			maxV = v
		}
	}
	if maxV == minV {
		return strings.Repeat("▅", len(values))
	}
	var b strings.Builder
	runes := []rune(blocks)
	for _, v := range values {
		idx := int((v - minV) / (maxV - minV) * 7)
		if idx < 0 {
			idx = 0
		}
		if idx > 7 {
			idx = 7
		}
		b.WriteRune(runes[idx])
	}
	return b.String()
}

func (s *uiState) saveRecord() error {
	if len(s.history) == 0 {
		return fmt.Errorf("no record to save")
	}
	dlg := new(walk.FileDialog)
	dlg.Filter = "JSON Files (*.json)|*.json"
	dlg.Title = "Save Simulation Record"
	dlg.FilePath = filepath.Join(defaultSaveDir(), "xgw-sim-record-"+time.Now().Format("20060102-150405")+".json")
	ok, err := dlg.ShowSave(s.mw)
	if err != nil {
		return err
	}
	if !ok {
		return nil
	}
	record := map[string]any{
		"saved_at": time.Now().Format(time.RFC3339),
		"summary":  s.buildSummary(),
		"options": map[string]any{
			"config_path":          s.configEdit.Text(),
			"scenario":             s.scenarioCombo.Text(),
			"media_target":         s.mediaTargetEdit.Text(),
			"media_rate_mbps":      s.mediaRateEdit.Text(),
			"media_payload_bytes":  s.mediaPayloadEdit.Text(),
			"burst_rate_mbps":      s.burstRateEdit.Text(),
			"burst_duration":       s.burstDurationEdit.Text(),
			"control_udp_target":   s.controlUDPTargetEdit.Text(),
			"control_udp_interval": s.controlUDPIntervalEdit.Text(),
			"control_udp_timeout":  s.controlUDPTimeoutEdit.Text(),
			"control_tcp_enabled":  s.enableTCPCheck.Checked(),
			"control_tcp_target":   s.controlTCPTargetEdit.Text(),
			"control_tcp_interval": s.controlTCPIntervalEdit.Text(),
			"control_tcp_timeout":  s.controlTCPTimeoutEdit.Text(),
			"control_tcp_payload":  s.controlTCPPayloadEdit.Text(),
			"control_tcp_read":     s.controlTCPReadEdit.Text(),
		},
		"history":         s.history,
		"event_timeline":  s.eventEntries,
		"summary_windows": s.summaryEntries,
	}
	if s.lastSnapshot != nil {
		record["latest_route_state"] = map[string]any{
			"session_id":        emptyDash(s.lastSnapshot.Route.SessionID),
			"route_name":        emptyDash(s.lastSnapshot.Route.RouteName),
			"line_id":           emptyDash(s.lastSnapshot.Route.LineID),
			"address":           emptyDash(s.lastSnapshot.Route.Address),
			"congestion":        emptyDash(s.lastSnapshot.Route.Congestion),
			"last_route_reason": emptyDash(s.lastSnapshot.Route.LastRouteReason),
			"default_budget":    s.lastSnapshot.Route.DefaultBudget,
			"scheduler":         s.lastSnapshot.Route.Scheduler,
			"keepalive":         s.lastSnapshot.Route.Keepalive,
		}
	}
	data, err := json.MarshalIndent(record, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(dlg.FilePath, data, 0644)
}

func defaultConfigPath() string {
	cwd, _ := os.Getwd()
	return filepath.Join(cwd, "client.json")
}

func defaultSaveDir() string {
	cwd, _ := os.Getwd()
	return cwd
}

func emptyDash(v string) string {
	if strings.TrimSpace(v) == "" {
		return "-"
	}
	return v
}

func formatTime(t time.Time) string {
	if t.IsZero() {
		return "-"
	}
	return t.Format("15:04:05")
}

func formatDuration(d time.Duration) string {
	if d <= 0 {
		return "-"
	}
	return d.Round(time.Millisecond).String()
}

func formatCountdown(now, when time.Time) string {
	if when.IsZero() {
		return "-"
	}
	if !when.After(now) {
		return "due"
	}
	return when.Sub(now).Round(time.Second).String()
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}
