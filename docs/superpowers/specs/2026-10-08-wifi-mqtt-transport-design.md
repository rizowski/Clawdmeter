# WiFi + MQTT transport for Clawdmeter — design

Status: approved 2026-10-08. Branch: `feat/wifi-mqtt`.

## Context

Usage data reaches the device only over a BLE GATT write from the host daemon. When the laptop leaves range and comes back, the BLE data link often does not come back on its own. The display then sits on "Zzz" or the pair screen.

Goal: usage data travels over WiFi + MQTT, with automatic reconnect. Setup is a one-time flow: the device shows a 6-digit code, the user types it into a setup command, and the command pushes WiFi and broker details to the device over BLE.

Decisions already made with the user:
- BLE stays for the HID keyboard (Space / Shift+Tab) and for setup. Usage data moves to MQTT.
- The host daemon runs the MQTT broker and announces it over mDNS.
- The broker uses password auth: a separate username and password for each device, plus ACLs. No TLS, and no certs in the code, because the repo is public.
- macOS (Python) and Linux (bash) daemons get MQTT. Windows stays BLE-only.
- The code is typed into a `clawdmeter-setup` terminal command, not the OS Bluetooth dialog.
- v1 covers only the five ESP32-S3 boards (they have PSRAM). C6 boards stay BLE-only (gated by `BOARD_HAS_WIFI`).

## Architecture

```
 laptop (daemon host)                                   Clawdmeter (S3)
 ┌──────────────────────────────┐                       ┌────────────────────────┐
 │ daemon (py / bash)           │                       │ main loop              │
 │  polls Anthropic, publishes ─┼──► mosquitto :18830 ◄─┼─ net.cpp (esp-mqtt)    │
 │  subscribes refresh          │    user/pass + ACL    │   mDNS browse          │
 │ mDNS: _clawdmeter._tcp       │                       │ ble.cpp (HID + PROV)   │
 │ clawdmeter-setup (bleak) ────┼── BLE PROV char ─────►│ prov.cpp (NVS config)  │
 └──────────────────────────────┘                       └────────────────────────┘
```

### Topics (prefix `clawdmeter/<device_id>/`, device_id = last 6 hex digits of the MAC)
- `usage`: daemon publishes **retained**, QoS 1. Same JSON schema as today, so the existing `parse_json` (`main.cpp:101`) does not change. A retained message means the device gets the latest data immediately on reconnect.
- `refresh`: the device publishes after each (re)connect. The daemon subscribes and polls right away (same job as the BLE REQ char today).
- `status`: the device publishes `online`, retained. The LWT is `offline`. Used for diagnostics.

### Broker security (password auth, no certs)
- Mosquitto runs as a child process of the daemon, using a generated config in `~/.config/claude-usage-monitor/broker/` (next to the existing daemon config).
  - One listener on port **18830**. This non-default port avoids clashing with a Home Assistant or distro broker on 1883. mDNS advertises the port, so the device never hard-codes it.
  - `allow_anonymous false`.
- No certs or secrets in the repo. All credentials are generated at install or setup time on the user's own machine.
- `daemon/broker/broker-init.sh` (shared by both OSes, run by the installers) runs once:
  - Creates the `daemon` user with a random password, using `mosquitto_passwd`.
  - Writes the ACL.
  - Generates a random `broker_id`.
- ACL (static file, never edited per device):
  - The daemon user can write `+/usage` and read `+/refresh` and `+/status`.
  - The MQTT username of a device **is** its device id. So `pattern read clawdmeter/%u/usage` and `pattern write clawdmeter/%u/{refresh,status}` limit each device to its own topics.
- `clawdmeter-setup` gives each device a random 24-char password. It adds the password to the password file and reloads mosquitto with SIGHUP.
- Known limits, documented in the README:
  - MQTT traffic, including the password, is not encrypted on the LAN.
  - WiFi and MQTT credentials sit in NVS without flash encryption.
  - The usage numbers are low value, and the Anthropic token never leaves the laptop.

### Discovery and reconnect (the core goal)
- The daemon announces `_clawdmeter._tcp` on port 18830 with TXT `id=<broker_id>`. Python uses `zeroconf`; Linux uses `avahi-publish-service`.
- Device reconnect loop in `net_tick()`:
  1. WiFi auto-reconnect.
  2. mDNS browse for `_clawdmeter._tcp` with a matching `id`.
  3. esp-mqtt connect, with built-in backoff.
