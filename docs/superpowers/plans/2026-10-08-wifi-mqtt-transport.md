# WiFi + MQTT Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Usage data reaches S3 Clawdmeters over WiFi + MQTT (auto-reconnecting via mDNS), provisioned once by typing a 6-digit on-screen code into `clawdmeter-setup`; BLE stays for the HID keys and setup.

**Architecture:**
- The host daemon (macOS Python or Linux bash) supervises a password-protected mosquitto on port 18830 and announces it as `_clawdmeter._tcp` over mDNS. It publishes the existing usage JSON, retained, to `clawdmeter/<id>/usage`.
- The firmware adds two layers:
  - `netcfg`: pure logic that can be host-tested.
  - `prov` + `net`: Arduino code covering NVS, WiFi, mDNS and esp-mqtt on its own task. Payloads go through the existing `parse_json` → `ui_update` path.
- Provisioning rides a new owner-gated BLE characteristic `...0005`.

**Tech Stack:**
- Firmware: Arduino-ESP32 3.x (pioarduino 55.03.38-1), ESP-IDF `esp-mqtt` (`mqtt_client.h`), `ESPmDNS`, `WiFi`, NimBLE-Arduino 2.x, ArduinoJson 7, LVGL 9.
- Host: mosquitto 2.x, Python 3.10+ with bleak, paho-mqtt ≥ 2.0 and zeroconf ≥ 0.132; bash with mosquitto-clients and avahi-utils.

**Spec:** `docs/superpowers/specs/2026-10-08-wifi-mqtt-transport-design.md`

## Global Constraints

- **Broker:** port **18830**, `allow_anonymous false`, no TLS, no certs. Nothing secret goes in the repo; every password is generated on the user's machine.
- **Broker state dir:** `~/.config/claude-usage-monitor/broker/`
  - Files: `broker.env` (`BROKER_ID`, `BROKER_PORT`, `DAEMON_PASS`), `passwd`, `acl`, `mosquitto.conf`, `devices`, `broker.pid`, `data/`.
  - Files are mode 0600 and the dir is 0700.
- **IDs and topics:**
  - Device id: 6 lowercase hex characters (last 3 bytes of the eFuse MAC). It is also the device's MQTT username.
  - Topics: `clawdmeter/<id>/usage` (retained, QoS 1), `clawdmeter/<id>/refresh`, `clawdmeter/<id>/status` (`online`, LWT `offline`, retained).
- **mDNS:** service `_clawdmeter._tcp`, TXT `id=<BROKER_ID>`. `BROKER_ID` is 16 lowercase hex characters.
- **BLE:** PROV char UUID `4c41555a-4465-7669-6365-000000000005` (WRITE | WRITE_NR | NOTIFY). It has the same owner + encryption gate as RX.
- **Setup code:** 6 digits, valid 120 s, at most 5 wrong tries.
- **Board scope:** `-DBOARD_HAS_WIFI` only on the five S3 envs: `waveshare_amoled_216`, `waveshare_amoled_18`, `waveshare_amoled_206`, `waveshare_lcd_154`, `waveshare_lcd_4`. C6 envs and `sim` must still build.
- **Shared code:** no `#ifdef BOARD_*`. Use the `BOARD_HAS_*` capability flags only (CLAUDE.md gotcha 10).
- **Windows daemon:** unchanged (BLE only).
- **Git:** stay on branch `feat/wifi-mqtt`, commit locally, never `git push`.

## Review Focus

1. **Laptop away then back (the core goal).** The broker vanishes and comes back, possibly on a new IP. The device must reconnect within about 20 s with no touch. Pinned in Task 5 (rediscover-after-20 s logic) and Task 11 (step 3–4 hardware check).
2. **Daemon not running during setup.** No broker means `net:up` can never arrive. The CLI must say "start the daemon" up front, not time out with no explanation. Pinned in Task 7 (`broker_reachable` test).
3. **Wrong WiFi password.** The device joins nothing. The CLI must time out with a WiFi hint, and the device must keep working over BLE. Pinned in Task 7 (timeout message) and Task 11 step 6.
4. **SSID or password with JSON-hostile or maximum-length characters** (quotes, colons, 32-char SSID, 63-char pass). Must round-trip exactly and never truncate silently. Pinned in Task 3 (`netcfg_parse` length tests) and Task 7 (`build_config_message` size guard).
5. **Board without WiFi (C6) running setup.** It must get a clear "no WiFi on this board" message, not a hang. Pinned in Task 4 (`nowifi` reply) and Task 7 (CLI branch).

---

## File map

| File | Status | Responsibility |
|---|---|---|
| `daemon/broker/find-tool.sh` | new | `clawd_find_tool NAME` — PATH + sbin fallback lookup (brew puts `mosquitto` in sbin) |
| `daemon/broker/broker-init.sh` | new | one-time broker state: ids, daemon password, ACL, conf |
| `daemon/tests/test_broker_init.sh` | new | init + live-broker auth/ACL test |
| `daemon/clawdmeter_mqtt.py` | new | Python transport: broker env, BrokerSupervisor, Announcer, Publisher |
| `daemon/tests/test_mqtt_transport.py` | new | unit + live-broker tests |
| `firmware/src/netcfg.{h,cpp}` | new | pure: NetConfig, JSON parse, setup-code session, topic builder |
| `firmware/test/test_netcfg/test_main.cpp` | new | host test |
| `firmware/src/prov.{h,cpp}` | new | NVS config, device id, PROV message handling |
| `firmware/src/net.{h,cpp}` | new | WiFi + mDNS + esp-mqtt task; stubs without `BOARD_HAS_WIFI` |
| `firmware/src/boards/sim/prov_sim.cpp` | new | prov stubs for the sim |
| `firmware/src/ble.{h,cpp}` | modify | PROV characteristic, shared owner gate |
| `firmware/src/boards/sim/ble_sim.cpp` | modify | PROV stubs |
| `firmware/src/main.cpp` | modify | prov/net wiring, `handle_payload`, serial dev cmds |
| `firmware/src/ui.{h,cpp}` | modify | link status from both transports, setup-code overlay |
| `firmware/platformio.ini` | modify | `-DBOARD_HAS_WIFI`, sim filter |
| `daemon/clawdmeter_setup.py`, `daemon/clawdmeter-setup` | new | provisioning CLI + wrapper |
| `daemon/tests/test_setup_cli.py` | new | CLI unit tests |
| `daemon/claude_usage_daemon.py` | modify | broker/announcer side tasks, `run_mqtt`, `run_ble` split |
| `daemon/tests/test_daemon_mqtt.py` | new | transport switch + run_mqtt tests |
| `daemon/claude-usage-daemon.sh` | modify | broker/avahi/sub supervision, `send_payload` |
| `daemon/tests/test_bash_mqtt.sh` | new | bash publish + transport tests |
| `install-mac.sh`, `install.sh`, `config.example`, `README.md`, `CLAUDE.md`, `docs/porting/capability-flags.md` | modify | install + docs |

---

### Task 1: Broker init script

**Files:**
- Create: `daemon/broker/find-tool.sh`, `daemon/broker/broker-init.sh`
- Test: `daemon/tests/test_broker_init.sh`

**Interfaces:**
- Produces: `broker-init.sh [DIR]` (default `~/.config/claude-usage-monitor/broker`; env `CLAWD_BROKER_PORT` overrides 18830). The files are listed in Global Constraints. `clawd_find_tool NAME` echoes the path or returns 1.

- [ ] **Step 1: Install mosquitto on the dev Mac** (needed for the live half of the test)

Run: `brew install mosquitto`
Expected: `/opt/homebrew/sbin/mosquitto` and `/opt/homebrew/bin/mosquitto_passwd` exist. Do **not** `brew services start` it — the daemon owns its own instance.

- [ ] **Step 2: Write the failing test** `daemon/tests/test_broker_init.sh`

```bash
#!/bin/bash
# Test for daemon/broker/broker-init.sh: generated state + a live mosquitto
# enforcing passwords and per-device ACLs. The live half is skipped when
# mosquitto isn't installed.
#
# Run: bash daemon/tests/test_broker_init.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
INIT="$HERE/../broker/broker-init.sh"
. "$HERE/../broker/find-tool.sh"

TMP="$(mktemp -d)"
BPID=""
trap '[ -n "$BPID" ] && kill "$BPID" 2>/dev/null; rm -rf "$TMP"' EXIT
fail=0
check() { if eval "$1"; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }
mode_of() { stat -f %Lp "$1" 2>/dev/null || stat -c %a "$1"; }

export CLAWD_BROKER_PORT=18931   # never collide with a running daemon's 18830
B="$TMP/b"
bash "$INIT" "$B" >/dev/null || { echo "FAIL: broker-init exited non-zero"; exit 1; }

check '[ -f "$B/broker.env" ] && [ -f "$B/passwd" ] && [ -f "$B/acl" ] && [ -f "$B/mosquitto.conf" ] && [ -f "$B/devices" ]' "all state files created"
check '[ "$(mode_of "$B/broker.env")" = 600 ] && [ "$(mode_of "$B/passwd")" = 600 ]' "secrets are 0600"
check '[ "$(mode_of "$B")" = 700 ]' "broker dir is 0700"
. "$B/broker.env"
check '[[ "$BROKER_ID" =~ ^[0-9a-f]{16}$ ]]' "broker id is 16 hex chars"
check '[ "$BROKER_PORT" = 18931 ]' "port honours CLAWD_BROKER_PORT"
check '[ ${#DAEMON_PASS} -eq 24 ]' "daemon password is 24 chars"
check 'grep -q "^allow_anonymous false" "$B/mosquitto.conf"' "anonymous disabled"
check 'grep -q "^listener 18931" "$B/mosquitto.conf"' "listener on configured port"
first_id="$BROKER_ID"; first_pass="$DAEMON_PASS"
bash "$INIT" "$B" >/dev/null
. "$B/broker.env"
check '[ "$BROKER_ID" = "$first_id" ] && [ "$DAEMON_PASS" = "$first_pass" ]' "re-run keeps id and password"

MOSQ="$(clawd_find_tool mosquitto)" || { echo "SKIP: live broker checks (mosquitto not installed)"; exit $fail; }
MPASS="$(clawd_find_tool mosquitto_passwd)"
"$MOSQ" -c "$B/mosquitto.conf" >/dev/null 2>&1 &
BPID=$!
sleep 1
P=(-h 127.0.0.1 -p 18931)

check '! mosquitto_pub "${P[@]}" -t x -m y 2>/dev/null' "anonymous publish refused"
check '! mosquitto_pub "${P[@]}" -u daemon -P wrong -t x -m y 2>/dev/null' "wrong password refused"

"$MPASS" -b "$B/passwd" abc123 devpass
kill -HUP "$BPID"; sleep 0.5
mosquitto_pub "${P[@]}" -u daemon -P "$DAEMON_PASS" -q 1 -r -t clawdmeter/abc123/usage -m '{"s":1}'
mosquitto_pub "${P[@]}" -u daemon -P "$DAEMON_PASS" -q 1 -r -t clawdmeter/ffffff/usage -m '{"s":2}'
got="$(mosquitto_sub "${P[@]}" -u abc123 -P devpass -t clawdmeter/abc123/usage -C 1 -W 3 2>/dev/null)"
check '[ "$got" = "{\"s\":1}" ]' "device reads its own retained usage"
other="$(mosquitto_sub "${P[@]}" -u abc123 -P devpass -t clawdmeter/ffffff/usage -C 1 -W 2 2>/dev/null)"
check '[ -z "$other" ]' "device cannot read another device's usage"

exit $fail
```

- [ ] **Step 3: Run it and confirm it fails**

Run: `bash daemon/tests/test_broker_init.sh`
Expected: error that `daemon/broker/find-tool.sh` does not exist (No such file or directory) and exit non-zero.

- [ ] **Step 4: Implement** `daemon/broker/find-tool.sh`

```bash
# Sourced helper. Homebrew (and Debian) install the mosquitto broker binary in
# sbin, which is not on PATH for launchd/systemd user services.
clawd_find_tool() {
    local name="$1" d
    if command -v "$name" >/dev/null 2>&1; then command -v "$name"; return 0; fi
    for d in /opt/homebrew/sbin /usr/local/sbin /usr/sbin; do
        if [ -x "$d/$name" ]; then echo "$d/$name"; return 0; fi
    done
    return 1
}
```

- [ ] **Step 5: Implement** `daemon/broker/broker-init.sh`

```bash
#!/bin/bash
# One-time setup for the daemon's local MQTT broker. Safe to re-run: keeps the
# existing broker id and daemon password, rewrites the ACL and conf.
#
# Usage: broker-init.sh [DIR]      (default ~/.config/claude-usage-monitor/broker)
#        CLAWD_BROKER_PORT=N        override the listener port (default 18830)
#
# Security model: password auth + ACL, no TLS. Every credential is generated
# here on the user's machine; nothing secret lives in the repo.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/find-tool.sh"

DIR="${1:-$HOME/.config/claude-usage-monitor/broker}"
PORT="${CLAWD_BROKER_PORT:-18830}"

MPASS="$(clawd_find_tool mosquitto_passwd)" || {
    echo "mosquitto_passwd not found — install mosquitto first" >&2
    exit 1
}

rand_hex() { od -An -N"$1" -tx1 /dev/urandom | tr -d ' \n'; }

umask 077
mkdir -p "$DIR/data"
chmod 700 "$DIR"

if [ ! -f "$DIR/broker.env" ]; then
    printf 'BROKER_ID=%s\nBROKER_PORT=%s\nDAEMON_PASS=%s\n' \
        "$(rand_hex 8)" "$PORT" "$(rand_hex 12)" > "$DIR/broker.env"
fi
# shellcheck disable=SC1091
. "$DIR/broker.env"
if [ "$BROKER_PORT" != "$PORT" ] && [ -n "${CLAWD_BROKER_PORT:-}" ]; then
    sed -i.bak "s/^BROKER_PORT=.*/BROKER_PORT=$PORT/" "$DIR/broker.env" && rm -f "$DIR/broker.env.bak"
    BROKER_PORT="$PORT"
fi

[ -f "$DIR/passwd" ] || : > "$DIR/passwd"
"$MPASS" -b "$DIR/passwd" daemon "$DAEMON_PASS"
[ -f "$DIR/devices" ] || : > "$DIR/devices"

# A device's MQTT username IS its device id, so %u patterns pin every device
# to its own topics without editing this file per device.
cat > "$DIR/acl" <<EOF
pattern read clawdmeter/%u/usage
pattern write clawdmeter/%u/refresh
pattern write clawdmeter/%u/status

user daemon
topic write clawdmeter/+/usage
topic read clawdmeter/+/refresh
topic read clawdmeter/+/status
EOF

cat > "$DIR/mosquitto.conf" <<EOF
# Generated by broker-init.sh — re-run it instead of editing.
listener $BROKER_PORT
allow_anonymous false
password_file $DIR/passwd
acl_file $DIR/acl
persistence true
persistence_location $DIR/data/
log_dest stdout
log_type error
log_type warning
log_type notice
EOF

chmod 600 "$DIR"/broker.env "$DIR"/passwd "$DIR"/acl "$DIR"/mosquitto.conf "$DIR"/devices
echo "Broker ready: $DIR (id=$BROKER_ID, port=$BROKER_PORT)"
```

