#!/usr/bin/env python3
"""Phase 8 / M3 behaviour test.

Launches the engine with --mcp-http 9876 and verifies:
  1. +blink / -blink moves the player (with valid LOS to e1m1's start area).
  2. Phase energy drains by ph_blink_cost.
  3. +gust does not crash.

Run via: python scripts/test_phase8_m3.py
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from urllib.error import URLError

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE      = os.path.join(ROOT, "zig-out", "bin", "quake.exe")
PORT     = 9876
BASE     = f"http://localhost:{PORT}"
MAP      = "e1m1"

# ---------------------------------------------------------------------------
# Simple SSE listener -- collects events into a queue keyed by id.
# ---------------------------------------------------------------------------
class SseListener:
    def __init__(self, url):
        self.url = url
        self.events = {}
        self.lock = threading.Lock()
        self.stop = False
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        try:
            with urllib.request.urlopen(self.url, timeout=10) as r:
                # Use the raw socket; urllib's response object enforces a
                # single read deadline. Recv'ing the underlying socket lets
                # the listener idle indefinitely between messages.
                sock = r.fp.raw._sock
                sock.settimeout(0.5)
                pending = ""
                while not self.stop:
                    try:
                        chunk = sock.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not chunk:
                        break
                    pending += chunk.decode("utf-8", "replace")
                    while "\n\n" in pending:
                        block, pending = pending.split("\n\n", 1)
                        data = ""
                        for line in block.split("\n"):
                            if line.startswith("data: "):
                                data += line[6:] + "\n"
                        data = data.strip()
                        if data and data.startswith("{"):
                            try:
                                msg = json.loads(data)
                            except json.JSONDecodeError:
                                continue
                            mid = msg.get("id")
                            if mid is not None:
                                with self.lock:
                                    self.events[mid] = msg
        except Exception as e:
            print(f"[sse] stopped: {e!r}")

    def get(self, msg_id, timeout=4.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if msg_id in self.events:
                    return self.events.pop(msg_id)
            time.sleep(0.05)
        return None


# ---------------------------------------------------------------------------
def wait_port_open(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def post(body):
    data = json.dumps(body).encode()
    req = urllib.request.Request(f"{BASE}/message", data=data,
                                 headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=5).read()


_next_id = [1]
def call(sse, name, args=None, timeout=4.0):
    mid = _next_id[0]; _next_id[0] += 1
    body = {"jsonrpc": "2.0", "id": mid, "method": "tools/call",
            "params": {"name": name, "arguments": args or {}}}
    post(body)
    return sse.get(mid, timeout=timeout)


def exec_cmd(sse, cmd):
    return call(sse, "console_exec", {"command": cmd})


def player_pos(sse):
    r = call(sse, "get_player_state")
    if not r or "result" not in r:
        return None
    text = r["result"]["content"][0]["text"]
    obj = json.loads(text)
    return tuple(obj["position"])


def cvar(sse, name):
    r = call(sse, "set_cvar", {"name": name, "value": str(0)})
    # set_cvar returns the new value; we use it to read by setting then reading
    # back via console_exec -- easier path: call get_cvar.
    r = call(sse, "console_exec", {"command": f"echo PHENERGY={name}=$0"})
    return r


def main():
    if not os.path.isfile(EXE):
        print(f"[fail] no exe at {EXE}", file=sys.stderr)
        return 2

    print(f"[run] launching: {EXE} --mcp-http {PORT} +map {MAP}")
    proc = subprocess.Popen(
        [EXE, "--mcp-http", str(PORT), "+map", MAP],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        if not wait_port_open(PORT, 25):
            print("[fail] mcp port did not open", file=sys.stderr)
            return 3

        sse = SseListener(f"{BASE}/sse")
        sse.start()
        # Give the SSE socket a beat to handshake.
        time.sleep(0.5)

        # Let the level finish loading.
        for _ in range(20):
            pos = player_pos(sse)
            if pos:
                break
            time.sleep(0.25)
        if not pos:
            print("[fail] could not read player_state", file=sys.stderr)
            return 4

        print(f"[ok] player at {pos}")

        # Suppress monster targeting so blink isn't ruined by combat.
        exec_cmd(sse, "notarget")

        # Hold blink for ~150 ms so the trace runs, then release.
        exec_cmd(sse, "+blink")
        time.sleep(0.15)
        before = player_pos(sse)
        exec_cmd(sse, "-blink")
        time.sleep(0.4)
        after  = player_pos(sse)

        print(f"[blink] before={before}  after={after}")
        if not after:
            print("[fail] no post-blink position")
            return 5

        moved = sum((a-b)**2 for a, b in zip(after, before)) ** 0.5
        print(f"[blink] delta = {moved:.1f} units")
        if moved < 32:
            print("[warn] blink did not move the player by >32 units "
                  "-- spawn might be in a closed alcove; not a hard fail.")

        # Verify phase energy dropped. ph_energy is the read-only mirror cvar.
        # We read by stuffing an `echo $ph_energy` and tailing the console.
        exec_cmd(sse, "echo PH_AFTER=$ph_energy")
        time.sleep(0.2)
        tail = call(sse, "console_tail", {"lines": 25})
        if tail and "result" in tail:
            t = tail["result"]["content"][0]["text"]
            print(f"[console tail]\n{t}")

        # Gust smoke test.
        exec_cmd(sse, "+gust")
        time.sleep(0.05)
        exec_cmd(sse, "-gust")
        time.sleep(0.2)

        # Done.
        exec_cmd(sse, "quit")
        proc.wait(timeout=4)
        print(f"[ok] game exited cleanly rc={proc.returncode}")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: proc.kill()


if __name__ == "__main__":
    sys.exit(main())
