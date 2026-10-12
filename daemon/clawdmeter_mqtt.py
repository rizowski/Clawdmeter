"""MQTT transport for the Clawdmeter daemon (macOS / Linux Python daemon).

The daemon runs a local mosquitto (state from daemon/broker/broker-init.sh),
announces it over mDNS as _clawdmeter._tcp, publishes usage JSON retained to
clawdmeter/<id>/usage, and hears refresh requests on clawdmeter/<id>/refresh.
Security model: password auth + ACL, no TLS (see the design spec).
"""

import asyncio
import json
import os
import re
import shutil
import socket
import time
from pathlib import Path
from typing import Callable

BROKER_DIR = Path.home() / ".config" / "claude-usage-monitor" / "broker"
SERVICE_TYPE = "_clawdmeter._tcp.local."
TOPIC_PREFIX = "clawdmeter"
_SBIN_FALLBACKS = ("/opt/homebrew/sbin", "/usr/local/sbin", "/usr/sbin")
_REQUIRED_ENV = {"BROKER_ID", "BROKER_PORT", "DAEMON_PASS"}


def find_tool(name: str) -> str | None:
    """PATH lookup with an sbin fallback — Homebrew installs `mosquitto` in sbin,
    which isn't on PATH under launchd."""
    path = shutil.which(name)
    if path:
        return path
    for d in _SBIN_FALLBACKS:
        cand = Path(d) / name
        if cand.is_file() and os.access(cand, os.X_OK):
            return str(cand)
    return None


def read_broker_env(broker_dir: Path | None = None) -> dict | None:
    f = (broker_dir or BROKER_DIR) / "broker.env"
    if not f.exists():
        return None
    out = {}
    for line in f.read_text().splitlines():
        if "=" in line:
            key, val = line.split("=", 1)
            out[key.strip()] = val.strip()
    return out if _REQUIRED_ENV <= out.keys() else None


def read_devices(broker_dir: Path | None = None) -> list[str]:
    f = (broker_dir or BROKER_DIR) / "devices"
    if not f.exists():
        return []
    return [ln.strip() for ln in f.read_text().splitlines() if re.fullmatch(r"[0-9a-f]{6}", ln.strip())]


def topic(device_id: str, leaf: str) -> str:
    return f"{TOPIC_PREFIX}/{device_id}/{leaf}"


def device_id_from_topic(t: str) -> str | None:
    parts = t.split("/")
    if len(parts) == 3 and parts[0] == TOPIC_PREFIX:
        return parts[1]
    return None


async def _sleep_or_stop(stop_event: asyncio.Event, seconds: float) -> None:
    try:
        await asyncio.wait_for(stop_event.wait(), timeout=seconds)
    except asyncio.TimeoutError:
        pass


class BrokerSupervisor:
    """Keeps one mosquitto child alive; restarts it with backoff if it exits."""

    def __init__(self, broker_dir: Path | None = None, log: Callable[[str], None] = print) -> None:
        self.dir = broker_dir or BROKER_DIR
        self.log = log

    async def run(self, stop_event: asyncio.Event) -> None:
        backoff = 1
        pid_file = self.dir / "broker.pid"
        while not stop_event.is_set():
            exe = find_tool("mosquitto")
            if exe is None:
                self.log("mosquitto not installed; WiFi devices can't connect")
                return
            proc = await asyncio.create_subprocess_exec(exe, "-c", str(self.dir / "mosquitto.conf"))
            pid_file.write_text(str(proc.pid))
            self.log(f"Broker started (pid={proc.pid})")
            started = time.monotonic()
            waiter = asyncio.ensure_future(proc.wait())
            stopper = asyncio.ensure_future(stop_event.wait())
            done, _ = await asyncio.wait({waiter, stopper}, return_when=asyncio.FIRST_COMPLETED)
            if stopper in done:
                proc.terminate()
                await waiter
                pid_file.unlink(missing_ok=True)
                return
            stopper.cancel()
            if time.monotonic() - started > 60:
                backoff = 1
            self.log(f"Broker exited (code {proc.returncode}); restarting in {backoff}s")
            await _sleep_or_stop(stop_event, backoff)
            backoff = min(backoff * 2, 60)
        pid_file.unlink(missing_ok=True)


