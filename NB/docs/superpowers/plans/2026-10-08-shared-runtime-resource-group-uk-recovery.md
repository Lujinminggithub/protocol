# Shared Runtime Isolation, Resource Group Compatibility, and UK Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make line CRUD instance-only on shared nodes, retain `line.upgrade` as the explicit shared-runtime maintenance action, remove dynamic resource-group coupling, and recover the two UK draft lines without disrupting active US lines.

**Architecture:** Dynamic line eligibility and locking use actual `device_id + role`; `resource_group` remains compatibility metadata. `line.open` invokes a new configuration-only deployment path that validates an existing shared shard runtime, atomically installs only one `instance_id`, and records an independent configuration deployment marker. Existing `line.upgrade` remains the only path allowed to build, distribute, activate, and roll back shared Node binaries.

**Tech Stack:** Go 1.26 control plane and worker, Python 3 deployment tools, C shard runtime, MySQL/MariaDB, embedded HTML/JavaScript, Paramiko/OpenSSH, systemd.

**Spec:** `docs/superpowers/specs/2026-10-08-shared-runtime-resource-group-uk-recovery-design.md`

## Global Constraints

- Preserve the NB wire format and all online client configurations.
- `line.open`, `line.disable`, deletion, and failed-open rollback must never switch `/etc/NB/shards/nb_node`, replace a shared shard unit, or restart a shared shard service.
- `line.upgrade` is the explicit shared-runtime maintenance operation and must expose every line affected through shared device roles.
- Dynamic operations, snapshots, runtime scans, and locks must not depend on operator-entered `resource_group` values.
- Historical `resource_group` values, static registry behavior, old operation records, and old APIs remain readable.
- Production acceptance must prove the four active US lines remain healthy and the Guangzhou/Hong Kong shard PIDs and binary hashes do not change during UK line open.
- Synchronize completed tracked changes from `E:\code\Newbility` to `E:\project\protocol\NB`, commit Chinese messages in both repositories, and do not push.

---

### Task 1: Remove Dynamic Resource-Group Coupling

