#!/usr/bin/env python3
"""The Claude Code prompt hook: the active plan follows the last prompt.

Covers daemon/clawdmeter-prompt-hook (the hook script), the daemon's
read_last_prompt_dir / PlanSelector / PromptWatcher / poll_active, and
daemon/clawdmeter-hook-install (the settings.json editor).

Run: python -m pytest daemon/tests/test_prompt_hook.py -x -q
"""
import asyncio
import importlib.machinery
import importlib.util
import json
import os
import subprocess
from pathlib import Path
from unittest.mock import AsyncMock, patch

import pytest

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import PlanSelector, PromptWatcher

DAEMON_DIR = Path(__file__).resolve().parents[1]
HOOK = DAEMON_DIR / "clawdmeter-prompt-hook"
A = Path("/cfg/a")
B = Path("/cfg/b")


def _load_installer():
    loader = importlib.machinery.SourceFileLoader(
        "clawdmeter_hook_install", str(DAEMON_DIR / "clawdmeter-hook-install"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


installer = _load_installer()


# ---------------------------------------------------------------------------
# The hook script
# ---------------------------------------------------------------------------

def _run_hook(home, args=(), env_extra=None, stdin=""):
    env = {"HOME": str(home), "PATH": os.environ["PATH"]}
    env.update(env_extra or {})
    r = subprocess.run([str(HOOK), *args], input=stdin, capture_output=True,
                       text=True, env=env, timeout=10)
    out = home / ".config" / "claude-usage-monitor" / "last-prompt"
    return r, (out.read_text() if out.exists() else None)


def test_hook_prints_nothing_and_exits_zero(tmp_path):
    r, _ = _run_hook(tmp_path)
    assert r.returncode == 0
    assert r.stdout == ""   # stdout would be added to the prompt context


def test_hook_prefers_claude_config_dir_env(tmp_path):
    _, out = _run_hook(tmp_path, args=[str(tmp_path / "arg")],
                       env_extra={"CLAUDE_CONFIG_DIR": str(tmp_path / "env") + "/"})
    assert out == f"{tmp_path / 'env'}\n"


def test_hook_uses_argument_without_env(tmp_path):
    _, out = _run_hook(tmp_path, args=["~/.claude-work"])
    assert out == f"{tmp_path / '.claude-work'}\n"


def test_hook_falls_back_to_transcript_path(tmp_path):
    stdin = json.dumps({"session_id": "x", "transcript_path":
                        f"{tmp_path}/.claude-work/projects/-repo/abc.jsonl", "prompt": "hi"})
    _, out = _run_hook(tmp_path, stdin=stdin)
    assert out == f"{tmp_path / '.claude-work'}\n"


def test_hook_defaults_to_dot_claude(tmp_path):
    _, out = _run_hook(tmp_path, stdin="{}")
    assert out == f"{tmp_path / '.claude'}\n"


def test_hook_exits_zero_when_state_dir_unwritable(tmp_path):
    (tmp_path / ".config").write_text("a file, not a dir")
    r, out = _run_hook(tmp_path, args=["/x"])
    assert r.returncode == 0 and r.stdout == "" and out is None


# ---------------------------------------------------------------------------
# read_last_prompt_dir
# ---------------------------------------------------------------------------

def test_last_prompt_absent_is_none():
    assert mod.read_last_prompt_dir() is None


def test_last_prompt_reads_first_line_and_expands_tilde():
    mod.LAST_PROMPT_FILE.write_text("~/.claude-work\n")
    assert mod.read_last_prompt_dir() == Path.home() / ".claude-work"


def test_last_prompt_blank_is_none():
    mod.LAST_PROMPT_FILE.write_text("\n")
    assert mod.read_last_prompt_dir() is None


# ---------------------------------------------------------------------------
# PlanSelector with a prompted dir
# ---------------------------------------------------------------------------

def test_selector_prompted_dir_beats_usage_rise():
    sel = PlanSelector()
    sel.choose({A: 10, B: 10})
    # B's usage rises, but the last prompt was in A.
    assert sel.choose({A: 10, B: 30}, prompted=A) == A
    assert sel.reason == "last prompt"


def test_selector_prompted_dir_matches_through_trailing_slash(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir(); b.mkdir()
    assert PlanSelector().choose({a: 5, b: 50}, prompted=Path(f"{a}/")) == a


def test_selector_unconfigured_prompted_dir_falls_back_to_usage():
    sel = PlanSelector()
    assert sel.choose({A: 10, B: 40}, prompted=Path("/cfg/other")) == B
    assert sel.reason == "usage"


def test_selector_tracks_rises_while_prompt_wins():
    # Rise history still accumulates, so removing the hook falls back cleanly.
    sel = PlanSelector()
    sel.choose({A: 10, B: 10}, prompted=A)
    sel.choose({A: 10, B: 20}, prompted=A)
    assert sel.choose({A: 10, B: 20}) == B


# ---------------------------------------------------------------------------
# poll_active: prompt selection and the switch chime guard
# ---------------------------------------------------------------------------

def _poll(sel, monkeypatch, util):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: "tB"}[d])

    async def fake_poll(token):
        return {"s": util[token], "c": 1, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload, _dead = asyncio.run(mod.poll_active(sel))
    return payload


def test_poll_active_follows_last_prompt(monkeypatch):
    mod.LAST_PROMPT_FILE.write_text(f"{A}\n")
    payload = _poll(PlanSelector(), monkeypatch, {"tA": 5, "tB": 80})
    assert payload["s"] == 5


def test_poll_active_drops_chime_on_plan_switch(monkeypatch):
    sel = PlanSelector()
    mod.LAST_PROMPT_FILE.write_text(f"{B}\n")
    assert _poll(sel, monkeypatch, {"tA": 5, "tB": 80}).get("c") == 1   # first send
    mod.LAST_PROMPT_FILE.write_text(f"{A}\n")
    switched = _poll(sel, monkeypatch, {"tA": 5, "tB": 80})
    assert switched["s"] == 5 and "c" not in switched
    assert _poll(sel, monkeypatch, {"tA": 5, "tB": 80}).get("c") == 1   # same plan again


def test_poll_active_single_dir_ignores_prompt_file(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A])
    monkeypatch.setattr(mod, "read_token_for", lambda d: "tA")
    mod.LAST_PROMPT_FILE.write_text("/somewhere/else\n")
    with patch.object(mod, "poll_api", new=AsyncMock(return_value={"s": 7, "ok": True})):
        payload, _ = asyncio.run(mod.poll_active(PlanSelector()))
    assert payload["s"] == 7


# ---------------------------------------------------------------------------
# PromptWatcher
# ---------------------------------------------------------------------------

def _touch_prompt(d: Path, bump: int):
    mod.LAST_PROMPT_FILE.write_text(f"{d}\n")
    st = mod.LAST_PROMPT_FILE.stat()
    os.utime(mod.LAST_PROMPT_FILE, ns=(st.st_atime_ns, st.st_mtime_ns + bump * 10**9))


def test_watcher_flags_prompt_in_other_plan(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    sel = PlanSelector()
    sel.choose({A: 1, B: 50})          # showing B
    w = PromptWatcher(sel)
    assert not w.switch_pending()      # nothing new yet
    _touch_prompt(A, 1)
    assert w.switch_pending()
    assert not w.switch_pending()      # consumed


def test_watcher_ignores_prompt_in_shown_plan(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    sel = PlanSelector()
    sel.choose({A: 1, B: 50})
    w = PromptWatcher(sel)
    _touch_prompt(B, 1)
    assert not w.switch_pending()


def test_watcher_ignores_prompts_from_before_startup(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    _touch_prompt(A, 0)
    sel = PlanSelector()
    sel.choose({A: 1, B: 50})
    assert not PromptWatcher(sel).switch_pending()


def test_watcher_idle_with_single_dir(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A])
    w = PromptWatcher(PlanSelector())
    _touch_prompt(B, 1)
    assert not w.switch_pending()


# ---------------------------------------------------------------------------
# clawdmeter-hook-install
# ---------------------------------------------------------------------------

def _settings(d: Path) -> dict:
    return json.loads((d / "settings.json").read_text())


def test_install_creates_settings_with_hook(tmp_path):
    assert installer.main([str(tmp_path)]) == 0
    groups = _settings(tmp_path)["hooks"]["UserPromptSubmit"]
    assert len(groups) == 1
    hook = groups[0]["hooks"][0]
    assert hook["type"] == "command" and "clawdmeter-prompt-hook" in hook["command"]


def test_install_writes_through_symlink_once(tmp_path):
    # ~/.claude-work/settings.json -> ~/.claude/settings.json, a common setup.
    main_dir, work_dir = tmp_path / ".claude", tmp_path / ".claude-work"
    main_dir.mkdir(); work_dir.mkdir()
    (main_dir / "settings.json").write_text(json.dumps({"model": "opus"}))
    (work_dir / "settings.json").symlink_to(main_dir / "settings.json")
    assert installer.main([str(main_dir), str(work_dir)]) == 0
    assert (work_dir / "settings.json").is_symlink()
    s = _settings(main_dir)
    assert s["model"] == "opus"
    commands = [h["command"] for g in s["hooks"]["UserPromptSubmit"] for h in g["hooks"]]
    assert len(commands) == 1
    assert installer.main(["--remove", str(work_dir)]) == 0
    assert (work_dir / "settings.json").is_symlink()
    assert _settings(main_dir) == {"model": "opus"}


def test_install_keeps_other_settings_and_is_idempotent(tmp_path):
    other = {"type": "command", "command": "echo mine"}
    (tmp_path / "settings.json").write_text(json.dumps({
        "model": "opus",
        "hooks": {"UserPromptSubmit": [{"hooks": [other]}],
                  "Stop": [{"hooks": [{"type": "command", "command": "say done"}]}]},
    }))
    installer.main([str(tmp_path)])
    installer.main([str(tmp_path)])
    s = _settings(tmp_path)
    assert s["model"] == "opus"
    assert s["hooks"]["Stop"][0]["hooks"][0]["command"] == "say done"
    commands = [h["command"] for g in s["hooks"]["UserPromptSubmit"] for h in g["hooks"]]
    assert commands.count("echo mine") == 1
    assert sum("clawdmeter-prompt-hook" in c for c in commands) == 1


def test_remove_restores_original_settings(tmp_path):
    original = {"model": "opus"}
    (tmp_path / "settings.json").write_text(json.dumps(original))
    installer.main([str(tmp_path)])
    assert installer.main(["--remove", str(tmp_path)]) == 0
    assert _settings(tmp_path) == original


def test_install_refuses_invalid_json(tmp_path):
    (tmp_path / "settings.json").write_text("{ not json")
    assert installer.main([str(tmp_path)]) == 1
    assert (tmp_path / "settings.json").read_text() == "{ not json"


def test_installed_command_runs(tmp_path):
    cfg = tmp_path / "cfg dir"          # a space in the path must survive quoting
    installer.main([str(cfg)])
    command = _settings(cfg)["hooks"]["UserPromptSubmit"][0]["hooks"][0]["command"]
    env = {"HOME": str(tmp_path), "PATH": os.environ["PATH"], "CLAUDE_CONFIG_DIR": str(cfg)}
    r = subprocess.run(command, shell=True, input="{}", capture_output=True, text=True, env=env)
    assert r.returncode == 0 and r.stdout == ""
    out = tmp_path / ".config" / "claude-usage-monitor" / "last-prompt"
    assert out.read_text() == f"{cfg}\n"