def lan_ipv4_addresses() -> list[str]:
    """The default-route IPv4 address, as a one-item list (empty when offline).

    Only this one is announced: the device connects to the FIRST address in
    the mDNS answer, and announcing every interface put VPN / Tailscale /
    container addresses (e.g. 10.244.x) ahead of the LAN one. A UDP
    connect() sends no packets; it only asks the kernel which source address
    the default route would use.
    """
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("192.0.2.1", 9))   # TEST-NET-1, never actually contacted
            addr = sock.getsockname()[0]
    except OSError:
        return []
    if addr.startswith(("127.", "169.254.", "0.")):
        return []
    return [addr]


class Announcer:
    """Advertises the broker as _clawdmeter._tcp with TXT id=<broker_id>, and
    re-announces when this machine's LAN addresses change."""

    def __init__(self, broker_id: str, port: int, log: Callable[[str], None] = print,
                 zc_factory: Callable | None = None, check_s: float = 15) -> None:
        self.broker_id = broker_id
        self.port = port
        self.log = log
        self.host = socket.gethostname().split(".")[0]
        self.zc_factory = zc_factory
        self.check_s = check_s

    def _info(self, addrs: list[str]):
        from zeroconf import ServiceInfo
        return ServiceInfo(
            SERVICE_TYPE,
            f"Clawdmeter broker on {self.host}.{SERVICE_TYPE}",
            port=self.port,
            properties={"id": self.broker_id},
            server=f"clawdmeter-broker-{self.host}.local.",
            parsed_addresses=addrs,
        )

    def _new_zeroconf(self):
        if self.zc_factory is not None:
            return self.zc_factory()
        from zeroconf import IPVersion
        from zeroconf.asyncio import AsyncZeroconf
        return AsyncZeroconf(ip_version=IPVersion.V4Only)

    async def run(self, stop_event: asyncio.Event) -> None:
        addrs: list[str] = []
        azc = None
        try:
            while not stop_event.is_set():
                new = lan_ipv4_addresses()
                if new != addrs:
                    # A fresh instance per address set: zeroconf binds its
                    # sockets per interface address at start, so after a WiFi
                    # rejoin with a new IP the old one can stop answering.
                    if azc is not None:
                        await azc.async_unregister_all_services()
                        await azc.async_close()
                        azc = None
                    addrs = new
                    if addrs:
                        azc = self._new_zeroconf()
                        await azc.async_register_service(self._info(addrs))
                        self.log(f"mDNS: announced broker on {addrs[0]}:{self.port}")
                    else:
                        self.log("mDNS: no network; waiting")
                await _sleep_or_stop(stop_event, self.check_s)
        finally:
            if azc is not None:
                await azc.async_unregister_all_services()
                await azc.async_close()


class Publisher:
    """paho-mqtt client (own network thread) logged in as the daemon user."""

    def __init__(self, env: dict, loop: asyncio.AbstractEventLoop,
                 on_refresh: Callable[[str], None], log: Callable[[str], None] = print) -> None:
        import paho.mqtt.client as mqtt
        self._mqtt = mqtt
        self.loop = loop
        self.on_refresh = on_refresh
        self.log = log
        self.port = int(env["BROKER_PORT"])
        self._c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                              client_id=f"clawdmeter-daemon-{socket.gethostname()}")
        self._c.username_pw_set("daemon", env["DAEMON_PASS"])
        self._c.on_connect = self._on_connect
        self._c.on_message = self._on_message
        self._c.reconnect_delay_set(1, 30)

    def start(self) -> None:
        self._c.connect_async("127.0.0.1", self.port, keepalive=30)
        self._c.loop_start()

    def stop(self) -> None:
        self._c.disconnect()
        self._c.loop_stop()

    def is_connected(self) -> bool:
        return self._c.is_connected()

    def _on_connect(self, client, _userdata, _flags, reason_code, _props=None) -> None:
        if reason_code.is_failure:
            self.log(f"MQTT: local broker refused login ({reason_code})")
            return
        client.subscribe(topic("+", "refresh"), qos=1)
        self.log("MQTT: connected to local broker")

    def _on_message(self, _client, _userdata, msg) -> None:
        dev = device_id_from_topic(msg.topic)
        if dev:
            self.loop.call_soon_threadsafe(self.on_refresh, dev)

    def publish_usage(self, device_ids: list[str], payload: dict) -> bool:
        if not self._c.is_connected():
            return False
        data = json.dumps(payload, separators=(",", ":"))
        ok = True
        for dev in device_ids:
            info = self._c.publish(topic(dev, "usage"), data, qos=1, retain=True)
            ok = ok and info.rc == self._mqtt.MQTT_ERR_SUCCESS
        return ok
