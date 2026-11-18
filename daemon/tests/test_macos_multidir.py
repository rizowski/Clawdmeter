#!/usr/bin/env python3
"""Unit tests for the macOS/Linux daemon's multi config-dir active-plan support.

Covers read_config_dirs, read_token_for, PlanSelector, and poll_active_payload.

Run: python -m pytest daemon/tests/test_macos_multidir.py -x -q
"""
import asyncio
from pathlib import Path
from unittest.mock import AsyncMock, patch

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import PlanSelector, read_config_dirs, read_token_for


def _run(coro):
    return asyncio.run(coro)


# ---------------------------------------------------------------------------
# read_config_dirs
# ---------------------------------------------------------------------------

def test_config_dirs_defaults_to_claude_when_unset(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    assert read_config_dirs() == [mod.DEFAULT_CONFIG_DIR]


def test_config_dirs_defaults_when_key_absent(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\nchime = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == [mod.DEFAULT_CONFIG_DIR]


def test_config_dirs_parses_comma_list_and_expands_tilde(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("config_dirs = ~/.claude, ~/.claude-work  # two plans\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == [Path.home() / ".claude", Path.home() / ".claude-work"]


# ---------------------------------------------------------------------------
# read_token_for
# ---------------------------------------------------------------------------

def test_token_for_reads_dir_credentials_file(tmp_path):
    (tmp_path / ".credentials.json").write_text('{"claudeAiOauth":{"accessToken":"TOK_X"}}')
    assert read_token_for(tmp_path) == "TOK_X"


def test_token_for_missing_file_non_default_returns_none(tmp_path, monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "linux")
    assert read_token_for(tmp_path) is None  # no file, not the default dir


def test_token_for_default_dir_falls_back_to_keychain_on_macos(tmp_path, monkeypatch):
    # An empty dir standing in as the default: no file present -> Keychain.
    monkeypatch.setattr(mod, "DEFAULT_CONFIG_DIR", tmp_path)
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    with patch.object(mod, "_read_token_keychain", return_value="TOK_KEYCHAIN"):
        assert read_token_for(tmp_path) == "TOK_KEYCHAIN"


def test_keychain_service_default_dir_is_bare_name(monkeypatch):
    assert mod.keychain_service_for(mod.DEFAULT_CONFIG_DIR) == "Claude Code-credentials"


def test_keychain_service_other_dir_is_hashed():
    # Claude Code names a CLAUDE_CONFIG_DIR dir's entry by sha256(path)[:8].
    import hashlib
    d = Path("/Users/someone/.claude-work")
    h = hashlib.sha256(b"/Users/someone/.claude-work").hexdigest()[:8]
    assert mod.keychain_service_for(d) == f"Claude Code-credentials-{h}"


def test_token_for_other_dir_reads_its_keychain_entry_on_macos(tmp_path, monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    seen = []
    with patch.object(mod, "_read_token_keychain",
                      side_effect=lambda svc: seen.append(svc) or "TOK_WORK"):
        assert read_token_for(tmp_path) == "TOK_WORK"
    assert seen == [mod.keychain_service_for(tmp_path)]


def test_token_for_file_wins_over_keychain(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "DEFAULT_CONFIG_DIR", tmp_path)
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    (tmp_path / ".credentials.json").write_text('{"accessToken":"TOK_FILE"}')
    with patch.object(mod, "_read_token_keychain", return_value="TOK_KEYCHAIN"):
        assert read_token_for(tmp_path) == "TOK_FILE"


# ---------------------------------------------------------------------------
# Blank credentials must read as ABSENT. Logging out of the CLI empties the
# values in place rather than deleting them; "" is a str, so a type-only check
# would pass it through and the daemon would poll with an empty Bearer token.
# ---------------------------------------------------------------------------

def test_blank_token_in_file_reads_as_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "linux")
    (tmp_path / ".credentials.json").write_text(
        '{"claudeAiOauth":{"accessToken":"","refreshToken":"","expiresAt":0}}'
    )
    assert read_token_for(tmp_path) is None


def test_blank_token_in_blob_reads_as_absent():
    """Same guard on the extraction chokepoint the Keychain path goes through."""
    assert mod._extract_access_token('{"accessToken":""}') is None
    assert (
        mod._extract_access_token('{"claudeAiOauth":{"accessToken":"","expiresAt":0}}')
        is None
    )


# ---------------------------------------------------------------------------
# PlanSelector — the "active = recent API activity" rule
# ---------------------------------------------------------------------------

A, B = Path("/a"), Path("/b")


def test_selector_startup_picks_highest_util():
    sel = PlanSelector()
    assert sel.choose({A: 10, B: 30}) == B  # no history yet -> highest %


def test_selector_switches_on_rise():
    sel = PlanSelector()
    sel.choose({A: 10, B: 30})           # startup -> B
    assert sel.choose({A: 20, B: 30}) == A  # A rose 10->20 -> A active


def test_selector_sticky_when_no_movement():
    sel = PlanSelector()
    sel.choose({A: 10, B: 30})
    sel.choose({A: 20, B: 30})           # A active
    assert sel.choose({A: 20, B: 30}) == A  # nothing moved -> still A (not higher B)


def test_selector_reset_to_zero_is_not_activity():
    sel = PlanSelector()
    sel.choose({A: 10, B: 30})
    sel.choose({A: 20, B: 30})           # A active
    sel.choose({A: 20, B: 45})           # B rose -> B active
    assert sel.choose({A: 20, B: 0}) == B   # B window reset (drop) isn't a rise -> stays B


def test_selector_larger_rise_wins_same_cycle():
    sel = PlanSelector()
    sel.choose({A: 10, B: 10})           # seed
    assert sel.choose({A: 12, B: 40}) == B  # both rose same cycle -> higher % breaks tie


# ---------------------------------------------------------------------------
# poll_active_payload — integration over the helpers
# ---------------------------------------------------------------------------

def test_poll_active_payload_picks_active_and_skips_tokenless(monkeypatch):
    dirs = [A, B]
    monkeypatch.setattr(mod, "read_config_dirs", lambda: dirs)
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: None}[d])  # B has no token

    async def fake_poll(token):
        return {"s": 25, "ok": True} if token == "tA" else None

    sel = PlanSelector()
    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(sel))
    assert payload == {"s": 25, "ok": True}  # only A had a token


def test_poll_active_payload_returns_none_when_all_fail(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: None)
    with patch.object(mod, "poll_api", new=AsyncMock(return_value=None)):
        assert _run(mod.poll_active_payload(PlanSelector())) is None


def test_poll_active_payload_selects_higher_util_plan(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: "tB"}[d])

    async def fake_poll(token):
        return {"s": 12, "ok": True} if token == "tA" else {"s": 40, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(PlanSelector()))
    assert payload["s"] == 40  # startup -> highest util plan (B)


