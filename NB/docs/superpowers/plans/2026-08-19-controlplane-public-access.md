# Control Plane Public Access Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove Nginx source-IP restrictions from the HTTPS control plane while retaining token authorization and the loopback-only application listener.

**Architecture:** Nginx remains the only public listener and terminates TLS on port 9091. The Go service remains private on `127.0.0.1:19091` and continues to enforce bearer-token authorization.

**Tech Stack:** Nginx, Go `nb-web`, Python `unittest`, systemd.

---

### Task 1: Protect the Nginx access boundary with a regression test

**Files:**
- Create: `controlplane/linux/test_nb_web_nginx.py`
- Test: `controlplane/linux/test_nb_web_nginx.py`

- [x] Add tests that reject source-IP `allow`/`deny` directives and require TLS plus loopback proxying.
- [x] Run `python controlplane/linux/test_nb_web_nginx.py` and confirm the public-access test fails on the current template.

### Task 2: Remove repository source-IP restrictions

**Files:**
- Modify: `controlplane/linux/nb-web.nginx.conf.example`

- [x] Delete the workstation-IP comment and all `allow`/`deny` directives.
- [x] Run `python controlplane/linux/test_nb_web_nginx.py` and confirm all tests pass.

### Task 3: Deploy and verify the HK Nginx configuration

**Files:**
- Modify remotely: `/etc/nginx/sites-available/nb-web`
- Verify remotely: `/etc/nginx/sites-enabled/nb-web`

- [x] Back up the active site and stage the template rendered with the HK public address.
- [x] Run `nginx -t`, atomically activate the staged file, and reload Nginx with automatic rollback on failure.
- [x] Verify public HTTPS reachability, token rejection without credentials, successful administrator API access, and the loopback-only `nb-web` listener.

### Task 4: Record the repository change

**Files:**
- Commit the design, plan, regression test, and Nginx template.

- [ ] Review `git diff --check` and the scoped diff.
- [ ] Commit only the files owned by this change.