Then: `chmod +x daemon/broker/broker-init.sh`

- [ ] **Step 6: Run the test and confirm it passes**

Run: `bash daemon/tests/test_broker_init.sh`
Expected: every line `PASS:`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add daemon/broker daemon/tests/test_broker_init.sh
git commit -m "daemon: broker-init for the local password-protected mosquitto"
```

---

### Task 2: Python MQTT transport module

**Files:**
- Create: `daemon/clawdmeter_mqtt.py`
- Test: `daemon/tests/test_mqtt_transport.py`

**Interfaces:**
- Consumes: broker dir layout from Task 1.
- Produces:
  - `BROKER_DIR: Path`, `SERVICE_TYPE = "_clawdmeter._tcp.local."`
  - `find_tool(name) -> str | None`
  - `read_broker_env(broker_dir: Path | None = None) -> dict | None`
  - `read_devices(broker_dir: Path | None = None) -> list[str]`
  - `topic(device_id, leaf) -> str`, `device_id_from_topic(t) -> str | None`
  - `class BrokerSupervisor(broker_dir=None, log=print)` with `async run(stop_event)`
  - `class Announcer(broker_id: str, port: int, log=print)` with `async run(stop_event)`
  - `class Publisher(env: dict, loop, on_refresh: Callable[[str], None], log=print)` with `start()`, `stop()`, `publish_usage(device_ids, payload) -> bool`

- [ ] **Step 1: Add dependencies to the dev venv**

Run: `daemon/.venv/bin/pip install "paho-mqtt>=2.0" "zeroconf>=0.132" pytest` (create the venv first with `python3 -m venv daemon/.venv && daemon/.venv/bin/pip install "bleak>=0.22" "httpx>=0.27"` if `daemon/.venv` doesn't exist)

- [ ] **Step 2: Write the failing tests** `daemon/tests/test_mqtt_transport.py`

```python
#!/usr/bin/env python3
"""Tests for daemon/clawdmeter_mqtt.py.

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_mqtt_transport.py -x -q
The live tests start a throwaway mosquitto on port 18932 and are skipped
when mosquitto isn't installed.
"""
import asyncio
import json
import os
import subprocess
import time
from pathlib import Path

import pytest

import daemon.clawdmeter_mqtt as m

REPO = Path(__file__).resolve().parents[2]


def test_read_broker_env_parses_keys(tmp_path):
    (tmp_path / "broker.env").write_text("BROKER_ID=00aa11bb22cc33dd\nBROKER_PORT=18830\nDAEMON_PASS=p\n")
    assert m.read_broker_env(tmp_path) == {"BROKER_ID": "00aa11bb22cc33dd", "BROKER_PORT": "18830", "DAEMON_PASS": "p"}


def test_read_broker_env_missing_or_incomplete_is_none(tmp_path):
    assert m.read_broker_env(tmp_path) is None
    (tmp_path / "broker.env").write_text("BROKER_ID=x\n")
    assert m.read_broker_env(tmp_path) is None


def test_read_devices_keeps_only_valid_ids(tmp_path):
    (tmp_path / "devices").write_text("a1b2c3\n\nnot-an-id\nA1B2C3\nffffff\n")
    assert m.read_devices(tmp_path) == ["a1b2c3", "ffffff"]


def test_topic_round_trip():
    assert m.topic("a1b2c3", "usage") == "clawdmeter/a1b2c3/usage"
    assert m.device_id_from_topic("clawdmeter/a1b2c3/refresh") == "a1b2c3"
    assert m.device_id_from_topic("other/a1b2c3/refresh") is None
    assert m.device_id_from_topic("clawdmeter/a1b2c3") is None


def test_find_tool_falls_back_to_sbin(monkeypatch, tmp_path):
    exe = tmp_path / "mosquitto"
    exe.write_text("#!/bin/sh\n")
    exe.chmod(0o755)
    monkeypatch.setattr(m.shutil, "which", lambda _n: None)
    monkeypatch.setattr(m, "_SBIN_FALLBACKS", (str(tmp_path),))
    assert m.find_tool("mosquitto") == str(exe)
    assert m.find_tool("nope") is None


# ---------------------------------------------------------------- live broker

@pytest.fixture
def live_broker(tmp_path):
    if m.find_tool("mosquitto") is None:
        pytest.skip("mosquitto not installed")
    env = dict(os.environ, CLAWD_BROKER_PORT="18932")
    subprocess.run(["bash", str(REPO / "daemon/broker/broker-init.sh"), str(tmp_path)], check=True, env=env,
                   stdout=subprocess.DEVNULL)
    passwd_tool = m.find_tool("mosquitto_passwd")
    subprocess.run([passwd_tool, "-b", str(tmp_path / "passwd"), "a1b2c3", "devpass"], check=True)
    proc = subprocess.Popen([m.find_tool("mosquitto"), "-c", str(tmp_path / "mosquitto.conf")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    yield m.read_broker_env(tmp_path)
    proc.terminate()
    proc.wait()


def _device_client():
    import paho.mqtt.client as mqtt
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="test-device")
    c.username_pw_set("a1b2c3", "devpass")
    c.connect("127.0.0.1", 18932)
    return c


def test_publisher_retains_usage_and_hears_refresh(live_broker):
    async def scenario():
        loop = asyncio.get_running_loop()
        refreshed = asyncio.Event()
        seen = []
        pub = m.Publisher(live_broker, loop, lambda dev: (seen.append(dev), refreshed.set()))
        pub.start()
        for _ in range(50):
            if pub.is_connected():
                break
            await asyncio.sleep(0.1)
        assert pub.publish_usage(["a1b2c3"], {"s": 12, "ok": True})

        got = []
        dev = _device_client()
        dev.on_message = lambda _c, _u, msg: got.append(msg.payload.decode())
        dev.subscribe("clawdmeter/a1b2c3/usage", qos=1)
        dev.loop_start()
        dev.publish("clawdmeter/a1b2c3/refresh", "1", qos=1)
        await asyncio.wait_for(refreshed.wait(), 5)
        for _ in range(30):
            if got:
                break
            await asyncio.sleep(0.1)
        dev.loop_stop()
        pub.stop()
        assert json.loads(got[0]) == {"s": 12, "ok": True}
        assert seen == ["a1b2c3"]

    asyncio.run(scenario())


def test_broker_supervisor_restarts_and_stops(tmp_path):
    if m.find_tool("mosquitto") is None:
        pytest.skip("mosquitto not installed")
    env = dict(os.environ, CLAWD_BROKER_PORT="18933")
    subprocess.run(["bash", str(REPO / "daemon/broker/broker-init.sh"), str(tmp_path)], check=True, env=env,
                   stdout=subprocess.DEVNULL)

    async def scenario():
        stop = asyncio.Event()
        sup = m.BrokerSupervisor(broker_dir=tmp_path, log=lambda _s: None)
        task = asyncio.create_task(sup.run(stop))
        await asyncio.sleep(1)
        first = int((tmp_path / "broker.pid").read_text())
        os.kill(first, 9)                       # crash it
        await asyncio.sleep(2.5)                # 1 s backoff + start
        second = int((tmp_path / "broker.pid").read_text())
        assert second != first
        stop.set()
        await asyncio.wait_for(task, 5)
        assert not (tmp_path / "broker.pid").exists()

    asyncio.run(scenario())
```

- [ ] **Step 3: Run and confirm failure**

Run: `daemon/.venv/bin/python -m pytest daemon/tests/test_mqtt_transport.py -x -q`
Expected: `ModuleNotFoundError: No module named 'daemon.clawdmeter_mqtt'`

- [ ] **Step 4: Implement** `daemon/clawdmeter_mqtt.py`

```python
"""MQTT transport for the Clawdmeter daemon (macOS / Linux Python daemon).

The daemon runs a local mosquitto (state from daemon/broker/broker-init.sh),
announces it over mDNS as _clawdmeter._tcp, publishes usage JSON retained to
clawdmeter/<id>/usage, and hears refresh requests on clawdmeter/<id>/refresh.
Security model: password auth + ACL, no TLS (see the design spec).
"""

import asyncio
import json
import os
import re
import shutil
import socket
import time
from pathlib import Path
from typing import Callable

BROKER_DIR = Path.home() / ".config" / "claude-usage-monitor" / "broker"
SERVICE_TYPE = "_clawdmeter._tcp.local."
TOPIC_PREFIX = "clawdmeter"
_SBIN_FALLBACKS = ("/opt/homebrew/sbin", "/usr/local/sbin", "/usr/sbin")
_REQUIRED_ENV = {"BROKER_ID", "BROKER_PORT", "DAEMON_PASS"}


def find_tool(name: str) -> str | None:
    """PATH lookup with an sbin fallback — Homebrew installs `mosquitto` in sbin,
    which isn't on PATH under launchd."""
    path = shutil.which(name)
    if path:
        return path
    for d in _SBIN_FALLBACKS:
        cand = Path(d) / name
        if cand.is_file() and os.access(cand, os.X_OK):
            return str(cand)
    return None


def read_broker_env(broker_dir: Path | None = None) -> dict | None:
    f = (broker_dir or BROKER_DIR) / "broker.env"
    if not f.exists():
        return None
    out = {}
    for line in f.read_text().splitlines():
        if "=" in line:
            key, val = line.split("=", 1)
            out[key.strip()] = val.strip()
    return out if _REQUIRED_ENV <= out.keys() else None


def read_devices(broker_dir: Path | None = None) -> list[str]:
    f = (broker_dir or BROKER_DIR) / "devices"
    if not f.exists():
        return []
    return [ln.strip() for ln in f.read_text().splitlines() if re.fullmatch(r"[0-9a-f]{6}", ln.strip())]


def topic(device_id: str, leaf: str) -> str:
    return f"{TOPIC_PREFIX}/{device_id}/{leaf}"


def device_id_from_topic(t: str) -> str | None:
    parts = t.split("/")
    if len(parts) == 3 and parts[0] == TOPIC_PREFIX:
        return parts[1]
    return None


async def _sleep_or_stop(stop_event: asyncio.Event, seconds: float) -> None:
    try:
        await asyncio.wait_for(stop_event.wait(), timeout=seconds)
    except asyncio.TimeoutError:
        pass


class BrokerSupervisor:
    """Keeps one mosquitto child alive; restarts it with backoff if it exits."""

    def __init__(self, broker_dir: Path | None = None, log: Callable[[str], None] = print) -> None:
        self.dir = broker_dir or BROKER_DIR
        self.log = log

    async def run(self, stop_event: asyncio.Event) -> None:
        backoff = 1
        pid_file = self.dir / "broker.pid"
        while not stop_event.is_set():
            exe = find_tool("mosquitto")
            if exe is None:
                self.log("mosquitto not installed; WiFi devices can't connect")
                return
            proc = await asyncio.create_subprocess_exec(exe, "-c", str(self.dir / "mosquitto.conf"))
            pid_file.write_text(str(proc.pid))
            self.log(f"Broker started (pid={proc.pid})")
            started = time.monotonic()
            waiter = asyncio.ensure_future(proc.wait())
            stopper = asyncio.ensure_future(stop_event.wait())
            done, _ = await asyncio.wait({waiter, stopper}, return_when=asyncio.FIRST_COMPLETED)
            if stopper in done:
                proc.terminate()
                await waiter
                pid_file.unlink(missing_ok=True)
                return
            stopper.cancel()
            if time.monotonic() - started > 60:
                backoff = 1
            self.log(f"Broker exited (code {proc.returncode}); restarting in {backoff}s")
            await _sleep_or_stop(stop_event, backoff)
            backoff = min(backoff * 2, 60)
        pid_file.unlink(missing_ok=True)


def lan_ipv4_addresses() -> list[str]:
    from zeroconf import get_all_addresses
    return sorted(a for a in get_all_addresses() if not a.startswith(("127.", "169.254.")))


class Announcer:
    """Advertises the broker as _clawdmeter._tcp with TXT id=<broker_id>, and
    re-announces when this machine's LAN addresses change."""

    def __init__(self, broker_id: str, port: int, log: Callable[[str], None] = print) -> None:
        self.broker_id = broker_id
        self.port = port
        self.log = log
        self.host = socket.gethostname().split(".")[0]

    def _info(self, addrs: list[str]):
        from zeroconf import ServiceInfo
        return ServiceInfo(
            SERVICE_TYPE,
            f"Clawdmeter broker on {self.host}.{SERVICE_TYPE}",
            port=self.port,
            properties={"id": self.broker_id},
            server=f"clawdmeter-broker-{self.host}.local.",
            parsed_addresses=addrs,
        )

    async def run(self, stop_event: asyncio.Event) -> None:
        from zeroconf import IPVersion
        from zeroconf.asyncio import AsyncZeroconf

        addrs = lan_ipv4_addresses()
        while not addrs and not stop_event.is_set():
            await _sleep_or_stop(stop_event, 10)
            addrs = lan_ipv4_addresses()
        if stop_event.is_set():
            return
        azc = AsyncZeroconf(ip_version=IPVersion.V4Only)
        info = self._info(addrs)
        await azc.async_register_service(info)
        self.log(f"mDNS: announced broker on {', '.join(addrs)}:{self.port}")
        try:
            while not stop_event.is_set():
                await _sleep_or_stop(stop_event, 60)
                new = lan_ipv4_addresses()
                if new and new != addrs:
                    addrs = new
                    info = self._info(addrs)
                    await azc.async_update_service(info)
                    self.log(f"mDNS: addresses changed -> {', '.join(addrs)}")
        finally:
            await azc.async_unregister_all_services()
            await azc.async_close()


class Publisher:
    """paho-mqtt client (own network thread) logged in as the daemon user."""

    def __init__(self, env: dict, loop: asyncio.AbstractEventLoop,
                 on_refresh: Callable[[str], None], log: Callable[[str], None] = print) -> None:
        import paho.mqtt.client as mqtt
        self._mqtt = mqtt
        self.loop = loop
        self.on_refresh = on_refresh
        self.log = log
        self.port = int(env["BROKER_PORT"])
        self._c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                              client_id=f"clawdmeter-daemon-{socket.gethostname()}")
        self._c.username_pw_set("daemon", env["DAEMON_PASS"])
        self._c.on_connect = self._on_connect
        self._c.on_message = self._on_message
        self._c.reconnect_delay_set(1, 30)

    def start(self) -> None:
        self._c.connect_async("127.0.0.1", self.port, keepalive=30)
        self._c.loop_start()

    def stop(self) -> None:
        self._c.disconnect()
        self._c.loop_stop()

    def is_connected(self) -> bool:
        return self._c.is_connected()

    def _on_connect(self, client, _userdata, _flags, reason_code, _props=None) -> None:
        if reason_code.is_failure:
            self.log(f"MQTT: local broker refused login ({reason_code})")
            return
        client.subscribe(topic("+", "refresh"), qos=1)
        self.log("MQTT: connected to local broker")

    def _on_message(self, _client, _userdata, msg) -> None:
        dev = device_id_from_topic(msg.topic)
        if dev:
            self.loop.call_soon_threadsafe(self.on_refresh, dev)

    def publish_usage(self, device_ids: list[str], payload: dict) -> bool:
        if not self._c.is_connected():
            return False
        data = json.dumps(payload, separators=(",", ":"))
        ok = True
        for dev in device_ids:
            info = self._c.publish(topic(dev, "usage"), data, qos=1, retain=True)
            ok = ok and info.rc == self._mqtt.MQTT_ERR_SUCCESS
        return ok
