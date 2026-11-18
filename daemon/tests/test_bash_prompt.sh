#!/bin/bash
# Tests for the bash daemon's prompt-hook support: poll() shows the plan of the
# last Claude Code prompt, drops the chime flag on a plan switch, and
# prompt_switch_check() asks for an early poll only for a prompt in another plan.
#
# Run: bash daemon/tests/test_bash_prompt.sh   (needs bash 4+)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DAEMON="$HERE/../claude-usage-daemon.sh"
extract() { awk "/^$1\\(\\) \\{/{f=1} f{print} f&&/^\\}/{exit}" "$DAEMON"; }
for fn in read_config_dirs _last_prompt_mtime read_last_prompt_dir _same_dir \
          prompt_switch_check prompt_switch_due _payload_session_pct poll; do
    eval "$(extract "$fn")"
done
log() { LOGS="${LOGS:-}|$1"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
check() { if eval "$1"; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }

A="$TMP/a"; B="$TMP/b"; mkdir -p "$A" "$B"
CONFIG_FILE="$TMP/config"
printf 'config_dirs = %s, %s\n' "$A" "$B" > "$CONFIG_FILE"
LAST_PROMPT_FILE="$TMP/last-prompt"
declare -A PREV_S=() LAST_ACTIVE=() UTIL=([$A]=5 [$B]=80)
POLL_SEQ=0; CURRENT_DIR=""; LAST_PROMPT_SEEN=""; PROMPT_SWITCH=0
REFRESH_MIN_GAP=15

read_token_for() { echo "tok:$1"; }
build_payload_for_token() { printf '{"s":%s,"sr":1,"c":1,"ok":true}' "${UTIL[${1#tok:}]}"; }
send_payload() { SENT="$1"; }

poll
check '[ "$CURRENT_DIR" = "$B" ]' "no hook: the higher-usage plan shows"

printf '%s/\n' "$A" > "$LAST_PROMPT_FILE"
poll
check '[ "$CURRENT_DIR" = "$A" ] && [ "$SENT" = "{\"s\":5,\"sr\":1,\"ok\":true}" ]' \
    "last prompt picks its plan, and the switch drops the chime flag"
poll
check '[ "$SENT" = "{\"s\":5,\"sr\":1,\"c\":1,\"ok\":true}" ]' "same plan again keeps the chime flag"

UTIL[$B]=90; poll
check '[ "$CURRENT_DIR" = "$A" ]' "a usage rise elsewhere does not override the last prompt"

printf '/not/configured\n' > "$LAST_PROMPT_FILE"
poll
check '[ "$CURRENT_DIR" = "$B" ]' "an unconfigured prompt dir falls back to the usage guess"

# prompt_switch_check: CURRENT_DIR is B now.
LAST_PROMPT_SEEN=$(_last_prompt_mtime)
prompt_switch_check
check '[ "$PROMPT_SWITCH" = 0 ]' "an unchanged prompt file asks for nothing"
sleep 1; printf '%s\n' "$B" > "$LAST_PROMPT_FILE"
prompt_switch_check
check '[ "$PROMPT_SWITCH" = 0 ]' "a prompt in the plan shown asks for nothing"
sleep 1; printf '%s\n' "$A" > "$LAST_PROMPT_FILE"
prompt_switch_check
check '[ "$PROMPT_SWITCH" = 1 ]' "a prompt in another plan asks for an early poll"
check 'prompt_switch_due 15 && ! prompt_switch_due 14' "the early poll keeps REFRESH_MIN_GAP"

PROMPT_SWITCH=0
printf 'config_dirs = %s\n' "$A" > "$CONFIG_FILE"
sleep 1; printf '%s\n' "$B" > "$LAST_PROMPT_FILE"
prompt_switch_check
check '[ "$PROMPT_SWITCH" = 0 ]' "a single configured dir ignores the hook"

exit $fail
