package worker

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func TestRuntimePortScanErrorUnavailableOnly(t *testing.T) {
	if !isIgnorableRuntimeScanError(errors.New("设备 KZ-80 运行时端口扫描失败; 设备 sp-02 运行时端口扫描失败")) {
		t.Fatal("unreachable device scan failures should be ignorable during cleanup")
	}
}

func TestRuntimePortScanErrorWithCauseRemainsIgnorable(t *testing.T) {
	err := errors.New(runtimeScanFailure("sp-02", "读取配置", errors.New("配置目录不存在")))
	if !isIgnorableRuntimeScanError(err) {
		t.Fatalf("detailed unavailable scan failure should remain ignorable: %v", err)
	}
	if !strings.Contains(err.Error(), "读取配置") || !strings.Contains(err.Error(), "配置目录不存在") {
		t.Fatalf("scan failure lost diagnostic cause: %v", err)
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

func TestMissingRuntimeShardConfigDirectoryIsRecognized(t *testing.T) {
	if !isMissingRuntimeShardConfigExitStatus(3) {
		t.Fatal("missing shard config directory must be recognized as an empty device")
	}
	if isMissingRuntimeShardConfigExitStatus(1) || isMissingRuntimeShardConfigExitStatus(2) {
		t.Fatal("unrelated remote command failures must not be treated as an empty device")
	}
}