```

- [ ] **Step 5: Run tests, confirm pass**

Run: `daemon/.venv/bin/python -m pytest daemon/tests/test_mqtt_transport.py -x -q`
Expected: `7 passed`

- [ ] **Step 6: Commit**

```bash
git add daemon/clawdmeter_mqtt.py daemon/tests/test_mqtt_transport.py
git commit -m "daemon: Python MQTT transport (broker supervisor, mDNS announcer, publisher)"
```

---

### Task 3: Firmware `netcfg` pure logic

**Files:**
- Create: `firmware/src/netcfg.h`, `firmware/src/netcfg.cpp`
- Test: `firmware/test/test_netcfg/test_main.cpp`

**Interfaces:**
- Produces (exact):

```cpp
struct NetConfig { char ssid[33]; char wpass[65]; char broker_id[17]; char pass[33]; };
bool netcfg_is_valid(const NetConfig* c);
bool netcfg_parse(const char* json, NetConfig* out);      // false on bad JSON / missing / over-long field
enum prov_result_t { PROV_OK, PROV_BAD_CODE, PROV_LOCKED, PROV_NOT_ACTIVE, PROV_BAD_JSON };
struct ProvSession { bool active; char code[7]; uint32_t expires_ms; uint8_t fails; };
static const uint32_t PROV_CODE_TTL_MS = 120000;
static const uint8_t  PROV_MAX_FAILS   = 5;
void prov_session_begin(ProvSession* s, uint32_t rand32, uint32_t now_ms);
bool prov_session_visible(const ProvSession* s, uint32_t now_ms);
prov_result_t prov_session_apply(ProvSession* s, const char* json, uint32_t now_ms, NetConfig* out);
void netcfg_topic(char* out, size_t n, const char* device_id, const char* leaf);
```

- [ ] **Step 1: Write the failing test** `firmware/test/test_netcfg/test_main.cpp`

```cpp
// Host unit test for netcfg — the pure provisioning logic (config JSON, setup
// code session, topic names). No Arduino/LVGL deps. From firmware/:
//
//   g++ -std=c++17 -I src -I .pio/libdeps/sim/ArduinoJson/src \
//       test/test_netcfg/test_main.cpp src/netcfg.cpp -o /tmp/t_netcfg && /tmp/t_netcfg

#include "netcfg.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const char* GOOD =
    "{\"op\":\"config\",\"code\":\"123456\",\"ssid\":\"Home \\\"5G\\\": up\","
    "\"wpass\":\"p@ss:word\",\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"devpass\"}";

static void should_parse_config_with_json_hostile_chars() {
    NetConfig c = {};
    CHECK(netcfg_parse(GOOD, &c));
    CHECK(strcmp(c.ssid, "Home \"5G\": up") == 0);
    CHECK(strcmp(c.wpass, "p@ss:word") == 0);
    CHECK(strcmp(c.broker_id, "00aa11bb22cc33dd") == 0);
    CHECK(strcmp(c.pass, "devpass") == 0);
    CHECK(netcfg_is_valid(&c));
}

static void should_accept_max_length_ssid_and_pass_and_reject_longer() {
    std::string ssid32(32, 's'), pass63(63, 'p');
    std::string ok = "{\"ssid\":\"" + ssid32 + "\",\"wpass\":\"" + pass63 +
                     "\",\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"x\"}";
    NetConfig c = {};
    CHECK(netcfg_parse(ok.c_str(), &c));
    CHECK(strlen(c.ssid) == 32 && strlen(c.wpass) == 63);

    std::string too_long = "{\"ssid\":\"" + ssid32 + "s\",\"wpass\":\"x\","
                           "\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"x\"}";
    CHECK(!netcfg_parse(too_long.c_str(), &c));   // never silently truncate
}

static void should_reject_missing_fields_and_bad_json() {
    NetConfig c = {};
    CHECK(!netcfg_parse("{\"ssid\":\"a\"}", &c));
    CHECK(!netcfg_parse("not json", &c));
    CHECK(!netcfg_parse("{\"ssid\":\"a\",\"wpass\":\"b\",\"broker_id\":\"short\",\"pass\":\"x\"}", &c));
}

static void should_allow_open_network_with_empty_wifi_password() {
    NetConfig c = {};
    CHECK(netcfg_parse("{\"ssid\":\"cafe\",\"wpass\":\"\",\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"x\"}", &c));
    CHECK(netcfg_is_valid(&c));
}

static void should_generate_six_digit_code_and_expire() {
    ProvSession s = {};
    prov_session_begin(&s, 42u, 1000);
    CHECK(strlen(s.code) == 6);
    CHECK(strcmp(s.code, "000042") == 0);
    CHECK(prov_session_visible(&s, 1000));
    CHECK(prov_session_visible(&s, 1000 + PROV_CODE_TTL_MS - 1));
    CHECK(!prov_session_visible(&s, 1000 + PROV_CODE_TTL_MS));
}

static void should_handle_millis_wraparound() {
    ProvSession s = {};
    prov_session_begin(&s, 1u, 0xFFFFFF00u);
    CHECK(prov_session_visible(&s, 0x00000100u));   // 512 ms later, after wrap
}

static void should_apply_with_right_code_and_end_session() {
    ProvSession s = {};
    prov_session_begin(&s, 123456u, 0);
    NetConfig c = {};
    CHECK(prov_session_apply(&s, GOOD, 10, &c) == PROV_OK);
    CHECK(strcmp(c.pass, "devpass") == 0);
    CHECK(!s.active);
    CHECK(prov_session_apply(&s, GOOD, 20, &c) == PROV_NOT_ACTIVE);
}

static void should_count_wrong_codes_and_lock_after_max() {
    ProvSession s = {};
    prov_session_begin(&s, 999999u, 0);
    NetConfig c = {};
    for (int i = 1; i < PROV_MAX_FAILS; i++) CHECK(prov_session_apply(&s, GOOD, 10, &c) == PROV_BAD_CODE);
    CHECK(prov_session_apply(&s, GOOD, 10, &c) == PROV_LOCKED);
    CHECK(!s.active);
}

static void should_not_count_bad_json_as_a_wrong_code() {
    ProvSession s = {};
    prov_session_begin(&s, 123456u, 0);
    NetConfig c = {};
    CHECK(prov_session_apply(&s, "{\"op\":\"config\",\"code\":\"123456\"}", 10, &c) == PROV_BAD_JSON);
    CHECK(s.active && s.fails == 0);
}

static void should_reject_after_expiry() {
    ProvSession s = {};
    prov_session_begin(&s, 123456u, 0);
    NetConfig c = {};
    CHECK(prov_session_apply(&s, GOOD, PROV_CODE_TTL_MS + 1, &c) == PROV_NOT_ACTIVE);
}

static void should_build_topics() {
    char t[48];
    netcfg_topic(t, sizeof t, "a1b2c3", "usage");
    CHECK(strcmp(t, "clawdmeter/a1b2c3/usage") == 0);
}

