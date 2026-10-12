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

# A failed send (broker down) still counts as a poll: at most one API call
# per POLL_INTERVAL instead of one every TICK.
eval "$(extract mqtt_tick)"
POLL_INTERVAL=60
REFRESH_FLAG="$TMP/refresh"
BROKER_DIR="$TMP/stub"; mkdir -p "$BROKER_DIR"; echo a1b2c3 > "$BROKER_DIR/devices"
poll() { POLLS=$((POLLS + 1)); return 1; }
POLLS=0; LAST_POLL=0
mqtt_tick 1000; mqtt_tick 1005; mqtt_tick 1010
check '[ "$POLLS" -eq 1 ] && [ "$LAST_POLL" -eq 1000 ]' "failed send doesn't re-poll every tick"
touch "$REFRESH_FLAG"; mqtt_tick 1015
check '[ "$POLLS" -eq 2 ] && [ ! -f "$REFRESH_FLAG" ]' "refresh request polls right away"

MOSQ="$(clawd_find_tool mosquitto)" || { echo "SKIP: publish test (mosquitto not installed)"; exit $fail; }
BROKER_DIR="$TMP/b"
CLAWD_BROKER_PORT=18934 bash "$HERE/../broker/broker-init.sh" "$BROKER_DIR" >/dev/null
. "$BROKER_DIR/broker.env"
"$(clawd_find_tool mosquitto_passwd)" -b "$BROKER_DIR/passwd" a1b2c3 devpass
printf 'a1b2c3\nnot-valid\n' > "$BROKER_DIR/devices"
"$MOSQ" -c "$BROKER_DIR/mosquitto.conf" >/dev/null 2>&1 & BPID=$!
sleep 1

# Publish must log in via client.conf: a password in argv is visible in ps.
check 'f="$(extract mqtt_publish)"; [ -n "$f" ] && ! grep -q -- " -P " <<<"$f" && grep -q -- " -o " <<<"$f"' "mqtt_publish logs in via client.conf, no password in argv"
mqtt_publish '{"s":7,"ok":true}'; rc=$?
check '[ $rc -eq 0 ]' "mqtt_publish succeeds"
got="$(mosquitto_sub -h 127.0.0.1 -p 18934 -u a1b2c3 -P devpass -t clawdmeter/a1b2c3/usage -C 1 -W 3)"
check '[ "$got" = "{\"s\":7,\"ok\":true}" ]' "device receives the retained payload"
exit $fail
