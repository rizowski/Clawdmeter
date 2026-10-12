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
check '[ "$(mode_of "$B/broker.env")" = 600 ] && [ "$(mode_of "$B/passwd")" = 600 ] && [ "$(mode_of "$B/client.conf")" = 600 ]' "secrets are 0600"
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
check '[ ${#VIEWER_PASS} -eq 24 ]' "viewer password is 24 chars"
first_viewer="$VIEWER_PASS"
bash "$INIT" "$B" >/dev/null; . "$B/broker.env"
check '[ "$VIEWER_PASS" = "$first_viewer" ]' "re-run keeps the viewer password"
# A broker set up before the viewer account existed gets one on re-run.
O="$TMP/old"; mkdir -p "$O"; chmod 700 "$O"
printf 'BROKER_ID=0011223344556677\nBROKER_PORT=18931\nDAEMON_PASS=aaaaaaaaaaaaaaaaaaaaaaaa\n' > "$O/broker.env"
bash "$INIT" "$O" >/dev/null
check 'grep -Eq "^VIEWER_PASS=[0-9a-f]{24}$" "$O/broker.env" && grep -q "^DAEMON_PASS=aaaa" "$O/broker.env"' "re-run adds a viewer to an older broker"

MOSQ="$(clawd_find_tool mosquitto)" || { echo "SKIP: live broker checks (mosquitto not installed)"; exit $fail; }
MPASS="$(clawd_find_tool mosquitto_passwd)"
"$MOSQ" -c "$B/mosquitto.conf" >/dev/null 2>&1 &
BPID=$!
sleep 1
P=(-h 127.0.0.1 -p 18931)

check '! mosquitto_pub "${P[@]}" -t x -m y 2>/dev/null' "anonymous publish refused"
check '! mosquitto_pub "${P[@]}" -u daemon -P wrong -t x -m y 2>/dev/null' "wrong password refused"
# The daemon logs in through an options file so the password never sits in
# argv (visible to every local user via ps).
check 'mosquitto_pub -o "$B/client.conf" -t clawdmeter/abc123/status -m t 2>/dev/null' "daemon logs in via client.conf"

"$MPASS" -b "$B/passwd" abc123 devpass
kill -HUP "$BPID"; sleep 0.5
mosquitto_pub "${P[@]}" -u daemon -P "$DAEMON_PASS" -q 1 -r -t clawdmeter/abc123/usage -m '{"s":1}'
mosquitto_pub "${P[@]}" -u daemon -P "$DAEMON_PASS" -q 1 -r -t clawdmeter/ffffff/usage -m '{"s":2}'
got="$(mosquitto_sub "${P[@]}" -u abc123 -P devpass -t clawdmeter/abc123/usage -C 1 -W 3 2>/dev/null)"
check '[ "$got" = "{\"s\":1}" ]' "device reads its own retained usage"
other="$(mosquitto_sub "${P[@]}" -u abc123 -P devpass -t clawdmeter/ffffff/usage -C 1 -W 2 2>/dev/null)"
check '[ -z "$other" ]' "device cannot read another device's usage"
# viewer: read-only access to every device, for tools like MQTT Analyzer.
V=(-u viewer -P "$VIEWER_PASS")
all="$(mosquitto_sub "${P[@]}" "${V[@]}" -t 'clawdmeter/+/usage' -C 2 -W 3 2>/dev/null | sort | tr '\n' ' ')"
check '[ "$all" = "{\"s\":1} {\"s\":2} " ]' "viewer reads every device's usage"
mosquitto_pub "${P[@]}" "${V[@]}" -q 1 -r -t clawdmeter/abc123/usage -m '{"s":99}' 2>/dev/null
got="$(mosquitto_sub "${P[@]}" -u abc123 -P devpass -t clawdmeter/abc123/usage -C 1 -W 3 2>/dev/null)"
check '[ "$got" = "{\"s\":1}" ]' "viewer cannot publish usage"
# The per-device %u patterns apply to every user, so without a deny rule the
# viewer could publish clawdmeter/viewer/refresh and make the daemon poll.
( sleep 1; mosquitto_pub "${P[@]}" "${V[@]}" -t clawdmeter/viewer/refresh -m x 2>/dev/null
  mosquitto_pub "${P[@]}" "${V[@]}" -t clawdmeter/viewer/status -m x 2>/dev/null ) &
PUBS=$!
heard="$(mosquitto_sub "${P[@]}" -u daemon -P "$DAEMON_PASS" -t 'clawdmeter/+/refresh' -t 'clawdmeter/+/status' -v -C 1 -W 3 2>/dev/null)"
wait "$PUBS"
check '[ -z "$heard" ]' "viewer cannot trigger a refresh or fake a status"

exit $fail