int main() {
    should_parse_config_with_json_hostile_chars();
    should_accept_max_length_ssid_and_pass_and_reject_longer();
    should_reject_missing_fields_and_bad_json();
    should_allow_open_network_with_empty_wifi_password();
    should_generate_six_digit_code_and_expire();
    should_handle_millis_wraparound();
    should_apply_with_right_code_and_end_session();
    should_count_wrong_codes_and_lock_after_max();
    should_not_count_bad_json_as_a_wrong_code();
    should_reject_after_expiry();
    should_build_topics();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all netcfg tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run and confirm failure**

Run (from `firmware/`): `pio pkg install -e sim >/dev/null; g++ -std=c++17 -I src -I .pio/libdeps/sim/ArduinoJson/src test/test_netcfg/test_main.cpp src/netcfg.cpp -o /tmp/t_netcfg && /tmp/t_netcfg`
Expected: compile error `netcfg.h: No such file or directory`

- [ ] **Step 3: Implement** `firmware/src/netcfg.h`

```cpp
#pragma once
// WiFi + MQTT settings pushed by clawdmeter-setup over the BLE PROV
// characteristic, plus the setup-code session that gates them. Pure logic, no
// Arduino deps — host-tested by test/test_netcfg.
#include <stddef.h>
#include <stdint.h>

struct NetConfig {
    char ssid[33];       // 802.11 SSID max 32 bytes
    char wpass[65];      // WPA2 passphrase max 63 (or 64-hex PSK); empty = open network
    char broker_id[17];  // 16 hex chars from broker-init.sh (matches the mDNS TXT id)
    char pass[33];       // MQTT password; username is the device id
};

bool netcfg_is_valid(const NetConfig* c);
// Fills *out from {"ssid","wpass","broker_id","pass"}. Rejects missing or
// over-long fields instead of truncating — a cut password fails silently later.
bool netcfg_parse(const char* json, NetConfig* out);

enum prov_result_t { PROV_OK, PROV_BAD_CODE, PROV_LOCKED, PROV_NOT_ACTIVE, PROV_BAD_JSON };

struct ProvSession {
    bool     active;
    char     code[7];
    uint32_t expires_ms;
    uint8_t  fails;
};

static const uint32_t PROV_CODE_TTL_MS = 120000;
static const uint8_t  PROV_MAX_FAILS   = 5;

void prov_session_begin(ProvSession* s, uint32_t rand32, uint32_t now_ms);
bool prov_session_visible(const ProvSession* s, uint32_t now_ms);
// Checks the message's "code" against the session; on a match parses the
// config into *out and ends the session. Bad JSON doesn't count as a wrong code.
prov_result_t prov_session_apply(ProvSession* s, const char* json, uint32_t now_ms, NetConfig* out);

// "clawdmeter/<device_id>/<leaf>"
void netcfg_topic(char* out, size_t n, const char* device_id, const char* leaf);
```

- [ ] **Step 4: Implement** `firmware/src/netcfg.cpp`

```cpp
#include "netcfg.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

static bool copy_field(JsonDocument& doc, const char* key, char* dst, size_t cap, bool allow_empty) {
    const char* v = doc[key] | (const char*)nullptr;
    if (!v) return false;
    size_t len = strlen(v);
    if (len >= cap || (!allow_empty && len == 0)) return false;
    memcpy(dst, v, len + 1);
    return true;
}

static bool is_hex16(const char* s) {
    if (strlen(s) != 16) return false;
    for (const char* p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    }
    return true;
}

bool netcfg_is_valid(const NetConfig* c) {
    return c->ssid[0] != '\0' && is_hex16(c->broker_id) && c->pass[0] != '\0';
}

bool netcfg_parse(const char* json, NetConfig* out) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    NetConfig c = {};
    if (!copy_field(doc, "ssid", c.ssid, sizeof c.ssid, false)) return false;
    if (!copy_field(doc, "wpass", c.wpass, sizeof c.wpass, true)) return false;
    if (!copy_field(doc, "broker_id", c.broker_id, sizeof c.broker_id, false)) return false;
    if (!copy_field(doc, "pass", c.pass, sizeof c.pass, false)) return false;
    if (!netcfg_is_valid(&c)) return false;
    *out = c;
    return true;
}

void prov_session_begin(ProvSession* s, uint32_t rand32, uint32_t now_ms) {
    s->active = true;
    snprintf(s->code, sizeof s->code, "%06lu", (unsigned long)(rand32 % 1000000u));
    s->expires_ms = now_ms + PROV_CODE_TTL_MS;
    s->fails = 0;
}

bool prov_session_visible(const ProvSession* s, uint32_t now_ms) {
    return s->active && (int32_t)(now_ms - s->expires_ms) < 0;
}

prov_result_t prov_session_apply(ProvSession* s, const char* json, uint32_t now_ms, NetConfig* out) {
    if (!prov_session_visible(s, now_ms)) {
        s->active = false;
        return PROV_NOT_ACTIVE;
    }
    JsonDocument doc;
    if (deserializeJson(doc, json)) return PROV_BAD_JSON;
    const char* code = doc["code"] | "";
    if (strcmp(code, s->code) != 0) {
        if (++s->fails >= PROV_MAX_FAILS) {
            s->active = false;
            return PROV_LOCKED;
        }
        return PROV_BAD_CODE;
    }
    if (!netcfg_parse(json, out)) return PROV_BAD_JSON;
    s->active = false;
    return PROV_OK;
}

void netcfg_topic(char* out, size_t n, const char* device_id, const char* leaf) {
    snprintf(out, n, "clawdmeter/%s/%s", device_id, leaf);
}
```

- [ ] **Step 5: Run test, confirm pass**

Run (from `firmware/`): `g++ -std=c++17 -I src -I .pio/libdeps/sim/ArduinoJson/src test/test_netcfg/test_main.cpp src/netcfg.cpp -o /tmp/t_netcfg && /tmp/t_netcfg`
Expected: `all netcfg tests passed`

- [ ] **Step 6: Commit**

```bash
git add firmware/src/netcfg.h firmware/src/netcfg.cpp firmware/test/test_netcfg
git commit -m "firmware: netcfg — provisioning config parse + setup-code session (host-tested)"
```

---

### Task 4: Firmware `prov` + BLE PROV characteristic

**Files:**
- Create: `firmware/src/prov.h`, `firmware/src/prov.cpp`, `firmware/src/boards/sim/prov_sim.cpp`
- Modify: `firmware/src/ble.h`, `firmware/src/ble.cpp`, `firmware/src/boards/sim/ble_sim.cpp`, `firmware/src/main.cpp`, `firmware/platformio.ini` (sim filter + `-DBOARD_HAS_WIFI` on 5 S3 envs)

**Interfaces:**
- Consumes: `netcfg.h` from Task 3.
- Produces:

```cpp
// prov.h
void prov_init(void);
const char* prov_device_id(void);        // 6 lowercase hex, e.g. "a1b2c3"
bool prov_is_configured(void);
const NetConfig* prov_config(void);
bool prov_handle_write(const char* json, char* resp, size_t resp_n);  // true = new config saved
bool prov_apply_unchecked(const char* json);  // serial dev path, no code
bool prov_code_visible(void);
const char* prov_code(void);
void prov_clear(void);
bool prov_take_net_up_pending(void);
// ble.h additions
bool ble_has_prov(void);
const char* ble_get_prov(void);
void ble_prov_notify(const char* json);
```

- [ ] **Step 1: Add the capability flag.** In `firmware/platformio.ini`, inside `build_flags` of each of `[env:waveshare_amoled_216]`, `[env:waveshare_lcd_154]`, `[env:waveshare_lcd_4]`, `[env:waveshare_amoled_18]`, `[env:waveshare_amoled_206]`, add directly under the `-DBOARD_HAS_PSRAM` line:

```ini
    ; WiFi + MQTT transport (S3 only; C6 has no PSRAM headroom — see the
    ; 2026-10-08 wifi-mqtt design spec)
    -DBOARD_HAS_WIFI
```

In `[env:sim]` `build_src_filter`, after `-<ble.cpp>`, add `-<prov.cpp>`.

- [ ] **Step 2: Create** `firmware/src/prov.h`

```cpp
#pragma once
// Provisioning: the WiFi + MQTT config clawdmeter-setup pushes over the BLE
// PROV characteristic, persisted in NVS (namespace "clawdnet"). All calls are
// loop-task only — ble.cpp hands PROV writes over via ble_has_prov().
#include <stddef.h>
#include "netcfg.h"

void prov_init(void);
const char* prov_device_id(void);          // 6 lowercase hex — also the MQTT username
bool prov_is_configured(void);
const NetConfig* prov_config(void);

// Handles one PROV message ({"op":"begin"} / {"op":"config",...}); writes the
// JSON reply to notify into resp. Returns true when a new config was saved.
bool prov_handle_write(const char* json, char* resp, size_t resp_n);
// Serial dev path: save a config without a setup code. USB access already
// means physical control of the device (it can be reflashed), so this adds no
// new attack surface.
bool prov_apply_unchecked(const char* json);

bool prov_code_visible(void);
const char* prov_code(void);
void prov_clear(void);                      // part of the hold-to-pair reset
bool prov_take_net_up_pending(void);        // true once after a config is applied
```

- [ ] **Step 3: Create** `firmware/src/prov.cpp`

```cpp
#include "prov.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>

static NetConfig   cfg = {};
static bool        configured = false;
static ProvSession sess = {};
static char        dev_id[7];
static bool        net_up_pending = false;

static void load(void) {
    Preferences p;
    p.begin("clawdnet", true);
    p.getString("ssid", cfg.ssid, sizeof cfg.ssid);
    p.getString("wpass", cfg.wpass, sizeof cfg.wpass);
    p.getString("broker", cfg.broker_id, sizeof cfg.broker_id);
    p.getString("pass", cfg.pass, sizeof cfg.pass);
    p.end();
    configured = netcfg_is_valid(&cfg);
}

static void save(void) {
    Preferences p;
    p.begin("clawdnet", false);
    p.putString("ssid", cfg.ssid);
    p.putString("wpass", cfg.wpass);
    p.putString("broker", cfg.broker_id);
    p.putString("pass", cfg.pass);
    p.end();
}

static void adopt(const NetConfig& next) {
    cfg = next;
    configured = true;
    save();
    net_up_pending = true;
    Serial.printf("PROV: saved config (ssid=%s broker=%s)\n", cfg.ssid, cfg.broker_id);
}

void prov_init(void) {
    // Last 3 bytes of the factory MAC — stable per chip, readable on a label.
    uint64_t mac = ESP.getEfuseMac();
    snprintf(dev_id, sizeof dev_id, "%02x%02x%02x",
             (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
    load();
    Serial.printf("PROV: device id %s, %s\n", dev_id,
                  configured ? "WiFi configured" : "no WiFi config");
}

const char* prov_device_id(void) { return dev_id; }
bool prov_is_configured(void) { return configured; }
const NetConfig* prov_config(void) { return &cfg; }

bool prov_handle_write(const char* json, char* resp, size_t resp_n) {
#ifndef BOARD_HAS_WIFI
    (void)json;
    snprintf(resp, resp_n, "{\"err\":\"nowifi\"}");
    return false;
#else
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        snprintf(resp, resp_n, "{\"err\":\"json\"}");
        return false;
    }
    const char* op = doc["op"] | "";
    if (strcmp(op, "begin") == 0) {
        prov_session_begin(&sess, esp_random(), millis());
        Serial.println("PROV: setup code shown");
        snprintf(resp, resp_n, "{\"id\":\"%s\"}", dev_id);
        return false;
    }
    if (strcmp(op, "config") == 0) {
        NetConfig next = {};
        switch (prov_session_apply(&sess, json, millis(), &next)) {
        case PROV_OK:
            adopt(next);
            snprintf(resp, resp_n, "{\"ok\":true}");
            return true;
        case PROV_BAD_CODE:   snprintf(resp, resp_n, "{\"err\":\"code\"}");    return false;
        case PROV_LOCKED:     snprintf(resp, resp_n, "{\"err\":\"locked\"}");  return false;
        case PROV_NOT_ACTIVE: snprintf(resp, resp_n, "{\"err\":\"expired\"}"); return false;
        case PROV_BAD_JSON:   snprintf(resp, resp_n, "{\"err\":\"json\"}");    return false;
        }
    }
    snprintf(resp, resp_n, "{\"err\":\"op\"}");
    return false;
#endif
}

bool prov_apply_unchecked(const char* json) {
    NetConfig next = {};
    if (!netcfg_parse(json, &next)) return false;
    adopt(next);
    return true;
}

bool prov_code_visible(void) { return prov_session_visible(&sess, millis()); }
const char* prov_code(void) { return sess.code; }

void prov_clear(void) {
    cfg = {};
    configured = false;
    sess.active = false;
    net_up_pending = false;
    Preferences p;
    p.begin("clawdnet", false);
    p.clear();
    p.end();
    Serial.println("PROV: WiFi config cleared");
}

bool prov_take_net_up_pending(void) {
    bool v = net_up_pending;
    net_up_pending = false;
    return v;
}
```

- [ ] **Step 4: Create** `firmware/src/boards/sim/prov_sim.cpp`

```cpp
// prov.h stub for the desktop simulator: no NVS, no WiFi — the sim gets its
// data from scenario playback (ble_sim.cpp), so provisioning is always off.
#include "../../prov.h"
#include <stdio.h>

static NetConfig empty_cfg = {};

void prov_init(void) {}
const char* prov_device_id(void) { return "000000"; }
bool prov_is_configured(void) { return false; }
const NetConfig* prov_config(void) { return &empty_cfg; }
bool prov_handle_write(const char*, char* resp, size_t n) {
    snprintf(resp, n, "{\"err\":\"nowifi\"}");
    return false;
}
bool prov_apply_unchecked(const char*) { return false; }
bool prov_code_visible(void) { return false; }
const char* prov_code(void) { return ""; }
void prov_clear(void) {}
bool prov_take_net_up_pending(void) { return false; }
```

- [ ] **Step 5: BLE PROV characteristic.** In `firmware/src/ble.h`, add after `void ble_request_refresh(void);`:

```cpp
// Provisioning channel (PROV char ...0005): writes are handed to the loop task
// the same way usage data is; replies go back as notifications.
bool ble_has_prov(void);
const char* ble_get_prov(void);
void ble_prov_notify(const char* json);
```

In `firmware/src/ble.cpp`:

(a) Add the UUID after `REQ_CHAR_UUID` (line 13):

```cpp
#define PROV_CHAR_UUID      "4c41555a-4465-7669-6365-000000000005"  // WiFi/MQTT provisioning
```

(b) Add statics after `static NimBLECharacteristic* req_char = nullptr;`:

```cpp
static NimBLECharacteristic* prov_char = nullptr;
static char prov_buf[BLE_BUF_SIZE];
static volatile bool prov_ready = false;
```

(c) Replace the gate at the top of `RxCallbacks::onWrite` (lines 264–279) with a shared helper placed directly above `class RxCallbacks`:

```cpp
// Only accept writes over a bonded+encrypted link, and only from the owner
// machine. Another machine's daemon in range is ignored so the display never
// rotates to a foreign account. The first encrypted writer claims ownership
// when none is set yet (e.g. a fresh pairing). Shared by RX and PROV.
static bool owner_write_allowed(NimBLEConnInfo& info, const char* what) {
    std::string id = info.getIdAddress().toString();
    if (!info.isEncrypted()) {
        Serial.printf("BLE: dropping %s write from unencrypted link\n", what);
        return false;
    }
    if (!owner_set && id != ZERO_ADDR) {
        claim_owner(id);
    }
    if (owner_set && strcmp(id.c_str(), owner_addr) != 0) {
        Serial.printf("BLE: dropping %s write from non-owner %s\n", what, id.c_str());
        return false;
    }
    return true;
}
```

and make `RxCallbacks::onWrite`:

```cpp
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        if (!owner_write_allowed(info, "RX")) return;
        std::string val = chr->getValue();
        size_t len = std::min(val.length(), (size_t)(BLE_BUF_SIZE - 1));
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
    }
```

(d) Add after `class ReqCallbacks {...};`:

```cpp
class ProvCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        if (!owner_write_allowed(info, "PROV")) return;
        std::string val = chr->getValue();
        size_t len = std::min(val.length(), (size_t)(BLE_BUF_SIZE - 1));
        memcpy(prov_buf, val.c_str(), len);
        prov_buf[len] = '\0';
        prov_ready = true;
    }
};
```

(e) In `ble_init()`, after `req_char->setCallbacks(&reqCb);`:

```cpp
    prov_char = svc->createCharacteristic(
        PROV_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
    );
    static ProvCallbacks provCb;
    prov_char->setCallbacks(&provCb);
```

(f) Append at the end of the file:

```cpp
bool ble_has_prov(void) {
    return prov_ready;
}

const char* ble_get_prov(void) {
    prov_ready = false;
    return prov_buf;
}

void ble_prov_notify(const char* json) {
    if (state == BLE_STATE_CONNECTED && prov_char) {
        prov_char->setValue(json);
        prov_char->notify();
    }
}
```

- [ ] **Step 6: Sim BLE stubs.** Append to `firmware/src/boards/sim/ble_sim.cpp`:

```cpp
bool ble_has_prov(void) { return false; }
const char* ble_get_prov(void) { return ""; }
void ble_prov_notify(const char* json) { printf("[sim] PROV notify %s\n", json); }
```

- [ ] **Step 7: Wire into `firmware/src/main.cpp`.**

(a) `#include "prov.h"` after `#include "ble.h"`.

(b) Grow the serial buffer (a `prov {...}` line carries a whole config): change `#define CMD_BUF_SIZE 64` to `#define CMD_BUF_SIZE 384`.

(c) In `check_serial_cmd()`, extend the dispatch chain:

```cpp
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            // Dev aids for the WiFi transport (no setup CLI needed):
            //   prov {"ssid":..,"wpass":..,"broker_id":..,"pass":..}
            //   provbegin   → show a setup code, as clawdmeter-setup would
            else if (strncmp(cmd_buf, "prov ", 5) == 0)
                Serial.println(prov_apply_unchecked(cmd_buf + 5) ? "PROV_OK" : "PROV_ERR");
            else if (strcmp(cmd_buf, "provbegin") == 0) {
                char resp[64];
                prov_handle_write("{\"op\":\"begin\"}", resp, sizeof resp);
                Serial.printf("%s code=%s\n", resp, prov_code());
            }
```

(d) In `setup()`, right after `ble_init();`: `prov_init();`

(e) In `pair_tick()`, inside `if (pair_state == PAIR_ARMED) {`, after `ble_clear_bonds();` add `prov_clear();` and update the log line to `"Pair: released in window — clearing bonds + WiFi config, advertising"`.

(f) In `loop()`, directly before `check_serial_cmd();`:

```cpp
    if (ble_has_prov()) {
        char resp[64];
        prov_handle_write(ble_get_prov(), resp, sizeof resp);
        ble_prov_notify(resp);
    }
```

- [ ] **Step 8: Build every env**

Run: `for e in waveshare_amoled_216 waveshare_amoled_18 waveshare_amoled_206 waveshare_lcd_154 waveshare_lcd_4 waveshare_amoled_216_c6 waveshare_amoled_18_c6 sim; do pio run -d firmware -e $e >/tmp/build_$e.log 2>&1 && echo "OK $e" || echo "FAIL $e"; done`
Expected: `OK` for all 8. (sim needs `brew install sdl2`; if SDL2 is absent, record the sim as skipped, not passed.)

- [ ] **Step 9: Hardware check (S3 board on USB)**

Flash: `pio run -d firmware -e <your_s3_env> -t upload`. Then in `pio device monitor -d firmware -e <env>`:
1. Type `provbegin` → expect `{"id":"xxxxxx"} code=NNNNNN`.
2. Type `prov {"ssid":"x","wpass":"y","broker_id":"00aa11bb22cc33dd","pass":"z"}` → expect `PROV_OK`.
3. Press the board's reset → boot log shows `PROV: device id xxxxxx, WiFi configured`.

- [ ] **Step 10: Commit**

```bash
git add firmware/src/prov.h firmware/src/prov.cpp firmware/src/boards/sim/prov_sim.cpp \
        firmware/src/ble.h firmware/src/ble.cpp firmware/src/boards/sim/ble_sim.cpp \
        firmware/src/main.cpp firmware/platformio.ini
git commit -m "firmware: BLE PROV characteristic + NVS-backed WiFi/MQTT provisioning"
```

---

### Task 5: Firmware `net` — WiFi, mDNS discovery, MQTT

**Files:**
- Create: `firmware/src/net.h`, `firmware/src/net.cpp`
- Modify: `firmware/src/main.cpp`, possibly `firmware/platformio.ini` (2.16 partition, Step 6)

**Interfaces:**
- Consumes: `prov_config()`, `prov_device_id()`, `prov_take_net_up_pending()` (Task 4); `netcfg_topic`, `netcfg_is_valid` (Task 3); `ble_prov_notify` (Task 4).
- Produces:

```cpp
enum net_state_t { NET_OFF, NET_WIFI, NET_DISCOVERING, NET_CONNECTED, NET_AUTH_FAIL };
void net_init(void);
void net_apply_config(void);   // re-read prov_config() (after setup or clear)
net_state_t net_get_state(void);
bool net_has_data(void);
const char* net_get_data(void);
```

- [ ] **Step 1: Spike the esp-mqtt header.** Create `firmware/src/net.h` (below) and a `net.cpp` containing only `#include "net.h"` + `#ifdef BOARD_HAS_WIFI` `#include <mqtt_client.h>` `#endif`, then run `pio run -d firmware -e waveshare_amoled_18`.
Expected: builds. If it fails with `mqtt_client.h: No such file or directory`, stop and tell the user — the fallback is `knolleary/PubSubClient@^2.8`, which changes Step 2's client code. Don't improvise.

`firmware/src/net.h`:

```cpp
#pragma once
// WiFi + MQTT transport for usage data. Mirrors ble.h's polling shape so
// main.cpp treats both sources alike. Compiles to stubs without BOARD_HAS_WIFI
// (C6 boards, sim).

enum net_state_t {
    NET_OFF,          // not configured (or no WiFi on this board)
    NET_WIFI,         // joining the WiFi network
    NET_DISCOVERING,  // on WiFi, looking for / connecting to the broker
    NET_CONNECTED,    // MQTT session up
    NET_AUTH_FAIL,    // broker rejected our login — re-run clawdmeter-setup
};

void net_init(void);
void net_apply_config(void);
net_state_t net_get_state(void);
bool net_has_data(void);
const char* net_get_data(void);
```

- [ ] **Step 2: Implement** `firmware/src/net.cpp`

```cpp
// WiFi + mDNS + MQTT transport. Usage JSON arrives on clawdmeter/<id>/usage,
// retained, so every reconnect immediately gets the latest value. The device
// finds the broker by browsing _clawdmeter._tcp for the broker id it was
// paired with, so the host's IP may change between sessions.
//
// All blocking work (WiFi join, the ~3 s mDNS browse, esp-mqtt start/stop)
// runs on its own FreeRTOS task so the LVGL loop never stalls. esp-mqtt's
// event handler runs on yet another task; payloads cross to the loop task
// through a spinlock-guarded buffer.
#include "net.h"

#ifndef BOARD_HAS_WIFI

void net_init(void) {}
void net_apply_config(void) {}
net_state_t net_get_state(void) { return NET_OFF; }
bool net_has_data(void) { return false; }
const char* net_get_data(void) { return ""; }

#else

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <mqtt_client.h>
#include "netcfg.h"
#include "prov.h"

#define NET_BUF_SIZE 512
static const uint32_t REDISCOVER_AFTER_MS = 20000;  // broker unreachable this long → browse again
static const uint32_t BROWSE_RETRY_MS     = 15000;  // between failed browses (laptop away)

static TaskHandle_t  net_task_h = nullptr;
static portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;
static char          rx_buf[NET_BUF_SIZE];    // written on the esp-mqtt task
static char          out_buf[NET_BUF_SIZE];   // handed to the loop task
static volatile bool data_ready = false;
static volatile bool mqtt_up = false;
static volatile bool auth_failed = false;
static volatile bool reconfig = false;
static volatile net_state_t state = NET_OFF;
static volatile uint32_t mqtt_down_since = 0;

static NetConfig pending = {};   // written by the loop task under mux
static NetConfig active  = {};   // net task's private copy
static esp_mqtt_client_handle_t client = nullptr;
static char t_usage[40], t_refresh[40], t_status[40];
static char uri[40], client_id[24], hostname[24];

static void mqtt_event(void*, esp_event_base_t, int32_t id, void* data) {
    auto* ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_up = true;
        auth_failed = false;
        esp_mqtt_client_subscribe_single(ev->client, t_usage, 1);   // the non-_single name is a C-only _Generic macro
        esp_mqtt_client_publish(ev->client, t_status, "online", 0, 1, 1);
        esp_mqtt_client_publish(ev->client, t_refresh, "1", 0, 1, 0);  // ask the daemon to poll now
        Serial.println("NET: MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (mqtt_up) mqtt_down_since = millis();
        mqtt_up = false;
        break;
    case MQTT_EVENT_DATA:
        // Usage payloads are < 512 B, so they always arrive in one piece.
        if (ev->topic_len == (int)strlen(t_usage) &&
            memcmp(ev->topic, t_usage, ev->topic_len) == 0 &&
            ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
            int len = ev->data_len < NET_BUF_SIZE - 1 ? ev->data_len : NET_BUF_SIZE - 1;
            portENTER_CRITICAL(&mux);
            memcpy(rx_buf, ev->data, len);
            rx_buf[len] = '\0';
            data_ready = true;
            portEXIT_CRITICAL(&mux);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle &&
            ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            esp_mqtt_connect_return_code_t rc = ev->error_handle->connect_return_code;
            if (rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                auth_failed = true;
                Serial.println("NET: broker rejected login — re-run clawdmeter-setup");
            }
        }
        break;
    default:
        break;
    }
}

static void mqtt_stop(void) {
    if (!client) return;
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
    mqtt_up = false;
}

// Browse _clawdmeter._tcp for the broker this device was paired with.
static bool discover(IPAddress* ip, uint16_t* port) {
    int n = MDNS.queryService("clawdmeter", "tcp");
    for (int i = 0; i < n; i++) {
        if (MDNS.txt(i, "id") == active.broker_id) {
            *ip = MDNS.address(i);
            *port = MDNS.port(i);
            return true;
        }
    }
    return false;
}

static void mqtt_start(IPAddress ip, uint16_t port) {
    snprintf(uri, sizeof uri, "mqtt://%s:%u", ip.toString().c_str(), port);
    esp_mqtt_client_config_t c = {};
    c.broker.address.uri = uri;
    c.credentials.username = prov_device_id();
    c.credentials.client_id = client_id;
    c.credentials.authentication.password = active.pass;
    c.session.keepalive = 30;
    c.session.last_will.topic = t_status;
    c.session.last_will.msg = "offline";
    c.session.last_will.qos = 1;
    c.session.last_will.retain = 1;
    c.network.reconnect_timeout_ms = 5000;
    c.buffer.size = 1024;
    client = esp_mqtt_client_init(&c);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, nullptr);
    mqtt_down_since = millis();
    esp_mqtt_client_start(client);
    Serial.printf("NET: MQTT -> %s\n", uri);
}

static void apply_pending(void) {
    portENTER_CRITICAL(&mux);
    active = pending;
    reconfig = false;
    portEXIT_CRITICAL(&mux);
    mqtt_stop();
    auth_failed = false;
    WiFi.disconnect(true);
    if (!netcfg_is_valid(&active)) {
        Serial.println("NET: no config — WiFi off");
        return;
    }
    netcfg_topic(t_usage, sizeof t_usage, prov_device_id(), "usage");
    netcfg_topic(t_refresh, sizeof t_refresh, prov_device_id(), "refresh");
    netcfg_topic(t_status, sizeof t_status, prov_device_id(), "status");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);           // modem sleep is mandatory while BLE shares the radio
    WiFi.setHostname(hostname);
    WiFi.setAutoReconnect(true);
    WiFi.begin(active.ssid, active.wpass);
    Serial.printf("NET: joining \"%s\"\n", active.ssid);
}

static void net_task(void*) {
    bool mdns_up = false;
    uint32_t next_browse = 0;
    for (;;) {
        if (reconfig) {
            apply_pending();
            next_browse = 0;
        }
        if (!netcfg_is_valid(&active)) {
            state = NET_OFF;
        } else if (WiFi.status() != WL_CONNECTED) {
            state = NET_WIFI;
            if (client) mqtt_stop();
        } else {
            if (!mdns_up) mdns_up = MDNS.begin(hostname);
            if (mqtt_up) {
                state = NET_CONNECTED;
            } else {
                state = auth_failed ? NET_AUTH_FAIL : NET_DISCOVERING;
                // No client yet, or the broker has been unreachable for a while
                // (laptop left, or came back on a new IP): browse again.
                bool stale = client && (millis() - mqtt_down_since) > REDISCOVER_AFTER_MS;
                if ((!client || stale) && (int32_t)(millis() - next_browse) >= 0) {
                    mqtt_stop();
                    IPAddress ip;
                    uint16_t port;
                    if (discover(&ip, &port)) mqtt_start(ip, port);
                    else Serial.println("NET: broker not found on mDNS");
                    next_browse = millis() + BROWSE_RETRY_MS;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void net_init(void) {
    snprintf(hostname, sizeof hostname, "clawdmeter-%s", prov_device_id());
    snprintf(client_id, sizeof client_id, "clawdmeter-%s", prov_device_id());
    net_apply_config();
}

void net_apply_config(void) {
    portENTER_CRITICAL(&mux);
    pending = *prov_config();
    reconfig = true;
    portEXIT_CRITICAL(&mux);
    // The task (and the WiFi stack's RAM) only comes up once a config exists.
    if (!net_task_h && netcfg_is_valid(prov_config())) {
        xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 1, &net_task_h, 0);
    }
}

net_state_t net_get_state(void) {
    return state;
}

bool net_has_data(void) {
    return data_ready;
}

const char* net_get_data(void) {
    portENTER_CRITICAL(&mux);
    memcpy(out_buf, rx_buf, NET_BUF_SIZE);
    data_ready = false;
    portEXIT_CRITICAL(&mux);
    return out_buf;
}

#endif  // BOARD_HAS_WIFI
```

- [ ] **Step 3: Wire into `firmware/src/main.cpp`.**

(a) `#include "net.h"` after `#include "prov.h"`.

(b) In `setup()`, after `prov_init();`: `net_init();`. Change the final log line to `"Dashboard ready (%s, %dx%d), waiting for data on BLE/WiFi...\n"`.

(c) Replace the whole `if (ble_has_data()) { ... }` block at the end of `loop()` with:

```cpp
    if (ble_has_data()) handle_payload(ble_get_data(), true);
    if (net_has_data()) handle_payload(net_get_data(), false);
```

and add this function above `loop()` (body moved verbatim from the old block; only ack/nack are now BLE-only):

```cpp
// One usage payload from either transport → usage-rate, chime, UI. Only the
// BLE path acks: MQTT QoS 1 already confirms delivery.
static void handle_payload(const char* json, bool from_ble) {
    if (!parse_json(json, &usage)) {
        if (from_ble) ble_send_nack();
        return;
    }
    int g_before = usage_rate_group();
    bool session_reset = usage_rate_sample(usage.session_pct);
    int g_after = usage_rate_group();
    // 5-hour session limit refilled → chime so the user knows they can
    // use Claude again (no-op on boards without a buzzer). Gated on the
    // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
    if (session_reset && usage.chime) {
        Serial.println("session reset detected — chime");
        sound_hal_play_reset();
    }
    if (g_after != g_before) {
        Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
            g_before, g_after, usage.session_pct);
        if (splash_is_active()) splash_pick_for_current_rate();
    }
    ui_update(&usage);
    if (from_ble) ble_send_ack();
}
```

(d) Make the PROV block from Task 4 re-apply networking:

```cpp
    if (ble_has_prov()) {
        char resp[64];
        bool applied = prov_handle_write(ble_get_prov(), resp, sizeof resp);
        ble_prov_notify(resp);
        if (applied) net_apply_config();
    }
    // Tell clawdmeter-setup the whole path works once the broker answers.
    if (net_get_state() == NET_CONNECTED && prov_take_net_up_pending()) {
        ble_prov_notify("{\"net\":\"up\"}");
    }
```

(e) In `check_serial_cmd()`, make the `prov ` branch re-apply too:

```cpp
            else if (strncmp(cmd_buf, "prov ", 5) == 0) {
                bool ok = prov_apply_unchecked(cmd_buf + 5);
                if (ok) net_apply_config();
                Serial.println(ok ? "PROV_OK" : "PROV_ERR");
            }
```

(f) In `pair_tick()`, after `prov_clear();` add `net_apply_config();`.

- [ ] **Step 4: Build every env**

Run the Task 4 Step 8 loop again.
Expected: all `OK`.

- [ ] **Step 5: Check flash headroom on the 2.16.** Run `grep -E "^(RAM|Flash):" /tmp/build_waveshare_amoled_216.log`.
If Flash is above 90 %, add these lines to `[env:waveshare_amoled_216]` under `board_build.arduino.memory_type = qio_opi` (they match the other S3 envs), rebuild, and confirm Flash drops:

```ini
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.partitions = default_16MB.csv
```

If the 2.16 boot-loops after flashing with 16 MB (meaning the chip is smaller), revert these three lines and tell the user.

- [ ] **Step 6: Hardware check — broker by hand, no daemon yet**

1. On the Mac: `bash daemon/broker/broker-init.sh` then `/opt/homebrew/sbin/mosquitto -c ~/.config/claude-usage-monitor/broker/mosquitto.conf`.
2. In a second terminal: `. ~/.config/claude-usage-monitor/broker/broker.env; dns-sd -R "Clawdmeter test" _clawdmeter._tcp local 18830 id=$BROKER_ID`.
3. In a third: `mosquitto_passwd -b ~/.config/claude-usage-monitor/broker/passwd <device_id> devpass && kill -HUP $(pgrep -f "mosquitto -c")` (device id from the Task 4 boot log).
4. Flash, and in the serial monitor: `prov {"ssid":"<your wifi>","wpass":"<pass>","broker_id":"<BROKER_ID>","pass":"devpass"}`.
   Expected log: `NET: joining`, then `NET: MQTT -> mqtt://<mac-ip>:18830`, then `NET: MQTT connected`.
5. `mosquitto_pub -p 18830 -u daemon -P $DAEMON_PASS -r -q 1 -t clawdmeter/<device_id>/usage -m '{"s":42,"sr":100,"w":10,"wr":5000,"st":"allowed","ok":true}'`.
   Expected: the usage screen shows 42 % (check with `./screenshot.sh /tmp/u.png` + Read).
6. Ctrl+C the mosquitto, wait 30 s, then restart it. Expected: `NET: MQTT connected` again within about 25 s, with no touch.

Clean up: Ctrl+C the `dns-sd` and mosquitto.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/net.h firmware/src/net.cpp firmware/src/main.cpp firmware/platformio.ini
git commit -m "firmware: WiFi + mDNS + MQTT transport feeding the existing payload path"
```

---

### Task 6: UI — link status from both transports + setup-code overlay

**Files:**
- Modify: `firmware/src/ui.h`, `firmware/src/ui.cpp`, `firmware/src/main.cpp`

**Interfaces:**
- Consumes: `net_state_t` (Task 5), `prov_code_visible()` / `prov_code()` (Task 4).
- Produces: `void ui_update_link_status(bool ble_connected, net_state_t net);` (replaces `ui_update_ble_status`) and `void ui_show_setup_code(const char* code);` (`nullptr` hides it).

- [ ] **Step 1: `ui.h`** — add `#include "net.h"` under `#include "ble.h"`, and replace the `ui_update_ble_status` declaration with:

```cpp
void ui_update_link_status(bool ble_connected, net_state_t net);
void ui_show_setup_code(const char* code);   // nullptr hides the overlay
```

- [ ] **Step 2: `ui.cpp` state.**
  - Next to `static bool s_ble_connected = false;`, add `static net_state_t s_net = NET_OFF;`.
  - Change that line's comment to `// BLE or MQTT link up`.

- [ ] **Step 3: `ui.cpp` view choice.** In `update_view_state()`, replace `if (!s_ble_connected) {` with:

```cpp
    if (!s_ble_connected && s_net == NET_OFF) {
        v = 0;  // pairing hint — only when there's no WiFi path either
```

Keep the existing `v = 0;` body. Update the function's leading comment: "pairing hint (no BLE link and no WiFi config)". With WiFi configured but the host away, the idle "Zzz" screen shows instead of "To pair…".

- [ ] **Step 4: `ui.cpp` status line.** In `ui_tick_anim()`, replace

```cpp
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
```

with

```cpp
    if (!s_ble_connected) {
        switch (s_net) {
        case NET_WIFI:        text = "Joining WiFi"; break;
        case NET_DISCOVERING: text = "Finding host"; break;
        case NET_AUTH_FAIL:   text = "Re-run setup"; break;   // broker rejected our login
        default:              text = "Waiting";      break;   // advertising / no host yet
        }
```

- [ ] **Step 5: `ui.cpp` link status.** Replace the whole `ui_update_ble_status` function with:

```cpp
void ui_update_link_status(bool ble_connected, net_state_t net) {
    bool was_connected = s_ble_connected;
    s_ble_connected = ble_connected || net == NET_CONNECTED;
    s_net = net;

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}
```

- [ ] **Step 6: `ui.cpp` code overlay.** Add the statics `static lv_obj_t* code_overlay = nullptr;` and `static lv_obj_t* code_label = nullptr;` beside the other `lv_obj_t*` statics. Put this builder after `build_idle_group`:

```cpp
// Setup-code overlay — full-screen on the top layer so it covers the splash
// and the usage view alike while clawdmeter-setup waits for the code.
static void build_code_overlay(void) {
    code_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(code_overlay, L.scr_w, L.scr_h);
    lv_obj_set_pos(code_overlay, 0, 0);
    lv_obj_set_style_bg_color(code_overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(code_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(code_overlay, 0, 0);
    lv_obj_set_style_radius(code_overlay, 0, 0);
    lv_obj_set_style_pad_all(code_overlay, 0, 0);
    lv_obj_clear_flag(code_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(code_overlay);
    lv_label_set_text(title, "Setup code");
    lv_obj_set_style_text_font(title, L.bt_status_font, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, L.content_y + L.pair_y1);

    code_label = lv_label_create(code_overlay);
    lv_label_set_text(code_label, "");
    lv_obj_set_style_text_font(code_label, L.pct_font, 0);
    lv_obj_set_style_text_color(code_label, COL_TEXT, 0);
    lv_obj_align(code_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* hint = lv_label_create(code_overlay);
    lv_label_set_text(hint, "Type it into clawdmeter-setup");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, L.scr_w - 40);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, L.bt_device_font, 0);
    lv_obj_set_style_text_color(hint, COL_DIM, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -L.pair_y1);

    lv_obj_add_flag(code_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_setup_code(const char* code) {
    if (!code_overlay) return;
    if (!code) {
        lv_obj_add_flag(code_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char spaced[8];
    snprintf(spaced, sizeof spaced, "%.3s %.3s", code, code + 3);   // "123 456" reads easier
    lv_label_set_text(code_label, spaced);
    lv_obj_clear_flag(code_overlay, LV_OBJ_FLAG_HIDDEN);
}
```

Call `build_code_overlay();` as the last statement of `ui_init()`.

- [ ] **Step 7: `main.cpp` wiring.**

(a) In `setup()`, replace `ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());` with `ui_update_link_status(ble_get_state() == BLE_STATE_CONNECTED, net_get_state());`

(b) Replace `static ble_state_t last_ble_state = BLE_STATE_INIT;` with:

```cpp
static ble_state_t last_ble_state = BLE_STATE_INIT;
static net_state_t last_net_state = NET_OFF;
```

(c) Replace the block

```cpp
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }
```

with

```cpp
    ble_state_t bs = ble_get_state();
    net_state_t ns = net_get_state();
    if (bs != last_ble_state || ns != last_net_state) {
        last_ble_state = bs;
        last_net_state = ns;
        ui_update_link_status(bs == BLE_STATE_CONNECTED, ns);
    }

    // Setup code: show while a clawdmeter-setup session is open; wake the
    // panel so the digits are readable even if the device was dozing.
    static bool code_shown = false;
    bool code_vis = prov_code_visible();
    if (code_vis != code_shown) {
        code_shown = code_vis;
        ui_show_setup_code(code_vis ? prov_code() : nullptr);
        if (code_vis) idle_note_activity();
    }
```

- [ ] **Step 8: Build every env** (Task 4 Step 8 loop). Expected: all `OK`.

- [ ] **Step 9: Visual QA on hardware** (CLAUDE.md "QA your own UI changes")
  1. Flash an S3 board.
  2. In the serial monitor, type `provbegin`, then within 120 s run `./screenshot.sh /tmp/code.png`. Read the PNG: "Setup code", then "NNN NNN" large and centred, then the hint below it. Nothing may be clipped by the rounded corners.
  3. Wait 120 s, then screenshot again. The overlay must be gone.
  4. With a config pushed but no broker running: screenshot. The status line must read "Finding host…" over the idle screen, not "To pair".
  5. If you have the LCD-1.54 (240×240), repeat step 2 on it; that is the compact layout.

- [ ] **Step 10: Commit**

```bash
git add firmware/src/ui.h firmware/src/ui.cpp firmware/src/main.cpp
git commit -m "firmware: link status from BLE or MQTT, setup-code overlay"
```

---

### Task 7: `clawdmeter-setup` CLI

**Files:**
- Create: `daemon/clawdmeter_setup.py`, `daemon/clawdmeter-setup`
- Test: `daemon/tests/test_setup_cli.py`

**Interfaces:**
- Consumes:
  - `discover_target()`, `CONFIG_FILE`, `CONNECT_TIMEOUT` from `daemon/claude_usage_daemon.py`.
  - `BROKER_DIR`, `read_broker_env()`, `find_tool()` from Task 2.
  - The PROV protocol from Task 4: `begin` → `{"id"}` or `{"err":"nowifi"}`; `config` → `{"ok":true}` or `{"err":"code"|"locked"|"expired"|"json"}`; later `{"net":"up"}`.
- Produces: `parse_nmcli_ssid(out)`, `parse_networksetup_ssid(out)`, `build_config_message(code, ssid, wpass, broker_id, password) -> bytes`, `add_device(broker_dir, device_id, password, passwd_tool) -> None`, `upsert_config_key(path, key, value)`, `broker_reachable(port) -> bool`, `main() -> int`.

- [ ] **Step 1: Write failing tests** `daemon/tests/test_setup_cli.py`

```python
#!/usr/bin/env python3
"""Tests for daemon/clawdmeter_setup.py (pure parts; BLE is covered by the
hardware check in the plan).

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_setup_cli.py -x -q
"""
import json
import socket
import subprocess

import pytest

import daemon.clawdmeter_setup as s


def test_parse_nmcli_ssid_picks_active_and_unescapes_colons():
    out = "no:Neighbour\nyes:Home\\:5G\nno:Other\n"
    assert s.parse_nmcli_ssid(out) == "Home:5G"
    assert s.parse_nmcli_ssid("no:x\n") is None


def test_parse_networksetup_ssid():
    assert s.parse_networksetup_ssid("Current Wi-Fi Network: Home 5G\n") == "Home 5G"
    assert s.parse_networksetup_ssid("You are not associated with an AirPort network.\n") is None


def test_build_config_message_round_trips_hostile_chars():
    raw = s.build_config_message("123456", 'Ho"me: 5G', "p@ss:w\"rd", "00aa11bb22cc33dd", "x" * 24)
    msg = json.loads(raw)
    assert msg == {"op": "config", "code": "123456", "ssid": 'Ho"me: 5G', "wpass": "p@ss:w\"rd",
                   "broker_id": "00aa11bb22cc33dd", "pass": "x" * 24}


def test_build_config_message_max_lengths_fit_one_ble_write():
    raw = s.build_config_message("123456", "s" * 32, "p" * 63, "00aa11bb22cc33dd", "x" * 24)
    assert len(raw) <= 500


def test_build_config_message_rejects_overlong_wifi_fields():
    with pytest.raises(ValueError):
        s.build_config_message("123456", "s" * 33, "p", "00aa11bb22cc33dd", "x")
    with pytest.raises(ValueError):
        s.build_config_message("123456", "s", "p" * 64 + "q", "00aa11bb22cc33dd", "x")


def test_upsert_config_key_replaces_existing(tmp_path):
    cfg = tmp_path / "config"
    cfg.write_text("chime = on\ntransport = ble\n")
    s.upsert_config_key(cfg, "transport", "mqtt")
    lines = cfg.read_text().splitlines()
    assert "chime = on" in lines and "transport = mqtt" in lines and "transport = ble" not in lines


def test_add_device_writes_passwd_dedupes_devices_and_hups_broker(tmp_path):
    log = tmp_path / "calls"
    fake = tmp_path / "mosquitto_passwd"
    fake.write_text(f'#!/bin/sh\necho "$@" >> "{log}"\n')
    fake.chmod(0o755)
    (tmp_path / "devices").write_text("a1b2c3\n")
    broker = subprocess.Popen(["sleep", "30"])
    (tmp_path / "broker.pid").write_text(str(broker.pid))

    s.add_device(tmp_path, "a1b2c3", "pw1", str(fake))
    s.add_device(tmp_path, "ffffff", "pw2", str(fake))

    assert log.read_text().splitlines() == [f"-b {tmp_path}/passwd a1b2c3 pw1", f"-b {tmp_path}/passwd ffffff pw2"]
    assert (tmp_path / "devices").read_text().splitlines() == ["a1b2c3", "ffffff"]
    assert broker.wait(timeout=5) != 0        # SIGHUP ended `sleep`


def test_broker_reachable():
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    assert s.broker_reachable(port)
    srv.close()
    assert not s.broker_reachable(port)
```

- [ ] **Step 2: Run, confirm failure**

Run: `daemon/.venv/bin/python -m pytest daemon/tests/test_setup_cli.py -x -q`
Expected: `ModuleNotFoundError: No module named 'daemon.clawdmeter_setup'`

- [ ] **Step 3: Implement** `daemon/clawdmeter_setup.py`

```python
#!/usr/bin/env python3
"""clawdmeter-setup: put a Clawdmeter on WiFi + the daemon's MQTT broker.

Run it on the machine that runs the daemon, with the device already paired
over Bluetooth and the daemon running (it hosts the broker). The device shows
a 6-digit code; you type it here; we push WiFi + broker details over the
encrypted BLE link and wait until the device reports it reached the broker.
"""

import argparse
import asyncio
import getpass
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
from pathlib import Path

from bleak import BleakClient
from bleak.exc import BleakError

try:  # run as a script (sys.path[0] == daemon/) or imported as daemon.* by tests
    import claude_usage_daemon as daemon_core
    import clawdmeter_mqtt as mqtt_transport
except ImportError:
    from daemon import claude_usage_daemon as daemon_core
    from daemon import clawdmeter_mqtt as mqtt_transport

PROV_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"
REPLY_TIMEOUT = 5.0
NET_UP_TIMEOUT = 45.0
MAX_CODE_TRIES = 5


def parse_nmcli_ssid(out: str) -> str | None:
    for line in out.splitlines():
        if line.startswith("yes:"):
            return line[4:].replace("\\:", ":") or None
    return None


def parse_networksetup_ssid(out: str) -> str | None:
    m = re.match(r"Current Wi-Fi Network: (.+)$", out.strip())
    return m.group(1) if m else None


def detect_ssid() -> str | None:
    try:
        if sys.platform.startswith("linux"):
            out = subprocess.run(["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"],
                                 capture_output=True, text=True, timeout=5).stdout
            return parse_nmcli_ssid(out)
        if sys.platform == "darwin":
            out = subprocess.run(["networksetup", "-getairportnetwork", "en0"],
                                 capture_output=True, text=True, timeout=5).stdout
            return parse_networksetup_ssid(out)
    except (OSError, subprocess.TimeoutExpired):
        pass
    return None


def build_config_message(code: str, ssid: str, wpass: str, broker_id: str, password: str) -> bytes:
    # Same limits the firmware enforces (netcfg.h) — fail here with a clear
    # message instead of a bare {"err":"json"} from the device.
    if not 1 <= len(ssid.encode()) <= 32:
        raise ValueError("WiFi network name must be 1-32 bytes")
    if len(wpass.encode()) > 64:
        raise ValueError("WiFi password must be at most 64 characters")
    msg = json.dumps({"op": "config", "code": code, "ssid": ssid, "wpass": wpass,
                      "broker_id": broker_id, "pass": password}, separators=(",", ":")).encode()
    if len(msg) > 500:
        raise ValueError("config too long for one BLE write")
    return msg


def add_device(broker_dir: Path, device_id: str, password: str, passwd_tool: str) -> None:
    subprocess.run([passwd_tool, "-b", str(broker_dir / "passwd"), device_id, password], check=True)
    devices = broker_dir / "devices"
    known = devices.read_text().split() if devices.exists() else []
    if device_id not in known:
        with devices.open("a") as f:
            f.write(device_id + "\n")
    pid_file = broker_dir / "broker.pid"
    if pid_file.exists():
        try:
            os.kill(int(pid_file.read_text().strip()), signal.SIGHUP)  # reload passwd
        except (ValueError, ProcessLookupError, PermissionError):
            pass


def upsert_config_key(path: Path, key: str, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = path.read_text().splitlines() if path.exists() else []
    kept = [ln for ln in lines if not re.match(rf"\s*{re.escape(key)}\s*=", ln)]
    kept.append(f"{key} = {value}")
    path.write_text("\n".join(kept) + "\n")


def broker_reachable(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def restart_daemon() -> bool:
    if sys.platform == "darwin":
        cmd = ["launchctl", "kickstart", "-k", f"gui/{os.getuid()}/com.user.claude-usage-daemon"]
    else:
        cmd = ["systemctl", "--user", "restart", "claude-usage-daemon"]
    try:
        return subprocess.run(cmd, capture_output=True, timeout=15).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


class ProvLink:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.replies: asyncio.Queue = asyncio.Queue()

    def _on_notify(self, _char, data: bytearray) -> None:
        try:
            self.replies.put_nowait(json.loads(bytes(data).decode()))
        except (ValueError, UnicodeDecodeError):
            self.replies.put_nowait({"err": "garbled"})

    async def start(self) -> None:
        await self.client.start_notify(PROV_CHAR_UUID, self._on_notify)

    async def send(self, msg) -> None:
        data = msg if isinstance(msg, bytes) else json.dumps(msg, separators=(",", ":")).encode()
        await self.client.write_gatt_char(PROV_CHAR_UUID, data, response=True)

    async def recv(self, timeout: float) -> dict:
        return await asyncio.wait_for(self.replies.get(), timeout)


def prompt_default(label: str, default: str | None) -> str:
    while True:
        ans = input(f"{label}" + (f" [{default}]" if default else "") + ": ").strip()
        if ans or default:
            return ans or default


async def provision(args) -> int:
    env = mqtt_transport.read_broker_env()
    if env is None:
        print("No broker set up yet. Re-run the installer (it runs daemon/broker/broker-init.sh).")
        return 1
    if not broker_reachable(int(env["BROKER_PORT"])):
        print("The broker isn't running. Start the daemon first — it hosts the broker.")
        return 1
    passwd_tool = mqtt_transport.find_tool("mosquitto_passwd")
    if passwd_tool is None:
        print("mosquitto_passwd not found — install mosquitto.")
        return 1

    target = await daemon_core.discover_target()
    if target is None:
        print("Clawdmeter not found. Pair it in Bluetooth settings first and make sure it's on.")
        return 1

    client = BleakClient(target)
    try:
        await asyncio.wait_for(client.connect(), timeout=daemon_core.CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        print(f"Could not connect over Bluetooth: {e}")
        return 1
    try:
        link = ProvLink(client)
        await link.start()
        await link.send({"op": "begin"})
        reply = await link.recv(REPLY_TIMEOUT)
        if reply.get("err") == "nowifi":
            print("This board has no WiFi support — it keeps using Bluetooth.")
            return 1
        device_id = reply.get("id", "")
        if not re.fullmatch(r"[0-9a-f]{6}", device_id):
            print(f"Unexpected reply from device: {reply}")
            return 1
        print(f"Device {device_id} is now showing a 6-digit setup code (valid 2 minutes).")

        ssid = args.ssid or prompt_default("WiFi network", detect_ssid())
        wpass = getpass.getpass(f"Password for {ssid} (blank if open): ")
        password = secrets.token_urlsafe(18)   # 24 chars
        add_device(mqtt_transport.BROKER_DIR, device_id, password, passwd_tool)

        for _ in range(MAX_CODE_TRIES):
            code = input("Code shown on the device: ").strip().replace(" ", "")
            if not re.fullmatch(r"\d{6}", code):
                print("Enter the 6 digits shown on the screen.")
                continue
            try:
                await link.send(build_config_message(code, ssid, wpass, env["BROKER_ID"], password))
            except ValueError as e:
                print(e)
                return 1
            reply = await link.recv(REPLY_TIMEOUT)
            if reply.get("ok"):
                break
            err = reply.get("err")
            if err == "code":
                print("Wrong code, try again.")
                continue
            if err in ("locked", "expired"):
                print("The code expired or had too many wrong tries. Run clawdmeter-setup again.")
                return 1
            print(f"Device rejected the config: {reply}")
            return 1
        else:
            return 1

        print("Saved. Waiting for the device to join WiFi and reach the broker...")
        try:
            while (await link.recv(NET_UP_TIMEOUT)).get("net") != "up":
                pass
        except asyncio.TimeoutError:
            print(f"The device did not reach the broker within {int(NET_UP_TIMEOUT)} s.\n"
                  f"  - Check the password for \"{ssid}\" and run clawdmeter-setup again.\n"
                  "  - macOS: allow incoming connections for mosquitto if the firewall asks.\n"
                  "The device keeps working over Bluetooth meanwhile.")
            return 1
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    upsert_config_key(daemon_core.CONFIG_FILE, "transport", "mqtt")
    print("Connected over WiFi. Switching the daemon to MQTT...")
    print("Done." if restart_daemon() else "Restart the daemon to finish switching to MQTT.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Set up a Clawdmeter for WiFi + MQTT.")
    ap.add_argument("--ssid", help="WiFi network name (default: the one this machine is on)")
    try:
        return asyncio.run(provision(ap.parse_args()))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
```

And `daemon/clawdmeter-setup` (then `chmod +x daemon/clawdmeter-setup`):

```bash
#!/bin/bash
# Wrapper so users can run ./daemon/clawdmeter-setup without knowing the venv.
HERE="$(cd "$(dirname "$(readlink -f "$0" 2>/dev/null || echo "$0")")" && pwd)"
PY="$HERE/.venv/bin/python"
[ -x "$PY" ] || { echo "Run the installer first (it creates $HERE/.venv)"; exit 1; }
exec "$PY" "$HERE/clawdmeter_setup.py" "$@"
```

- [ ] **Step 4: Run tests, confirm pass**

Run: `daemon/.venv/bin/python -m pytest daemon/tests/test_setup_cli.py -x -q`
Expected: `8 passed`

- [ ] **Step 5: Commit**

```bash
git add daemon/clawdmeter_setup.py daemon/clawdmeter-setup daemon/tests/test_setup_cli.py
git commit -m "daemon: clawdmeter-setup — code-verified WiFi/MQTT provisioning over BLE"
```

---

### Task 8: Python daemon — MQTT mode

**Files:**
- Modify: `daemon/claude_usage_daemon.py`
- Test: `daemon/tests/test_daemon_mqtt.py`

**Interfaces:**
- Consumes: Task 2 `BrokerSupervisor`, `Announcer`, `Publisher`, `read_broker_env`, `read_devices`.
- Produces: `read_transport_setting() -> str` (`"ble"` | `"mqtt"`), `async run_mqtt(env, stop_event)`, `async run_ble(stop_event)` (the old `main()` loop body, unchanged).

- [ ] **Step 1: Write failing tests** `daemon/tests/test_daemon_mqtt.py`

```python
#!/usr/bin/env python3
"""Tests for the daemon's MQTT mode (transport switch + run_mqtt loop).

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_daemon_mqtt.py -x -q
"""
import asyncio

import daemon.claude_usage_daemon as mod


def test_transport_defaults_to_ble(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    assert mod.read_transport_setting() == "ble"


def test_transport_reads_mqtt(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("chime = on\ntransport = MQTT  # set by clawdmeter-setup\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert mod.read_transport_setting() == "mqtt"


def test_transport_ignores_unknown_values(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("transport = carrier-pigeon\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert mod.read_transport_setting() == "ble"


class FakePublisher:
    instances = []

    def __init__(self, env, loop, on_refresh, log=print):
        self.on_refresh = on_refresh
        self.published = []
        self.stopped = False
        FakePublisher.instances.append(self)

    def start(self):
        pass

    def stop(self):
        self.stopped = True

    def publish_usage(self, device_ids, payload):
        self.published.append((list(device_ids), payload))
        return True


def _run_until(stop_after_publishes, payloads, monkeypatch, devices=("a1b2c3",)):
    FakePublisher.instances.clear()
    monkeypatch.setattr(mod.mqtt_transport, "Publisher", FakePublisher)
    monkeypatch.setattr(mod.mqtt_transport, "read_devices", lambda *_a: list(devices))
    queue = list(payloads)

    async def fake_poll_active(*_a):
        return queue.pop(0) if queue else (None, False)

    monkeypatch.setattr(mod, "poll_active", fake_poll_active)
    monkeypatch.setattr(mod, "TICK", 0.05)

    async def scenario():
        stop = asyncio.Event()
        task = asyncio.create_task(mod.run_mqtt({"BROKER_PORT": "1", "DAEMON_PASS": "x"}, stop))
        for _ in range(100):
            await asyncio.sleep(0.02)
            pubs = FakePublisher.instances[0].published if FakePublisher.instances else []
            if len(pubs) >= stop_after_publishes:
                break
            if FakePublisher.instances and len(pubs) == 1 and stop_after_publishes == 2:
                FakePublisher.instances[0].on_refresh("a1b2c3")   # device asks for fresh data
        stop.set()
        await asyncio.wait_for(task, 2)

    asyncio.run(scenario())
    return FakePublisher.instances[0]


def test_run_mqtt_publishes_payload_to_every_device(monkeypatch):
    pub = _run_until(1, [({"s": 5, "ok": True}, False)], monkeypatch, devices=("a1b2c3", "ffffff"))
    assert pub.published[0] == (["a1b2c3", "ffffff"], {"s": 5, "ok": True})
    assert pub.stopped


def test_run_mqtt_signals_no_data_when_tokens_dead(monkeypatch):
    pub = _run_until(1, [(None, True)], monkeypatch)
    assert pub.published[0] == (["a1b2c3"], {"ok": False})


def test_run_mqtt_refresh_request_triggers_immediate_poll(monkeypatch):
    pub = _run_until(2, [({"s": 1, "ok": True}, False), ({"s": 2, "ok": True}, False)], monkeypatch)
    assert [p for _d, p in pub.published] == [{"s": 1, "ok": True}, {"s": 2, "ok": True}]
```

- [ ] **Step 2: Run, confirm failure**

Run: `daemon/.venv/bin/python -m pytest daemon/tests/test_daemon_mqtt.py -x -q`
Expected: `AttributeError: module 'daemon.claude_usage_daemon' has no attribute 'read_transport_setting'`

- [ ] **Step 3: Implement in `daemon/claude_usage_daemon.py`.**

(a) After the `from bleak.exc import BleakError` import:

```python
try:  # run as a script (sys.path[0] == daemon/) or imported as daemon.* by tests
    import clawdmeter_mqtt as mqtt_transport
except ImportError:
    from daemon import clawdmeter_mqtt as mqtt_transport
```

Also update the module docstring's last paragraph to mention: "With `transport = mqtt` in the config (set by clawdmeter-setup), usage is published to the local MQTT broker instead (see clawdmeter_mqtt.py)."

(b) After `read_clock_setting()`, add (same shape as `read_chime_setting`):

```python
def read_transport_setting() -> str:
    """Read the `transport` option from the config file. One of: ble|mqtt.

    Defaults to "ble". clawdmeter-setup flips it to "mqtt" once a device has
    reached the broker over WiFi. Read once at startup — the setup CLI
    restarts the daemon after changing it.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "transport":
                    val = val.strip().lower()
                    if val in ("ble", "mqtt"):
                        return val
    except OSError:
        pass
    return "ble"
```

(c) Add before `async def main()`:

```python
async def run_mqtt(env: dict, stop_event: asyncio.Event) -> None:
    """Poll Anthropic and publish to every set-up device over the local broker.

    The broker keeps the last payload retained, so a device that walks back
    into range gets current numbers the moment it reconnects; its refresh
    request then triggers a fresh poll right away.
    """
    refresh = asyncio.Event()

    def _on_refresh(device_id: str) -> None:
        log(f"Refresh requested by {device_id}")
        refresh.set()

    pub = mqtt_transport.Publisher(env, asyncio.get_running_loop(), _on_refresh, log)
    pub.start()
    last_poll = 0.0
    try:
        while not stop_event.is_set():
            if refresh.is_set() or time.time() - last_poll >= POLL_INTERVAL:
                refresh.clear()
                devices = mqtt_transport.read_devices()
                if not devices:
                    log("No WiFi devices set up yet; run daemon/clawdmeter-setup")
                    last_poll = time.time()
                else:
                    # Same free-ride token policy as the BLE path (see connect_and_run).
                    payload, dead = await poll_active()
                    if payload is not None:
                        log(f"Publishing: {json.dumps(payload, separators=(',', ':'))}")
                        if pub.publish_usage(devices, payload):
                            last_poll = time.time()
                    elif dead:
                        log("No usable token; signalling no-data to devices")
                        if pub.publish_usage(devices, {"ok": False}):
                            last_poll = time.time()
                    else:
                        log("No usable config dir this cycle")
            try:
                await asyncio.wait_for(refresh.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        pub.stop()
```

(d) Split `main()`:
- Move everything from `backoff = 1` to the end of its `while` loop, unchanged, into a new `async def run_ble(stop_event: asyncio.Event) -> None:` placed directly above `main()`.
- Then make `main()` end (after the signal-handler setup and the two `log(...)` lines) with:

```python
    # The broker + mDNS announcement run whenever broker-init.sh has been run,
    # even in BLE mode — clawdmeter-setup needs the broker up to verify a
    # freshly provisioned device before switching the transport to MQTT.
    env = mqtt_transport.read_broker_env()
    side_tasks = []
    if env is not None:
        side_tasks.append(asyncio.create_task(mqtt_transport.BrokerSupervisor(log=log).run(stop_event)))
        side_tasks.append(asyncio.create_task(
            mqtt_transport.Announcer(env["BROKER_ID"], int(env["BROKER_PORT"]), log).run(stop_event)))
    try:
        if env is not None and read_transport_setting() == "mqtt":
            log("Transport: MQTT (WiFi)")
            await run_mqtt(env, stop_event)
        else:
            if read_transport_setting() == "mqtt":
                log("transport = mqtt but no broker is set up; falling back to BLE")
            await run_ble(stop_event)
    finally:
        stop_event.set()
        await asyncio.gather(*side_tasks, return_exceptions=True)
```

Change the startup banner `log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")` to `log("=== Claude Usage Tracker Daemon (macOS) ===")`.

- [ ] **Step 4: Run all daemon Python tests**

Run: `daemon/.venv/bin/python -m pytest daemon/tests/test_daemon_mqtt.py daemon/tests/test_macos_multidir.py daemon/tests/test_freeride.py daemon/tests/test_mqtt_transport.py -x -q`
Expected: all pass (the `test_windows_*` files are Windows-daemon tests and are unaffected).

- [ ] **Step 5: Commit**

```bash
git add daemon/claude_usage_daemon.py daemon/tests/test_daemon_mqtt.py
git commit -m "daemon(macOS): host the broker, announce it, publish usage over MQTT"
```

---

### Task 9: Linux bash daemon — MQTT mode

**Files:**
- Modify: `daemon/claude-usage-daemon.sh`
- Test: `daemon/tests/test_bash_mqtt.sh`

**Interfaces:**
- Consumes: Task 1 broker dir + `find-tool.sh`.
- Produces (top-level bash functions, extractable by the awk pattern from `test_bash_token.sh`):
  - `read_transport_setting`
  - `broker_ensure`
  - `announce_ensure`
  - `refresh_sub_ensure`
  - `side_services_tick`
  - `mqtt_publish PAYLOAD`
  - `send_payload PAYLOAD`

- [ ] **Step 1: Write the failing test** `daemon/tests/test_bash_mqtt.sh`

```bash
#!/bin/bash
# Tests for the bash daemon's MQTT helpers: transport setting + publish to every
# set-up device. The publish half needs mosquitto and is skipped without it.
#
# Run: bash daemon/tests/test_bash_mqtt.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DAEMON="$HERE/../claude-usage-daemon.sh"
. "$HERE/../broker/find-tool.sh"
extract() { awk "/^$1\\(\\) \\{/{f=1} f{print} f&&/^\\}/{exit}" "$DAEMON"; }
eval "$(extract read_transport_setting)"
eval "$(extract mqtt_publish)"
log() { :; }

TMP="$(mktemp -d)"; BPID=""
trap '[ -n "$BPID" ] && kill "$BPID" 2>/dev/null; rm -rf "$TMP"' EXIT
fail=0
check() { if eval "$1"; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }

CONFIG_FILE="$TMP/config"
check '[ "$(read_transport_setting)" = ble ]' "transport defaults to ble"
printf 'chime = on\ntransport = MQTT # set by setup\n' > "$CONFIG_FILE"
check '[ "$(read_transport_setting)" = mqtt ]' "transport = mqtt is read"
printf 'transport = pigeon\n' > "$CONFIG_FILE"
check '[ "$(read_transport_setting)" = ble ]' "unknown transport falls back to ble"

MOSQ="$(clawd_find_tool mosquitto)" || { echo "SKIP: publish test (mosquitto not installed)"; exit $fail; }
BROKER_DIR="$TMP/b"
CLAWD_BROKER_PORT=18934 bash "$HERE/../broker/broker-init.sh" "$BROKER_DIR" >/dev/null
. "$BROKER_DIR/broker.env"
"$(clawd_find_tool mosquitto_passwd)" -b "$BROKER_DIR/passwd" a1b2c3 devpass
printf 'a1b2c3\nnot-valid\n' > "$BROKER_DIR/devices"
"$MOSQ" -c "$BROKER_DIR/mosquitto.conf" >/dev/null 2>&1 & BPID=$!
sleep 1

mqtt_publish '{"s":7,"ok":true}'; rc=$?
check '[ $rc -eq 0 ]' "mqtt_publish succeeds"
got="$(mosquitto_sub -h 127.0.0.1 -p 18934 -u a1b2c3 -P devpass -t clawdmeter/a1b2c3/usage -C 1 -W 3)"
check '[ "$got" = "{\"s\":7,\"ok\":true}" ]' "device receives the retained payload"
exit $fail
```

- [ ] **Step 2: Run, confirm failure**

Run: `bash daemon/tests/test_bash_mqtt.sh`
Expected: `FAIL: transport defaults to ble` (the function doesn't exist yet), exit 1.

- [ ] **Step 3: Implement in `daemon/claude-usage-daemon.sh`.**

(a) Header comment: add a line `# Optional WiFi/MQTT transport: see daemon/broker/ and clawdmeter-setup.`. Dependencies line: append `; for MQTT: mosquitto, mosquitto-clients, avahi-utils`.

(b) After `NOTIFY_PID=""` add:

```bash
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
BROKER_DIR="$HOME/.config/claude-usage-monitor/broker"
BROKER_PID=""
AVAHI_PID=""
MQTT_SUB_PID=""
TRANSPORT="ble"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/broker/find-tool.sh"
```

(c) After `read_chime_setting()` add:

```bash
# Read the `transport` option: ble (default) | mqtt. clawdmeter-setup sets it
# to mqtt once a device has reached the broker over WiFi, then restarts us.
read_transport_setting() {
    local val=""
    if [ -f "$CONFIG_FILE" ]; then
        val=$(sed -e 's/#.*//' "$CONFIG_FILE" \
            | awk -F= 'tolower($1) ~ /^[[:space:]]*transport[[:space:]]*$/ {gsub(/[[:space:]]/, "", $2); print tolower($2)}' \
            | tail -1)
    fi
    case "$val" in
        mqtt) echo mqtt ;;
        *)    echo ble ;;
    esac
}
```

(d) After `write_gatt()` add:

```bash
# --- MQTT transport ---------------------------------------------------------
# The broker + mDNS announcement run whenever broker-init.sh has been run (even
# in BLE mode), because clawdmeter-setup needs the broker up to verify a device.

broker_ensure() {
    if [ -n "$BROKER_PID" ] && kill -0 "$BROKER_PID" 2>/dev/null; then return 0; fi
    local exe
    exe=$(clawd_find_tool mosquitto) || { log "mosquitto not installed; WiFi devices can't connect"; return 1; }
    "$exe" -c "$BROKER_DIR/mosquitto.conf" &
    BROKER_PID=$!
    echo "$BROKER_PID" > "$BROKER_DIR/broker.pid"
    log "Broker started (pid=$BROKER_PID)"
}

announce_ensure() {
    if [ -n "$AVAHI_PID" ] && kill -0 "$AVAHI_PID" 2>/dev/null; then return 0; fi
    command -v avahi-publish-service >/dev/null || { log "avahi-publish-service missing; devices can't find the broker"; return 1; }
    avahi-publish-service "Clawdmeter broker on $(hostname -s)" _clawdmeter._tcp "$BROKER_PORT" "id=$BROKER_ID" >/dev/null 2>&1 &
    AVAHI_PID=$!
    log "mDNS: announcing _clawdmeter._tcp on port $BROKER_PORT"
}

# Device refresh requests → the same flag file the BLE notify path uses.
refresh_sub_ensure() {
    if [ -n "$MQTT_SUB_PID" ] && kill -0 "$MQTT_SUB_PID" 2>/dev/null; then return 0; fi
    setsid bash -c "mosquitto_sub -h 127.0.0.1 -p '$BROKER_PORT' -u daemon -P '$DAEMON_PASS' -t 'clawdmeter/+/refresh' 2>/dev/null | while read -r _; do touch '$REFRESH_FLAG'; done" &
    MQTT_SUB_PID=$!
}

side_services_tick() {
    [ -n "${BROKER_ID:-}" ] || return 0
    broker_ensure
    announce_ensure
}

# Publish one payload, retained, to every device clawdmeter-setup has added.
mqtt_publish() {
    local payload="$1" dev rc=0
    while read -r dev; do
        [[ "$dev" =~ ^[0-9a-f]{6}$ ]] || continue
        mosquitto_pub -h 127.0.0.1 -p "$BROKER_PORT" -u daemon -P "$DAEMON_PASS" \
            -q 1 -r -t "clawdmeter/$dev/usage" -m "$payload" || rc=1
    done < "$BROKER_DIR/devices"
    return $rc
}

send_payload() {
    if [ "$TRANSPORT" = mqtt ]; then
        mqtt_publish "$1"
    else
        write_gatt "$RX_CHAR_PATH" "$1"
    fi
}

stop_mqtt_side() {
    [ -n "$MQTT_SUB_PID" ] && kill -TERM -"$MQTT_SUB_PID" 2>/dev/null
    [ -n "$AVAHI_PID" ] && kill "$AVAHI_PID" 2>/dev/null
    [ -n "$BROKER_PID" ] && kill "$BROKER_PID" 2>/dev/null
    rm -f "$BROKER_DIR/broker.pid"
}
```

(e) In `poll()`, replace `write_gatt "$RX_CHAR_PATH" "${cycle_payload[$best_dir]}" || { log "Write failed"; return 1; }` with `send_payload "${cycle_payload[$best_dir]}" || { log "Send failed"; return 1; }`.

(f) In `cleanup()`, after `stop_notify_subscriber` add `stop_mqtt_side`.

(g) Replace `log "=== Claude Usage Tracker Daemon (BLE) ==="` and the following lines up to (not including) `BACKOFF=1` with:

```bash
log "=== Claude Usage Tracker Daemon ==="
log "Poll interval: ${POLL_INTERVAL}s"

if [ -f "$BROKER_DIR/broker.env" ]; then
    # shellcheck disable=SC1091
    . "$BROKER_DIR/broker.env"
fi
TRANSPORT=$(read_transport_setting)
if [ "$TRANSPORT" = mqtt ] && [ -z "${BROKER_ID:-}" ]; then
    log "transport = mqtt but no broker is set up; falling back to BLE"
    TRANSPORT=ble
fi

if [ "$TRANSPORT" = mqtt ]; then
    log "Transport: MQTT (WiFi)"
    LAST_POLL=0
    while true; do
        side_services_tick
        refresh_sub_ensure
        NOW=$(date +%s)
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            if [ ! -s "$BROKER_DIR/devices" ]; then
                log "No WiFi devices set up yet; run daemon/clawdmeter-setup"
                LAST_POLL=$NOW
            else
                poll && LAST_POLL=$NOW
            fi
        fi
        sleep "$TICK"
    done
fi
```

(h) In the BLE loops, call `side_services_tick` as the first line inside the outer `while true; do`, and as the first line inside `while is_connected; do`.

- [ ] **Step 4: Run tests**

Run: `bash daemon/tests/test_bash_mqtt.sh && bash daemon/tests/test_bash_token.sh && bash -n daemon/claude-usage-daemon.sh`
Expected: all `PASS:`, then the token test passes, and the syntax check is silent.

- [ ] **Step 5: Commit**

```bash
git add daemon/claude-usage-daemon.sh daemon/tests/test_bash_mqtt.sh
git commit -m "daemon(Linux): host the broker via avahi + mosquitto, publish over MQTT"
```

---

### Task 10: Installers + docs

**Files:**
- Modify: `install-mac.sh`, `install.sh`, `config.example`, `README.md`, `CLAUDE.md`, `docs/porting/capability-flags.md`

- [ ] **Step 1: `install-mac.sh`.**
  - In step [2/6], change the pip line to `"$VENV_DIR/bin/pip" install --quiet "bleak>=0.22" "httpx>=0.27" "paho-mqtt>=2.0" "zeroconf>=0.132"`.
  - Directly after step [4/6] (`configure_chime` + `echo ""`), insert:

```bash
# Optional WiFi transport: a local password-protected MQTT broker the daemon
# hosts. Devices are then set up with daemon/clawdmeter-setup.
configure_wifi_transport() {
    [ -t 0 ] || return 0
    local ans
    read -r -p "  Enable WiFi transport (MQTT; S3 boards only)? [y/N] " ans || ans=""
    [[ "$ans" =~ ^[Yy]$ ]] || { echo "  WiFi transport skipped (Bluetooth only)."; return 0; }
    if ! command -v mosquitto_passwd >/dev/null 2>&1; then
        command -v brew >/dev/null 2>&1 || { echo "  Homebrew is needed to install mosquitto; skipping."; return 0; }
        brew install mosquitto
    fi
    bash "$SCRIPT_DIR/daemon/broker/broker-init.sh"
    WIFI_ENABLED=1
}
WIFI_ENABLED=0
configure_wifi_transport
echo ""
```

  - In the final "=== Done ===" block, after the pairing steps, add:

```bash
if [ "$WIFI_ENABLED" = 1 ]; then
    echo "WiFi setup (after Bluetooth pairing, with the daemon running):"
    echo "  $SCRIPT_DIR/daemon/clawdmeter-setup"
    echo "  Type the 6-digit code the device shows. If macOS asks whether"
    echo "  mosquitto may accept incoming connections, click Allow."
    echo ""
fi
```

- [ ] **Step 2: `install.sh`.**
  - Add the same `configure_wifi_transport` function, but with the Linux install line `sudo apt-get install -y mosquitto mosquitto-clients avahi-utils python3-venv`.
  - After `broker-init.sh`, create the setup CLI venv:

```bash
    local venv="$SCRIPT_DIR/daemon/.venv"
    [ -d "$venv" ] || python3 -m venv "$venv"
    "$venv/bin/pip" install --quiet "bleak>=0.22" "httpx>=0.27" "paho-mqtt>=2.0" "zeroconf>=0.132"
```

  - Call it after `configure_chime` in step [3/4]. Add the same "WiFi setup" block to the final output, without the macOS firewall sentence.
  - Add a comment: the distro's own mosquitto service is left alone; ours listens on 18830, so they don't clash.
  - Fix the stale "SC01 Plus" wording in the final output to "Clawdmeter".

- [ ] **Step 3: `config.example`** — append:

```ini
# How usage reaches the device.
#   ble  = Bluetooth (default)
#   mqtt = WiFi via the daemon's local MQTT broker. Don't set this by hand:
#          run daemon/clawdmeter-setup, which sets it once the device is online.
# transport = ble
```

- [ ] **Step 4: `README.md`.**
  - Add a `## WiFi setup (optional, S3 boards)` section before `## How it works`. Cover:
    - What it does: auto-reconnect when you come back, and BLE kept for the keys.
    - Prerequisites: run the installer, choose "y" for WiFi, and pair over Bluetooth first.
    - Steps: run `daemon/clawdmeter-setup`, type the code, and wait for "Connected over WiFi."
    - Re-provisioning: run it again. Factory reset: hold PWR for 3 s, then release; this clears both the Bluetooth bond and WiFi.
    - Security: a password per device plus ACLs. Traffic is not encrypted on the LAN, the credentials sit in device flash, and the Anthropic token never leaves the computer.
    - C6 boards stay Bluetooth-only.
  - In `## BLE protocol`:
    - Add the rows `REQ Characteristic (notify)` `...0004` and `PROV Characteristic (write/notify)` `...0005`.
    - Add a short "MQTT topics" table: `clawdmeter/<id>/usage` (retained JSON, same format as RX), `refresh`, `status`.
    - Add a line on `_clawdmeter._tcp` with TXT `id`.

- [ ] **Step 5: `CLAUDE.md`.**
  - In "Daemon / host side", add a **WiFi/MQTT transport** paragraph covering:
    - The broker dir and port 18830.
    - `transport = mqtt` in the config.
    - Device id = MQTT user; ACL uses `%u`.
    - Device discovery via `_clawdmeter._tcp` TXT id.
    - `net.cpp` runs on its own task; rediscovers after 20 s down.
    - Serial dev cmds `prov {json}` / `provbegin`.
    - Hold-to-pair clears WiFi too.
  - In the architecture tree, add `net.{h,cpp}`, `prov.{h,cpp}` and `netcfg.{h,cpp}` with one-line descriptions.

- [ ] **Step 6: `docs/porting/capability-flags.md`** — add a table row:

```markdown
| `BOARD_HAS_WIFI`               | 0       | Set as a `-D` flag in `platformio.ini` (like `BOARD_HAS_PSRAM`). Enables the WiFi + MQTT transport (`net.cpp`, PROV `begin`). Leave undefined on PSRAM-less chips (C6): WiFi + BLE + LVGL don't fit internal SRAM there yet. Without it `net.cpp` compiles to stubs and PROV replies `{"err":"nowifi"}`. |
```

- [ ] **Step 7: Syntax-check installers, then commit**

Run: `bash -n install.sh && bash -n install-mac.sh && echo ok`
Expected: `ok`

```bash
git add install-mac.sh install.sh config.example README.md CLAUDE.md docs/porting/capability-flags.md
git commit -m "docs+installers: optional WiFi/MQTT transport setup"
```

---

### Task 11: End-to-end verification on hardware

No new code. Each step below is pass or fail; report each result to the user.

- [ ] **Step 1: Full build + host tests**

Run:
```bash
for e in waveshare_amoled_216 waveshare_amoled_18 waveshare_amoled_206 waveshare_lcd_154 waveshare_lcd_4 waveshare_amoled_216_c6 waveshare_amoled_18_c6 sim; do pio run -d firmware -e $e >/tmp/build_$e.log 2>&1 && echo "OK $e" || echo "FAIL $e"; done
(cd firmware && g++ -std=c++17 -I src -I .pio/libdeps/sim/ArduinoJson/src test/test_netcfg/test_main.cpp src/netcfg.cpp -o /tmp/t_netcfg && /tmp/t_netcfg)
daemon/.venv/bin/python -m pytest daemon/tests/test_mqtt_transport.py daemon/tests/test_setup_cli.py daemon/tests/test_daemon_mqtt.py daemon/tests/test_macos_multidir.py daemon/tests/test_freeride.py -q
bash daemon/tests/test_broker_init.sh && bash daemon/tests/test_bash_mqtt.sh && bash daemon/tests/test_bash_token.sh
```
Expected: 8× `OK`, `all netcfg tests passed`, pytest all passed, every bash line `PASS:`.

- [ ] **Step 2: Real setup flow (macOS, S3 board)**
  1. Flash the board.
  2. Run `./install-mac.sh` and answer "y" to WiFi.
  3. Pair the device in Bluetooth Settings.
  4. Run `daemon/clawdmeter-setup` and type the code shown on screen.
  5. Expected: "Connected over WiFi.", then "Done.". The daemon log shows `Transport: MQTT (WiFi)` and then `Publishing:` lines, and the device shows live usage (check with `./screenshot.sh`).

- [ ] **Step 3: The walk-away case.** Run `launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`. Wait 2 min; the device goes to "Finding host…". Run `launchctl load -w …`. Expected: live data within about 25 s, with no touch.

- [ ] **Step 4: IP change.** Toggle the Mac's WiFi off and on (a new DHCP lease if possible). Expected: the device recovers within about 40 s.

- [ ] **Step 5: HID regression.** Press BOOT: a Space is typed into the focused app. On boards with a second button, it types Shift+Tab.

- [ ] **Step 6: Wrong WiFi password.** Re-run `clawdmeter-setup` with a wrong password. Expected: the CLI prints the 45 s timeout message, and the device keeps showing data over BLE if the BLE daemon path is active, or "Joining WiFi…".

- [ ] **Step 7: Factory reset.** Hold PWR about 3 s, then release. Expected in the serial log: `bonds cleared` and `PROV: WiFi config cleared`. The screen shows the pairing hint.

- [ ] **Step 8: C6 (if available).** Run `clawdmeter-setup` against a C6 board. Expected: "This board has no WiFi support".

- [ ] **Step 9: Finish the branch.** Use `superpowers:finishing-a-development-branch`. Per the user's memory, "push it" means merge locally into `main` and never `git push`.
