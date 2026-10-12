#!/usr/bin/env python3
"""clawdmeter-setup: put a Clawdmeter on WiFi + the daemon's MQTT broker.

Run it on the machine that runs the daemon, with the device already paired
over Bluetooth and the daemon running (it hosts the broker). The device shows
a 6-digit code; you type it here; we push WiFi + broker details over the
encrypted BLE link and wait until the device reports it reached the broker.
"""

import argparse
import asyncio
import getpass
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
from pathlib import Path

from bleak import BleakClient
from bleak.exc import BleakError

try:  # run as a script (sys.path[0] == daemon/) or imported as daemon.* by tests
    import claude_usage_daemon as daemon_core
    import clawdmeter_mqtt as mqtt_transport
except ImportError:
    from daemon import claude_usage_daemon as daemon_core
    from daemon import clawdmeter_mqtt as mqtt_transport

PROV_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"
REPLY_TIMEOUT = 5.0
NET_UP_TIMEOUT = 45.0
MAX_CODE_TRIES = 5


def parse_nmcli_ssid(out: str) -> str | None:
    for line in out.splitlines():
        if line.startswith("yes:"):
            return line[4:].replace("\\:", ":") or None
    return None


def parse_networksetup_ssid(out: str) -> str | None:
    m = re.match(r"Current Wi-Fi Network: (.+)$", out.strip())
    return m.group(1) if m else None


def detect_ssid() -> str | None:
    try:
        if sys.platform.startswith("linux"):
            out = subprocess.run(["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"],
                                 capture_output=True, text=True, timeout=5).stdout
            return parse_nmcli_ssid(out)
        if sys.platform == "darwin":
            out = subprocess.run(["networksetup", "-getairportnetwork", "en0"],
                                 capture_output=True, text=True, timeout=5).stdout
            return parse_networksetup_ssid(out)
    except (OSError, subprocess.TimeoutExpired):
        pass
    return None


def build_config_message(code: str, ssid: str, wpass: str, broker_id: str, password: str) -> bytes:
    # Same limits the firmware enforces (netcfg.h) — fail here with a clear
    # message instead of a bare {"err":"json"} from the device.
    if not 1 <= len(ssid.encode()) <= 32:
        raise ValueError("WiFi network name must be 1-32 bytes")
    if len(wpass.encode()) > 64:
        raise ValueError("WiFi password must be at most 64 characters")
    msg = json.dumps({"op": "config", "code": code, "ssid": ssid, "wpass": wpass,
                      "broker_id": broker_id, "pass": password}, separators=(",", ":")).encode()
    if len(msg) > 500:
        raise ValueError("config too long for one BLE write")
    return msg


