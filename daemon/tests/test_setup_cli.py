#!/usr/bin/env python3
"""Tests for daemon/clawdmeter_setup.py (pure parts; BLE is covered by the
hardware check in the plan).

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_setup_cli.py -x -q
"""
import asyncio
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


def test_add_device_pipes_password_via_stdin_dedupes_devices_and_hups_broker(tmp_path):
    # The password must never be an argv element: any local user can read a
    # running command line with ps.
    log = tmp_path / "calls"
    fake = tmp_path / "mosquitto_passwd"
    fake.write_text(f'#!/bin/sh\necho "ARGS $@" >> "{log}"\ncat >> "{log}"\n')
    fake.chmod(0o755)
    (tmp_path / "devices").write_text("a1b2c3\n")
    broker = subprocess.Popen(["sleep", "30"])
    (tmp_path / "broker.pid").write_text(str(broker.pid))

    s.add_device(tmp_path, "a1b2c3", "pw1", str(fake))
    s.add_device(tmp_path, "ffffff", "pw2", str(fake))

    assert log.read_text().splitlines() == [
        f"ARGS {tmp_path}/passwd a1b2c3", "pw1", "pw1",
        f"ARGS {tmp_path}/passwd ffffff", "pw2", "pw2",
    ]
    assert (tmp_path / "devices").read_text().splitlines() == ["a1b2c3", "ffffff"]
    assert broker.wait(timeout=5) != 0        # SIGHUP ended `sleep`


def test_interpret_begin_reply():
    assert s.interpret_begin_reply({"id": "a1b2c3"}) == ("a1b2c3", None)
    dev, msg = s.interpret_begin_reply({"err": "nowifi"})
    assert dev is None and "no WiFi" in msg
    dev, msg = s.interpret_begin_reply({"err": "locked"})
    assert dev is None and "wait a minute" in msg.lower()
    dev, msg = s.interpret_begin_reply({"id": "../etc"})
    assert dev is None and "Unexpected reply" in msg


def test_broker_reachable():
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    assert s.broker_reachable(port)
    srv.close()
    assert not s.broker_reachable(port)


# ---------------------------------------------------------------- provision flow

class FakeLink:
    """Scripted PROV replies; records what the CLI sent."""
    replies: list = []
    sent: list = []

    def __init__(self, _client):
        pass

    async def start(self):
        pass

    async def send(self, msg):
        FakeLink.sent.append(msg)

    async def recv(self, _timeout):
        if not FakeLink.replies:
            raise asyncio.TimeoutError
        return FakeLink.replies.pop(0)


class FakeClient:
    def __init__(self, _target):
        pass

    async def connect(self):
        pass

    async def disconnect(self):
        pass


def _provision(monkeypatch, tmp_path, replies, codes):
    import argparse
    FakeLink.replies = list(replies)
    FakeLink.sent = []
    added = []
    monkeypatch.setattr(s, "ProvLink", FakeLink)
    monkeypatch.setattr(s, "BleakClient", FakeClient)
    monkeypatch.setattr(s.mqtt_transport, "read_broker_env",
                        lambda *_a: {"BROKER_ID": "00aa11bb22cc33dd", "BROKER_PORT": "1", "DAEMON_PASS": "x"})
    monkeypatch.setattr(s.mqtt_transport, "find_tool", lambda _n: "/bin/true")
    monkeypatch.setattr(s, "broker_reachable", lambda _p: True)
    monkeypatch.setattr(s, "add_device", lambda *a: added.append(a[1]))
    monkeypatch.setattr(s, "upsert_config_key", lambda *a: None)
    monkeypatch.setattr(s, "restart_daemon", lambda: True)
    monkeypatch.setattr(s, "NET_UP_TIMEOUT", 0.01)

    async def fake_target(*_a, **_k):
        return "AA:BB:CC:DD:EE:FF"
    monkeypatch.setattr(s.daemon_core, "discover_target", fake_target)
    monkeypatch.setattr(s.getpass, "getpass", lambda _p: "wifipass")
    answers = iter(codes)

    def fake_input(_p):
        try:
            return next(answers)
        except StopIteration:
            raise KeyboardInterrupt
    monkeypatch.setattr("builtins.input", fake_input)
    try:
        rc = asyncio.run(s.provision(argparse.Namespace(ssid="Home")))
    except KeyboardInterrupt:
        rc = 130
    return rc, added


def test_aborted_rerun_leaves_working_device_password_alone(monkeypatch, tmp_path):
    # Wrong code, then the user quits: the device's broker password must not
    # have been replaced, or a device that was working stops connecting.
    rc, added = _provision(monkeypatch, tmp_path,
                           [{"id": "a1b2c3"}, {"err": "code"}], ["111111"])
    assert rc != 0
    assert added == []


def test_device_added_to_broker_after_code_accepted(monkeypatch, tmp_path):
    rc, added = _provision(monkeypatch, tmp_path,
                           [{"id": "a1b2c3"}, {"ok": True}, {"net": "up"}], ["123456"])
    assert rc == 0
    assert added == ["a1b2c3"]
