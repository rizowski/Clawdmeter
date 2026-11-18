"""Shared fixtures for the daemon tests."""
import pytest


@pytest.fixture(autouse=True)
def _isolated_last_prompt(tmp_path, monkeypatch):
    """Keep every test off the real ~/.config/claude-usage-monitor/last-prompt.

    The macOS/Linux daemon reads it on each poll to pick the active plan. The
    file starts absent, which matches a machine without the prompt hook.
    """
    try:
        import daemon.claude_usage_daemon as mod
    except ImportError:   # bleak/httpx missing: those tests skip themselves
        yield
        return
    monkeypatch.setattr(mod, "LAST_PROMPT_FILE", tmp_path / "last-prompt-isolated")
    yield