def add_device(broker_dir: Path, device_id: str, password: str, passwd_tool: str) -> None:
    # Password via stdin (mosquitto_passwd prompts twice), never argv: any
    # local user can read a running command line with ps.
    subprocess.run([passwd_tool, str(broker_dir / "passwd"), device_id],
                   input=f"{password}\n{password}\n", text=True, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    devices = broker_dir / "devices"
    known = devices.read_text().split() if devices.exists() else []
    if device_id not in known:
        with devices.open("a") as f:
            f.write(device_id + "\n")
    pid_file = broker_dir / "broker.pid"
    if pid_file.exists():
        try:
            os.kill(int(pid_file.read_text().strip()), signal.SIGHUP)  # reload passwd
        except (ValueError, ProcessLookupError, PermissionError):
            pass


def upsert_config_key(path: Path, key: str, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = path.read_text().splitlines() if path.exists() else []
    kept = [ln for ln in lines if not re.match(rf"\s*{re.escape(key)}\s*=", ln)]
    kept.append(f"{key} = {value}")
    path.write_text("\n".join(kept) + "\n")


def broker_reachable(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def restart_daemon() -> bool:
    if sys.platform == "darwin":
        cmd = ["launchctl", "kickstart", "-k", f"gui/{os.getuid()}/com.user.claude-usage-daemon"]
    else:
        cmd = ["systemctl", "--user", "restart", "claude-usage-daemon"]
    try:
        return subprocess.run(cmd, capture_output=True, timeout=15).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def interpret_begin_reply(reply: dict) -> tuple[str | None, str | None]:
    """(device_id, None) on success, else (None, message to show the user)."""
    err = reply.get("err")
    if err == "nowifi":
        return None, "This board has no WiFi support — it keeps using Bluetooth."
    if err == "locked":
        return None, "Too many wrong codes. Wait a minute, then run clawdmeter-setup again."
    device_id = reply.get("id", "")
    if not re.fullmatch(r"[0-9a-f]{6}", device_id):
        return None, f"Unexpected reply from device: {reply}"
    return device_id, None


class ProvLink:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.replies: asyncio.Queue = asyncio.Queue()

    def _on_notify(self, _char, data: bytearray) -> None:
        try:
            self.replies.put_nowait(json.loads(bytes(data).decode()))
        except (ValueError, UnicodeDecodeError):
            self.replies.put_nowait({"err": "garbled"})

    async def start(self) -> None:
        await self.client.start_notify(PROV_CHAR_UUID, self._on_notify)

    async def send(self, msg) -> None:
        data = msg if isinstance(msg, bytes) else json.dumps(msg, separators=(",", ":")).encode()
        await self.client.write_gatt_char(PROV_CHAR_UUID, data, response=True)

    async def recv(self, timeout: float) -> dict:
        return await asyncio.wait_for(self.replies.get(), timeout)


def prompt_default(label: str, default: str | None) -> str:
    while True:
        ans = input(f"{label}" + (f" [{default}]" if default else "") + ": ").strip()
        if ans or default:
            return ans or default


async def provision(args) -> int:
    env = mqtt_transport.read_broker_env()
    if env is None:
        print("No broker set up yet. Re-run the installer (it runs daemon/broker/broker-init.sh).")
        return 1
    if not broker_reachable(int(env["BROKER_PORT"])):
        print("The broker isn't running. Start the daemon first — it hosts the broker.")
        return 1
    passwd_tool = mqtt_transport.find_tool("mosquitto_passwd")
    if passwd_tool is None:
        print("mosquitto_passwd not found — install mosquitto.")
        return 1

    target = await daemon_core.discover_target()
    if target is None:
        print("Clawdmeter not found. Pair it in Bluetooth settings first and make sure it's on.")
        return 1

    client = BleakClient(target)
    try:
        await asyncio.wait_for(client.connect(), timeout=daemon_core.CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        print(f"Could not connect over Bluetooth: {e}")
        return 1
    try:
        link = ProvLink(client)
        await link.start()
        await link.send({"op": "begin"})
        device_id, problem = interpret_begin_reply(await link.recv(REPLY_TIMEOUT))
        if problem:
            print(problem)
            return 1
        print(f"Device {device_id} is now showing a 6-digit setup code (valid 2 minutes).")

        ssid = args.ssid or prompt_default("WiFi network", detect_ssid())
        wpass = getpass.getpass(f"Password for {ssid} (blank if open): ")
        password = secrets.token_urlsafe(18)   # 24 chars
        add_device(mqtt_transport.BROKER_DIR, device_id, password, passwd_tool)

        for _ in range(MAX_CODE_TRIES):
            code = input("Code shown on the device: ").strip().replace(" ", "")
            if not re.fullmatch(r"\d{6}", code):
                print("Enter the 6 digits shown on the screen.")
                continue
            try:
                await link.send(build_config_message(code, ssid, wpass, env["BROKER_ID"], password))
            except ValueError as e:
                print(e)
                return 1
            reply = await link.recv(REPLY_TIMEOUT)
            if reply.get("ok"):
                break
            err = reply.get("err")
            if err == "code":
                print("Wrong code, try again.")
                continue
            if err in ("locked", "expired"):
                print("The code expired or had too many wrong tries. Run clawdmeter-setup again.")
                return 1
            print(f"Device rejected the config: {reply}")
            return 1
        else:
            return 1

        print("Saved. Waiting for the device to join WiFi and reach the broker...")
        try:
            while (await link.recv(NET_UP_TIMEOUT)).get("net") != "up":
                pass
        except asyncio.TimeoutError:
            print(f"The device did not reach the broker within {int(NET_UP_TIMEOUT)} s.\n"
                  f"  - Check the password for \"{ssid}\" and run clawdmeter-setup again.\n"
                  "  - macOS: allow incoming connections for mosquitto if the firewall asks.\n"
                  "The device keeps working over Bluetooth meanwhile.")
            return 1
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    upsert_config_key(daemon_core.CONFIG_FILE, "transport", "mqtt")
    print("Connected over WiFi. Switching the daemon to MQTT...")
    print("Done." if restart_daemon() else "Restart the daemon to finish switching to MQTT.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Set up a Clawdmeter for WiFi + MQTT.")
    ap.add_argument("--ssid", help="WiFi network name (default: the one this machine is on)")
    try:
        return asyncio.run(provision(ap.parse_args()))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
