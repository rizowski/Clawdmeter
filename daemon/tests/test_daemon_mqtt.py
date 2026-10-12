#!/usr/bin/env python3
"""Tests for the daemon's MQTT mode (transport switch + run_mqtt loop).

Run: daemon/.venv/bin/python -m pytest daemon/tests/test_daemon_mqtt.py -x -q
"""
import asyncio

import daemon.claude_usage_daemon as mod


def test_transport_defaults_to_ble(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    assert mod.read_transport_setting() == "ble"


def test_transport_reads_mqtt(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("chime = on\ntransport = MQTT  # set by clawdmeter-setup\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert mod.read_transport_setting() == "mqtt"


def test_transport_ignores_unknown_values(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("transport = carrier-pigeon\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert mod.read_transport_setting() == "ble"


class FakePublisher:
    instances = []

    def __init__(self, env, loop, on_refresh, log=print):
        self.on_refresh = on_refresh
        self.published = []
        self.stopped = False
        FakePublisher.instances.append(self)

    def start(self):
        pass

    def stop(self):
        self.stopped = True

    def publish_usage(self, device_ids, payload):
        self.published.append((list(device_ids), payload))
        return True


def _run_until(stop_after_publishes, payloads, monkeypatch, devices=("a1b2c3",)):
    FakePublisher.instances.clear()
    monkeypatch.setattr(mod.mqtt_transport, "Publisher", FakePublisher)
    monkeypatch.setattr(mod.mqtt_transport, "read_devices", lambda *_a: list(devices))
    queue = list(payloads)

    async def fake_poll_active(*_a):
        return queue.pop(0) if queue else (None, False)

    monkeypatch.setattr(mod, "poll_active", fake_poll_active)
    monkeypatch.setattr(mod, "TICK", 0.05)

    async def scenario():
        stop = asyncio.Event()
        task = asyncio.create_task(mod.run_mqtt({"BROKER_PORT": "1", "DAEMON_PASS": "x"}, stop))
        for _ in range(100):
            await asyncio.sleep(0.02)
            pubs = FakePublisher.instances[0].published if FakePublisher.instances else []
            if len(pubs) >= stop_after_publishes:
                break
            if FakePublisher.instances and len(pubs) == 1 and stop_after_publishes == 2:
                FakePublisher.instances[0].on_refresh("a1b2c3")   # device asks for fresh data
        stop.set()
        await asyncio.wait_for(task, 2)

    asyncio.run(scenario())
    return FakePublisher.instances[0]


def test_run_mqtt_publishes_payload_to_every_device(monkeypatch):
    pub = _run_until(1, [({"s": 5, "ok": True}, False)], monkeypatch, devices=("a1b2c3", "ffffff"))
    assert pub.published[0] == (["a1b2c3", "ffffff"], {"s": 5, "ok": True})
    assert pub.stopped


def test_run_mqtt_signals_no_data_when_tokens_dead(monkeypatch):
    pub = _run_until(1, [(None, True)], monkeypatch)
    assert pub.published[0] == (["a1b2c3"], {"ok": False})


def test_run_mqtt_refresh_request_triggers_immediate_poll(monkeypatch):
    pub = _run_until(2, [({"s": 1, "ok": True}, False), ({"s": 2, "ok": True}, False)], monkeypatch)
    assert [p for _d, p in pub.published] == [{"s": 1, "ok": True}, {"s": 2, "ok": True}]
