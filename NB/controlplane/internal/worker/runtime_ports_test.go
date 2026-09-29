package worker

import (
	"errors"
	"testing"
	"time"
)

func TestRuntimePortScanErrorUnavailableOnly(t *testing.T) {
	if !isIgnorableRuntimeScanError(errors.New("设备 KZ-80 运行时端口扫描失败; 设备 sp-02 运行时端口扫描失败")) {
		t.Fatal("unreachable device scan failures should be ignorable during cleanup")
	}
}

func TestRuntimePortScanErrorCredentialOrParseFailureBlocksCleanup(t *testing.T) {
	for _, message := range []string{
		"设备 sp-02 凭据不可用",
		"设备 sp-02 运行时端口解析失败",
	} {
		if isIgnorableRuntimeScanError(errors.New(message)) {
			t.Fatalf("%q must block cleanup", message)
		}
	}
}

func TestRuntimePortScanCooldownSuppressesPeriodicPlan(t *testing.T) {
	client := &Client{}
	plan := dynamicPlan{Nodes: []dynamicNode{{DeviceID: "sp-02"}, {DeviceID: "other"}}}
	now := time.Unix(100, 0)
	client.noteRuntimeScanFailure([]dynamicPlan{plan}, now, errors.New("设备 sp-02 运行时端口扫描失败"))
	if !client.runtimePlanCooling(plan, now.Add(time.Minute)) {
		t.Fatal("failed device plan should be cooled down")
	}
	if client.runtimePlanCooling(plan, now.Add(runtimePortScanCooldown+time.Second)) {
		t.Fatal("runtime scan cooldown should expire")
	}
}

func TestRuntimePortScanCooldownDoesNotSuppressConfigurationErrors(t *testing.T) {
	client := &Client{}
	plan := dynamicPlan{Nodes: []dynamicNode{{DeviceID: "sp-02"}}}
	now := time.Unix(100, 0)
	client.noteRuntimeScanFailure([]dynamicPlan{plan}, now, errors.New("设备 sp-02 凭据不可用"))
	if client.runtimePlanCooling(plan, now.Add(time.Minute)) {
		t.Fatal("credential errors must remain visible for immediate correction")
	}
}
