package webapp

import (
	"bytes"
	"context"
	"encoding/xml"
	"fmt"
	"net"
	"net/http"
	"os/exec"
	"strconv"
	"time"
)

type nmapResult struct {
	Hosts []struct {
		Status struct {
			State string `xml:"state,attr"`
		} `xml:"status"`
		Ports []struct {
			Port  int `xml:"portid,attr"`
			State struct {
				State  string `xml:"state,attr"`
				Reason string `xml:"reason,attr"`
			} `xml:"state"`
		} `xml:"ports>port"`
	} `xml:"host"`
}

func boundedNmap(ctx context.Context, host string, port int) (map[string]any, error) {
	binary, err := exec.LookPath("nmap")
	if err != nil {
		return nil, err
	}
	command := exec.CommandContext(ctx, binary, "-Pn", "-n", "-p", strconv.Itoa(port), "--host-timeout", "8s", "--reason", "-oX", "-", host)
	var output bytes.Buffer
	command.Stdout = &output
	if err = command.Run(); err != nil {
		return nil, err
	}
	var parsed nmapResult
	if err = xml.Unmarshal(output.Bytes(), &parsed); err != nil {
		return nil, err
	}
	result := map[string]any{"method": "nmap", "reachable": false, "port": port, "state": "unknown"}
	if len(parsed.Hosts) > 0 {
		result["reachable"] = parsed.Hosts[0].Status.State == "up"
		if len(parsed.Hosts[0].Ports) > 0 {
			result["state"] = parsed.Hosts[0].Ports[0].State.State
			result["reason"] = parsed.Hosts[0].Ports[0].State.Reason
		}
	}
	return result, nil
}

func tcpProbe(ctx context.Context, host string, port int) (map[string]any, error) {
	started := time.Now()
	dialer := net.Dialer{Timeout: 6 * time.Second}
	connection, err := dialer.DialContext(ctx, "tcp", net.JoinHostPort(host, strconv.Itoa(port)))
	result := map[string]any{"method": "tcp", "reachable": err == nil, "port": port, "latency_ms": float64(time.Since(started).Microseconds()) / 1000}
	if err != nil {
		result["error"] = "connection failed"
		return result, nil
	}
	connection.Close()
	result["state"] = "open"
	return result, nil
}

func (a *App) probeDevice(w http.ResponseWriter, r *http.Request) {
	device, err := a.store.Device(r.Context(), r.PathValue("id"))
	if err != nil {
		problem(w, 404, "device not found")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()
	started := time.Now()
	result, scanErr := boundedNmap(ctx, device.Host, device.SSHPort)
	if scanErr != nil {
		result, scanErr = tcpProbe(ctx, device.Host, device.SSHPort)
	}
	if scanErr != nil {
		problem(w, 502, "device probe failed")
		return
	}
	result["device_id"] = device.ID
	result["elapsed_ms"] = float64(time.Since(started).Microseconds()) / 1000
	health := "unreachable"
	if reachable, _ := result["reachable"].(bool); reachable && fmt.Sprint(result["state"]) == "open" {
		health = "healthy"
	}
	_ = a.store.UpdateDeviceHealth(r.Context(), device.ID, health)
	result["health"] = health
	writeJSON(w, 200, result)
}
