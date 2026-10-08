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