# ---------------------------------------------------------------------------
# discover_target — the daemon only ever targets the device this system already
# holds; it never scans for a nearby device by name (there is no scan fallback).
# ---------------------------------------------------------------------------

def test_discover_target_darwin_uses_os_held_device(monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    sentinel = object()
    with patch.object(mod, "retrieve_connected_macos", new=AsyncMock(return_value=sentinel)):
        assert _run(mod.discover_target()) is sentinel  # used directly, no scan


def test_discover_target_darwin_returns_none_when_not_held(monkeypatch):
    # Not held by the OS -> wait (return None); never grabs an arbitrary device.
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    with patch.object(mod, "retrieve_connected_macos", new=AsyncMock(return_value=None)):
        assert _run(mod.discover_target()) is None


def test_discover_target_non_darwin_uses_pinned_address(monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "linux")
    monkeypatch.setattr(mod, "load_cached_address", lambda: "AA:BB:CC:DD:EE:FF")
    assert _run(mod.discover_target()) == "AA:BB:CC:DD:EE:FF"


def test_discover_target_non_darwin_returns_none_without_pin(monkeypatch):
    # No pinned address cached -> wait; never scans by name.
    monkeypatch.setattr(mod.sys, "platform", "linux")
    monkeypatch.setattr(mod, "load_cached_address", lambda: None)
    assert _run(mod.discover_target()) is None


# ---------------------------------------------------------------------------
# poll_api: a plan at its limit answers 429 with the usage headers
# ---------------------------------------------------------------------------

class _FakeHttp:
    def __init__(self, resp):
        self._resp = resp

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False

    async def post(self, *args, **kwargs):
        return self._resp


def _poll_with(status, headers):
    import httpx
    resp = httpx.Response(status, headers=headers, text="{}")
    with patch.object(mod.httpx, "AsyncClient", new=lambda **kw: _FakeHttp(resp)):
        return _run(mod.poll_api("TOK"))


def test_poll_api_reads_usage_from_429_at_the_limit():
    payload = _poll_with(429, {
        "anthropic-ratelimit-unified-5h-utilization": "1.02",
        "anthropic-ratelimit-unified-5h-reset": "0",
        "anthropic-ratelimit-unified-5h-status": "rejected",
        "anthropic-ratelimit-unified-7d-utilization": "0.4",
        "anthropic-ratelimit-unified-7d-reset": "0",
    })
    assert payload is not None
    assert payload["s"] == 100          # clamped: the header reports 1.02
    assert payload["w"] == 40
    assert payload["st"] == "rejected"
    assert payload["acct"] == "pro"


def test_poll_api_429_without_usage_headers_is_transient():
    assert _poll_with(429, {}) is None


def test_poll_api_500_with_usage_headers_is_still_transient():
    assert _poll_with(500, {"anthropic-ratelimit-unified-5h-utilization": "0.5"}) is None


# ---------------------------------------------------------------------------
# plan name ("pl") — the device shows it as the title
# ---------------------------------------------------------------------------

def test_extract_plan_name_from_nested_blob():
    blob = '{"claudeAiOauth": {"accessToken": "T", "subscriptionType": "team"}}'
    assert mod._extract_plan_name(blob) == "Team"


def test_extract_plan_name_unknown_and_missing():
    assert mod._extract_plan_name('{"claudeAiOauth": {"subscriptionType": "max_ultra_plus"}}') == "Max Ultra P"
    assert mod._extract_plan_name('{"claudeAiOauth": {"accessToken": "T"}}') == ""
    assert mod._extract_plan_name("sk-ant-raw-token") == ""


def test_read_plan_for_reads_credentials_file(tmp_path):
    (tmp_path / ".credentials.json").write_text(
        '{"claudeAiOauth": {"accessToken": "T", "subscriptionType": "max"}}')
    assert mod.read_plan_for(tmp_path) == "Max"


def test_poll_active_adds_plan_of_the_active_dir(monkeypatch):
    A, B = Path("/a"), Path("/b")
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: "tB"}[d])
    monkeypatch.setattr(mod, "read_plan_for", lambda d: {A: "Max", B: "Team"}[d])
    monkeypatch.setattr(mod, "read_last_prompt_dir", lambda: B)

    async def fake_poll(token):
        return {"s": 10, "ok": True}
    monkeypatch.setattr(mod, "poll_api", fake_poll)
    assert _run(mod.poll_active_payload(PlanSelector()))["pl"] == "Team"


def test_poll_active_omits_plan_when_unknown(monkeypatch):
    A = Path("/a")
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A])
    monkeypatch.setattr(mod, "read_token_for", lambda d: "tA")
    monkeypatch.setattr(mod, "read_plan_for", lambda d: "")

    async def fake_poll(token):
        return {"s": 10, "ok": True}
    monkeypatch.setattr(mod, "poll_api", fake_poll)
    assert "pl" not in _run(mod.poll_active_payload(PlanSelector()))
