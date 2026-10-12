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


def test_announcer_advertises_broker_id_over_mdns():
    from zeroconf import IPVersion, ServiceStateChange
    from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

    async def scenario():
        stop = asyncio.Event()
        task = asyncio.create_task(m.Announcer("00aa11bb22cc33dd", 18935, log=lambda _s: None).run(stop))
        azc = AsyncZeroconf(ip_version=IPVersion.V4Only)
        found = asyncio.Queue()

        def on_change(zeroconf, service_type, name, state_change):
            if state_change is ServiceStateChange.Added:
                found.put_nowait(name)

        browser = AsyncServiceBrowser(azc.zeroconf, m.SERVICE_TYPE, handlers=[on_change])
        txt_id = port = None
        try:
            while txt_id != "00aa11bb22cc33dd":
                name = await asyncio.wait_for(found.get(), 10)
                info = AsyncServiceInfo(m.SERVICE_TYPE, name)
                await info.async_request(azc.zeroconf, 3000)
                txt_id = (info.properties.get(b"id") or b"").decode()
                port = info.port
        finally:
            await browser.async_cancel()
            await azc.async_close()
            stop.set()
            await asyncio.wait_for(task, 5)
        assert port == 18935

    asyncio.run(scenario())


def test_lan_addresses_is_only_the_default_route_address(monkeypatch):
    # The device takes the FIRST IPv4 in the mDNS answer. Announcing every
    # interface let a Tailscale/VPN address come first, so the device never
    # reached the broker. Only the default-route address is announced.
    class FakeSock:
        def __init__(self, *_a):
            pass

        def connect(self, _addr):
            pass

        def getsockname(self):
            return ("192.168.1.20", 5353)

        def close(self):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *_a):
            self.close()

    monkeypatch.setattr(m.socket, "socket", FakeSock)
    assert m.lan_ipv4_addresses() == ["192.168.1.20"]


def test_announcer_rebuilds_zeroconf_when_the_address_changes(monkeypatch):
    # python-zeroconf binds sockets per address at start; after a WiFi rejoin
    # with a new IP the old instance can go deaf, so build a fresh one.
    addrs = iter([["10.0.0.5"], ["10.0.0.5"], ["10.0.0.9"]])
    monkeypatch.setattr(m, "lan_ipv4_addresses", lambda: next(addrs, ["10.0.0.9"]))
    made = []

    class FakeZC:
        def __init__(self):
            self.registered = []
            made.append(self)

        async def async_register_service(self, info):
            self.registered.append(info.parsed_addresses())

        async def async_update_service(self, info):
            self.registered.append(info.parsed_addresses())

        async def async_unregister_all_services(self):
            pass

        async def async_close(self):
            pass

    async def scenario():
        stop = asyncio.Event()
        ann = m.Announcer("00aa11bb22cc33dd", 18830, log=lambda _s: None, zc_factory=FakeZC, check_s=0.01)
        task = asyncio.create_task(ann.run(stop))
        await asyncio.sleep(0.2)
        stop.set()
        await asyncio.wait_for(task, 2)

    asyncio.run(scenario())
    assert len(made) == 2
    assert made[1].registered == [["10.0.0.9"]]