**Files:**
- Modify: `controlplane/internal/worker/dynamic.go`
- Modify: `controlplane/internal/worker/client.go`
- Modify: `controlplane/internal/worker/runtime_ports.go`
- Modify: `controlplane/internal/worker/config.go`
- Modify: `controlplane/internal/worker/worker_test.go`
- Modify: `controlplane/internal/webapp/inventory.go`
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/app_test.go`

**Interfaces:**
- Consumes: existing `dynamicPlan`, `central.LineSpec`, and worker registry JSON.
- Produces: server-owned default resource group `managed`; dynamic plans accepted independently of `DynamicConfig.ResourceGroups`.

- [x] **Step 1: Write worker tests that reproduce the rename failure**

Add tests that create a registry containing `resource_groups: ["gz-hk-primary"]` and a plan containing
`resource_group: "gz-hk-primary-product"`. Assert that `dynamicLine`, dynamic snapshot resolution, and runtime scan
plan selection accept the plan. The behavior change that makes these tests pass is removal of dynamic group filtering;
device credentials, host keys, ports, and operation kinds remain validated.

- [x] **Step 2: Run the focused worker tests and verify RED**

Run:

```powershell
go test ./controlplane/internal/worker -run 'ResourceGroup|DynamicSnapshot|RuntimePort' -count=1
```

Expected: FAIL because `validateDynamicPlan`, `snapshotLines`, or runtime scan selection rejects the renamed group.

- [x] **Step 3: Write Web tests for the server-owned default**

Add a request test that omits `resource_group`, saves a valid three-node spec, and asserts the stored value is
`managed`. Add an embedded-asset assertion that the open-line form no longer contains `name="resource_group"`.

- [x] **Step 4: Run focused Web tests and verify RED**

Run:

```powershell
go test ./controlplane/internal/webapp -run 'ResourceGroup|LineSpec' -count=1
```

Expected: FAIL because the API rejects an empty group and the form still exposes the field.

- [x] **Step 5: Implement compatibility semantics**

In `saveLineSpec`, normalize an empty group before validation:

```go
if strings.TrimSpace(spec.ResourceGroup) == "" {
    spec.ResourceGroup = "managed"
}
```

Remove `contains(plan.ResourceGroup, cfg.ResourceGroups)` from dynamic plan validation and dynamic-plan filters.
Allow `dynamic.resource_groups` to be empty while continuing to validate any supplied legacy values. Remove the
form input and omit `resource_group` from the JavaScript request. Do not change static `Registry.Lines` collision
semantics.

- [x] **Step 6: Run focused and package tests**

Run:

```powershell
go test ./controlplane/internal/worker ./controlplane/internal/webapp -count=1
```

Expected: PASS.

- [x] **Step 7: Commit**

```powershell
git add controlplane/internal/worker controlplane/internal/webapp
git commit -m "解除动态线路资源组耦合"
```

### Task 2: Add Configuration-Only Shared-Shard Deployment

**Files:**
- Modify: `tools/nb_shard_deploy.py`
- Modify: `tools/deploy_shard_runtime.py`
- Modify: `tools/deploy_core.py`
- Modify: `tools/deploy.py`
- Modify: `tools/test_shard_deploy.py`
- Create: `tools/test_deploy_instance_only.py`

**Interfaces:**
- Produces: `deploy_shard_runtime.install_instance(...) -> str`.
- Produces: `deploy.py deploy-instance --deployment-id <id> --socks-port <port>`.
- Produces: per-line marker `<INSTANCE_WORK>/current-deployment` read by `deploy.py current` before legacy fallback.
- Consumes: an already active shared shard service and `/etc/NB/shards/nb_node`; never mutates either.

- [x] **Step 1: Write a failing shared-runtime immutability test**

Use a fake remote command recorder and call the wished-for `install_instance`. Assert commands contain atomic
`<instance_id>.conf.next` installation, `systemctl reload`, and target control verification. Read-only shared binary
preflight is required, but command and upload records must contain none of:

```python
forbidden_commands = ("systemctl restart", "systemctl stop", "ln -sfn", ".nb_node.next")
forbidden_uploads = ("/etc/systemd/system/", "/shards/nb_node")
```

Also assert a missing or inactive shared service returns `runtime-preflight` before any config write.

- [x] **Step 2: Run the new test and verify RED**

Run:

```powershell
python tools/test_deploy_instance_only.py
```

Expected: FAIL because `install_instance` and `deploy-instance` do not exist.

- [x] **Step 3: Implement `install_instance`**

The function must:

```python
def install_instance(c, role, command, environment, deployment_id, *, work,
                     instance_work, deploy_instance, run, push_bytes,
                     effective_workers) -> str:
    """Install one line config into an existing shard without shared mutation."""
