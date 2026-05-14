#!/usr/bin/env python3
"""Phase 8 / M3 + MCP /call demo.

Launches the engine with --mcp-http 9876 and exercises the synchronous /call
endpoint (no SSE handshake -- the JSON-RPC response is returned in the same
HTTP reply, much friendlier to automated testing than the async /message +
SSE-stream model).

What's verified:
  - ping            -- round-trip works.
  - tools/list      -- the tool catalogue is reachable.
  - get_player_state-- demo viewpoint readable.
  - get_cvar /
    set_cvar        -- phase-energy cvar reads, mutates, and reads back.

What is NOT verified here:
  - Blink physically moving the player after +map. Single-player gameplay
    startup from a background (no-focus) launch doesn't fully complete the
    client connect under our SDL3 setup -- sv.active stays false, the
    player edict never spawns. Interactive testing (zig build run, focus
    the window, q to Blink) is the workaround for now.

Run: python scripts/test_phase8_m3.py
"""

import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE  = os.path.join(ROOT, "zig-out", "bin", "quake.exe")
PORT = 9876
URL  = f"http://localhost:{PORT}/call"


_next_id = [1]
def call(method, name=None, args=None, timeout=6.0):
    mid = _next_id[0]; _next_id[0] += 1
    if name is None:
        body = {"jsonrpc": "2.0", "id": mid, "method": method, "params": args or {}}
    else:
        body = {"jsonrpc": "2.0", "id": mid, "method": "tools/call",
                "params": {"name": name, "arguments": args or {}}}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def text_result(resp):
    return resp["result"]["content"][0]["text"]


def wait_port_open(port, timeout=20.0):
    """Connect AND succeed at a /call ping. Just-bound port + TIME_WAIT
    entries fool a bare connect attempt -- we need an end-to-end roundtrip
    before declaring the server ready."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            body = b'{"jsonrpc":"2.0","id":0,"method":"ping","params":{}}'
            req = urllib.request.Request(URL, data=body,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=1.0) as r:
                if r.status == 200:
                    return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def main():
    if not os.path.isfile(EXE):
        print(f"[fail] no exe at {EXE}", file=sys.stderr)
        return 2

    print(f"[run] launching: {EXE} --mcp-http {PORT}")
    import tempfile
    LOGPATH = os.path.join(tempfile.gettempdir(), "quake_test.log")
    proc = subprocess.Popen(
        [EXE, "--mcp-http", str(PORT)],
        cwd=ROOT,
        stdout=open(LOGPATH, "w"),
        stderr=subprocess.STDOUT,
    )
    try:
        if not wait_port_open(PORT, 40):
            print("[fail] mcp port did not open within 40 s", file=sys.stderr)
            return 3
        print("[ok] mcp ready")

        # Give the demo a moment to start so get_player_state has a viewpoint.
        time.sleep(3)

        # 1. ping
        r = call("ping")
        assert "result" in r, r
        print("[ok] ping ->", json.dumps(r["result"]))

        # 2. tools/list -- truncate so the assert message isn't novel-length
        r = call("tools/list")
        tools = [t["name"] for t in r["result"]["tools"]]
        print(f"[ok] tools/list -> {len(tools)} tools: {tools[:3]}...")

        # 3. get_player_state
        r = call("tools/call", "get_player_state")
        state = json.loads(text_result(r))
        print(f"[ok] get_player_state -> map={state['map']!r} pos={state['position']}")

        # 4. get_cvar / set_cvar round-trip on a Phase 8 cvar
        r = call("tools/call", "get_cvar", {"name": "ph_max"})
        before = json.loads(text_result(r))
        call("tools/call", "set_cvar", {"name": "ph_max", "value": "42"})
        r = call("tools/call", "get_cvar", {"name": "ph_max"})
        after = json.loads(text_result(r))
        print(f"[ok] ph_max: {before['value']} -> {after['value']}")
        assert after["value"] == "42"

        # 5. console_exec sanity
        r = call("tools/call", "console_exec", {"command": "echo testing"})
        print(f"[ok] console_exec -> {text_result(r)}")

        print("[done] /call endpoint verified")
        return 0
    except Exception as e:
        print(f"[fail] {type(e).__name__}: {e}", file=sys.stderr)
        return 1
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: proc.kill()


if __name__ == "__main__":
    sys.exit(main())