- If the broker is gone (laptop away), the browse runs again every 15 s. The IP may change between sessions, so it never caches.
- On connect: subscribe to `usage`, publish `refresh`. The UI shows data as soon as the retained message arrives.

### Provisioning (BLE)
- New characteristic `...0005` (PROV, WRITE + NOTIFY) on the existing data service. It uses the same owner + encryption gate as RX (`ble.cpp:262-287`). The laptop already owns the bond from normal OS pairing, which the HID keyboard needs anyway.
- Flow, run by `clawdmeter-setup`:
  1. The CLI finds the device the same way the macOS daemon does (`retrievePeripheralsWithServices`, `.py:240-292`; on Linux, bleak's BlueZ backend). It writes `{"op":"begin"}`.
  2. The device shows a random 6-digit code on screen for 120 s.
  3. The CLI prompts for the code. It detects the SSID (`nmcli` on Linux; on macOS it prompts with the last-used SSID as the default) and prompts for the WiFi password with hidden input.
  4. The CLI creates the device's MQTT user and password, then sends one JSON write, about 250 bytes, which is under the 512-byte ATT limit:
     `{"op":"config","code":"123456","ssid":…,"wpass":…,"broker_id":…,"pass":…}`. There is no `user` field: the device uses its own id as the username.
  - The `begin` reply is `{"id":"<device_id>"}`, so the CLI knows which MQTT user to create. On boards without WiFi it is `{"err":"nowifi"}`.
  5. The device checks the code first. If it is wrong, it notifies `{"err":"code"}`. After 5 failures the code is invalidated and the user must run `begin` again. If it is right, the device saves to NVS (namespace `clawdnet`) and notifies `{"ok":true}`. It then connects WiFi + MQTT and notifies `{"net":"up"}` once the broker connects, so the CLI can report success end to end.
- Re-running setup overwrites the config. Hold-to-pair (`ble_clear_bonds`) also clears the WiFi config, which gives one factory-reset gesture.

### Firmware components
- **`firmware/src/net.{h,cpp}`** (new):
  - Functions: `net_init`, `net_apply_config`, `net_get_state` (OFF / WIFI / DISCOVERING / CONNECTED / AUTH_FAIL), `net_has_data`, `net_get_data`. The device sends its refresh request inside the MQTT connect handler, so no separate refresh call is needed.
  - All blocking work (WiFi join, the ~3 s mDNS browse, esp-mqtt start and stop) runs on a dedicated FreeRTOS task, so the LVGL loop never stalls.
  - It mirrors the `ble_has_data` / `ble_get_data` polling shape, so `main.cpp` treats both sources the same.
  - The esp-mqtt event handler copies the payload into a 512-byte buffer under a mutex, because esp-mqtt runs in its own task.
  - When `BOARD_HAS_WIFI` is not set, the functions compile to no-op stubs (C6 + sim).
- **`firmware/src/prov.{h,cpp}`** (new):
  - NVS load and save of the WiFi and MQTT config.
  - Code generation and verification with `esp_random`, config JSON parsing, and `prov_is_configured()`.
  - `ble.cpp` routes PROV writes into it.
- **`main.cpp`**:
  - The loop polls `net_has_data()` beside `ble_has_data()` (`main.cpp:380-402`). Both feed `parse_json` → `ui_update`.
  - The link-state check (`:361-365`) passes the combined connected state (`BLE data link || MQTT`) to the UI.
- **`ui.cpp`**:
  - `ui_update_ble_status` becomes a link status driven by both transports. The pair screen (`:585-614`) shows only when neither link is up.
  - A new setup-code overlay shows the 6 digits large, plus "Enter this code in clawdmeter-setup".
  - The status line shows "WiFi…", "Finding host…" or "Connected".
- **Board flags**: add `-DBOARD_HAS_WIFI` to the five S3 envs in `platformio.ini`. This is a capability flag, not `#ifdef BOARD_*`, so it follows `docs/porting/capability-flags.md`.
- **Flash**: set `waveshare_amoled_216` to 16 MB / `default_16MB.csv` like the other S3 envs, in case WiFi pushes the app past the default partition. Check sizes after the build.
- **Serial dev commands**: `prov {json}` writes a config without a code check (USB access already means physical control), and `provbegin` shows a code. Both let net and UI be tested before the setup CLI exists.
- **Sim**: the stubbed `net` reports OFF, so the sim stays unchanged. Optionally add a `w` key later to fake MQTT state (YAGNI for v1).

### Host components
- **`daemon/broker/broker-init.sh`** (new, shared): password file, ACL, `broker_id` and the mosquitto conf template.
- **`daemon/clawdmeter_setup.py`** (new, macOS + Linux): the bleak provisioning CLI described above. It uses `mosquitto_passwd` to add the device user.
- **`daemon/claude_usage_daemon.py`** (macOS):
  - Supervises the mosquitto child process and restarts it if it exits.
  - Announces with zeroconf and publishes with paho-mqtt to localhost as the `daemon` user.
  - Subscribes to `+/refresh`.
  - When `transport = mqtt` is set in `config`, BLE data writes are skipped. BLE stays as the fallback for devices that have not been set up.
- **`daemon/claude-usage-daemon.sh`** (Linux):
  - Same behaviour with `mosquitto`, `mosquitto_pub -r` and `avahi-publish-service` as background children.
  - Refresh uses `mosquitto_sub`, which touches the existing flag file. This reuses the dbus-monitor flag pattern (`.sh:230-247`).
- **Installers**:
  - `install-mac.sh`: `brew install mosquitto`, and add `paho-mqtt` + `zeroconf` to the venv.
  - `install.sh`: `apt install mosquitto mosquitto-clients avahi-utils` and a python venv for the setup CLI. The distro's mosquitto service stays as it is. Our broker uses its own conf on 18830, so the two do not clash.
- **Docs**: a README section "WiFi setup", the protocol table updated with PROV + topics, and a CLAUDE.md "Daemon / host side" note.

## Error handling
- Wrong code: the device notifies an error and the CLI asks again (max 5).
- Bad WiFi password: no `net:up` arrives within 30 s. The CLI reports "WiFi join failed" and the device keeps its BLE path.
- Broker unreachable: the device stays in DISCOVERING. It shows the last data, then goes stale after 90 s (`DATA_FRESH_MS`), which is today's behaviour.
- MQTT auth is rejected (broker re-initialised, or the user was deleted): the UI status shows "Host login rejected — re-run setup".
- mosquitto crashes: the daemon restarts it with a backoff and logs it.

## Phases
Suggested phases for the implementation plan, each one testable on its own:
   - Phase A: broker-init + mosquitto running on the Mac, checked with `mosquitto_pub` / `mosquitto_sub`.
   - Phase B: firmware `net` with a hard-coded config, receiving usage from the broker.
   - Phase C: `prov` + PROV char + setup CLI + code overlay.
   - Phase D: macOS daemon publishing over MQTT.
   - Phase E: Linux bash daemon + installers + docs.

Rough effort: about 2 days of build plus a few hours of hardware testing on a real S3 board.

## Verification
- **Builds**: `pio run -d firmware -e <env>` for all 7 hardware envs + `sim`. C6 and sim must still build with the stubs. Check app size against the partition table.
- **Broker**:
  - `mosquitto_sub -p 18830 -u daemon -P …` succeeds.
  - An anonymous connection, or one with the wrong password, is refused.
  - A device user cannot read another device's topic (ACL test).
- **End-to-end on hardware** (S3 board):
  1. Run `clawdmeter-setup`, type the shown code, and see `net:up`.
  2. Read `screenshot.sh` output with the Read tool to confirm the usage screen fills from MQTT.
  3. Stop the daemon (simulates walking away), then start it again. The device reconnects without anyone touching it, and data appears within about 20 s.
  4. Change the laptop's IP or reconnect WiFi. The device rediscovers it over mDNS.
  5. Hold-to-pair, then confirm the WiFi config is cleared.
- **Regression**: the Windows daemon still works over BLE, and the HID keys still type Space / Shift+Tab.
- **Daemon tests**: extend `daemon/tests/` with a payload-publish test against a local mosquitto.