```

Validate the shared symlink, unit, and each worker service before writing. Create the per-line release directory,
render each worker config, upload to `.next`, validate permissions, atomically rename, reload, verify only the target
control sockets, save immutable per-line config copies, and write `current-deployment`. On failure restore only the
target config backups and reload; never restart a service.

- [x] **Step 4: Add `deploy-instance` orchestration**

Implement a transaction parallel to `act_deploy_socks` that connects roles in Exit, Relay, Entry activation order,
acquires existing device deployment locks, pushes only line security/rules/routes, calls `install_instance`, runs the
SOCKS smoke gate, and rolls back only target instance files. It must not call `_require_local_build`,
`_stage_entry_release`, `_copy_release_between_nodes`, `_activate_release`, `_backup_role_unit`, or
`_rollback_release`.

- [x] **Step 5: Make `current` marker-aware**

For named instances, `_remote_current_deployment` first reads and validates `<INSTANCE_WORK>/current-deployment`.
If absent, retain the existing symlink-based logic for historical deployments.

- [x] **Step 6: Run deployment tests**

Run:

```powershell
python tools/test_deploy_instance_only.py
python tools/test_shard_deploy.py
python tools/test_deploy_namespace.py
python tools/test_deploy_transaction.py
```

Expected: all print PASS or their existing success messages.

- [x] **Step 7: Commit**

```powershell
git add tools/nb_shard_deploy.py tools/deploy_shard_runtime.py tools/deploy_core.py tools/deploy.py tools/test_shard_deploy.py tools/test_deploy_instance_only.py
git commit -m "新增共享节点线路配置部署"
```

### Task 3: Route Line Open and Rollback Through Instance-Only Operations

**Files:**
- Modify: `tools/line_open.py`
- Modify: `tools/deploy.py`
- Modify: `tools/test_line_open.py`
- Modify: `tools/test_deploy_transaction.py`
- Modify: `controlplane/internal/worker/runner.go`
- Modify: `controlplane/internal/worker/worker_test.go`

**Interfaces:**
- Consumes: `deploy.py deploy-instance` from Task 2.
- Produces: deterministic configuration deployment IDs `cfg-<16 lowercase hex>` derived from normalized hosts and profile inputs.
- Preserves: `line.upgrade -> deploy.py build -> deploy.py deploy-socks` as the shared-binary maintenance path.

- [x] **Step 1: Write failing line-open tests**

Assert the executable line-open path never calls `prepare_release`, `deploy.py build`, `prepare-release`, or
`deploy-socks`; it calls `deploy-instance --deployment-id cfg-...` for bootstrap and stable configuration phases.
Assert `line.upgrade` still uses the existing build plus `deploy-socks` sequence.

- [x] **Step 2: Run focused tests and verify RED**

Run:

```powershell
python tools/test_line_open.py
go test ./controlplane/internal/worker -run 'Steps|Upgrade|Open' -count=1
```

Expected: FAIL because open still builds and deploys a binary.

- [x] **Step 3: Implement deterministic config deployments**

Add:

```python
def configuration_deployment_id(hosts: pathlib.Path, profile: pathlib.Path) -> str:
    digest = hashlib.sha256(hosts.read_bytes() + b"\0" + profile.read_bytes()).hexdigest()
    return "cfg-" + digest[:16]
```

Replace both open deployment calls with `deploy-instance`. Keep qualification and policy application unchanged.
Record configuration deployment IDs in the existing checkpoint fields so resume remains idempotent.

- [x] **Step 4: Make failed-open rollback instance-only**

Add an exact config rollback mode that restores saved `<instance_id>.conf` files and the per-line marker, reloads,
and validates target controls. It must reject a rollback requiring a different shared binary and must not restart
shared shards.

- [x] **Step 5: Run line-open and worker tests**

Run:

```powershell
python tools/test_line_open.py
python tools/test_deploy_transaction.py
go test ./controlplane/internal/worker -count=1
```

Expected: PASS.

- [x] **Step 6: Commit**

```powershell
git add tools/line_open.py tools/deploy.py tools/test_line_open.py tools/test_deploy_transaction.py controlplane/internal/worker
git commit -m "开线改为线路实例级部署"
```

### Task 4: Productize Shared-Runtime Upgrade Impact

**Files:**
- Modify: `controlplane/internal/central/inventory.go`
- Modify: `controlplane/internal/central/store_test.go`
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/app_test.go`
- Modify: `controlplane/internal/webapp/assets/app.js`

**Interfaces:**
- Produces: `Store.LinesSharingDeviceRoles(ctx, lineID string) ([]string, error)`.
- Produces: authoritative `request.affected_lines` for `line.upgrade`.
- Consumes: existing line nodes and operation request JSON.

- [x] **Step 1: Write failing central-store test**

Create three lines where two share Entry/Relay and one is disjoint. Assert querying either shared line returns both
line IDs exactly once in sorted order and excludes the disjoint line.

- [x] **Step 2: Run the store test and verify RED**

Run:

```powershell
go test ./controlplane/internal/central -run LinesSharingDeviceRoles -count=1
```

Expected: FAIL because the method does not exist.

- [x] **Step 3: Implement the impact query and operation enrichment**

Use a self-join on `line_nodes` matching both `device_id` and `role`. When creating `line.upgrade`, ignore any
client-supplied impact list and insert the server-generated sorted `affected_lines` into the request before persistence.

- [x] **Step 4: Add Web confirmation coverage**

Render the affected line IDs in the upgrade confirmation and operation detail. Test the embedded asset contains the
impact label and the API request/result retains the server-generated list.

- [x] **Step 5: Run control-plane tests**

Run:

```powershell
go test ./controlplane/internal/central ./controlplane/internal/webapp -count=1
```

Expected: PASS.

- [x] **Step 6: Commit**

```powershell
git add controlplane/internal/central controlplane/internal/webapp
git commit -m "展示共享运行时升级影响线路"
```

### Task 5: Full Verification, Deployment, and UK Recovery

**Files:**
- Modify: `docs/superpowers/plans/2026-10-08-shared-runtime-resource-group-uk-recovery.md` (checkboxes only)
- Runtime: `152.32.171.216:/opt/nb-controlplane`
- Runtime: UK Exit `86.53.60.2:/etc/ssh/sshd_config.d/60-nb-capacity.conf`

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: deployed control plane, deleted `gz-hk-uk-10001`, active `gz-hk-uk-10002`, unchanged US shared runtime.

- [x] **Step 1: Run repository verification**

Run the focused tests above, then:

```powershell
go test ./controlplane/... -count=1
go test -race ./controlplane/internal/worker -count=1
python -m compileall -q tools
ctest --test-dir build --output-on-failure
```

Expected: all commands exit 0.

- [x] **Step 2: Build Linux control-plane binaries**

Build `nb-web` and `nb-web-worker` for `linux/amd64`, record SHA256, and run their version/help smoke checks before
upload. Do not build or deploy a new Node binary as part of UK line open.

- [x] **Step 3: Capture the shared-runtime baseline**

Before deployment or UK retry, record active US line states, Guangzhou/Hong Kong shard PIDs, shared binary SHA256,
and target control socket health. Abort UK open if any active US line is already unhealthy.

- [x] **Step 4: Harden UK SSH capacity**

Back up any existing drop-in, write the approved three settings, run `sshd -t`, reload `ssh`, and read back `sshd -T`.
From the Hong Kong Relay run at least 20 bounded SSH key-exchange probes; require all to reach host-key exchange without
`MaxStartups`, reset, or banner failure.

- [x] **Step 5: Deploy Web and worker transactionally**

Back up current binaries and registry, upload verified binaries, restart services, and verify `systemctl is-active`,
health endpoints, worker heartbeat, version, and logs. Roll back both binaries on any gate failure.

- [x] **Step 6: Delete UK line 10001**

Create one new normal deletion request for `gz-hk-uk-10001`, wait for its `line.disable` operation to succeed, verify
all three target instance configs/control sockets are absent or were already absent, and verify the control-plane line
record is deleted.

- [x] **Step 7: Open UK line 10002**

Create one new `line.open` operation for `gz-hk-uk-10002`, wait through completion, then verify Exit bind IP
`86.53.60.118`, client config delivery, three role controls, runtime claims, and end-to-end SOCKS behavior.

- [x] **Step 8: Prove shared US lines were not disturbed**

Compare the before/after Guangzhou and Hong Kong shard PIDs and binary hashes byte-for-byte. Recheck all four active
US lines and require no new failed snapshots, control loss, or data-plane regression.

- [x] **Step 9: Commit production-tested changes**

```powershell
git add -u
git commit -m "完成共享节点隔离与英国线路恢复"
```

### Task 6: Synchronize the Canonical Repositories

**Files:**
- Source: `E:\code\Newbility`
- Destination: `E:\project\protocol\NB`

**Interfaces:**
- Consumes: production-tested tracked tree from Task 5.
- Produces: identical tracked content and separate local Chinese commits in both repositories.

- [x] **Step 1: Compare both worktrees without deleting user files**

Inspect `git status`, remotes, current branches, and tracked diffs. Preserve unrelated and untracked files in both trees.

- [x] **Step 2: Synchronize the approved tracked changes**

Copy only the files changed by this plan from Newbility to NB. Verify `git diff --check` and compare SHA256 for every
copied file.

- [x] **Step 3: Run focused smoke tests in the destination**

Run worker/Web Go tests and Python deployment tests from `E:\project\protocol\NB`.

- [x] **Step 4: Commit without pushing**

```powershell
git add <changed-files>
git commit -m "同步共享节点隔离与英国线路修复"
```

- [x] **Step 5: Final audit**

Confirm both repositories have no uncommitted tracked changes, no push occurred, control-plane services remain active,
`gz-hk-uk-10001` is absent, `gz-hk-uk-10002` is active, and the four US lines remain healthy.
