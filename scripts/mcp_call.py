"""Quick MCP client for the running Quake instance.

Usage examples:
    python scripts/mcp_call.py console_exec '{"command":"r_decals_debug 1"}'
    python scripts/mcp_call.py console_tail '{"lines":40}'
    python scripts/mcp_call.py get_player_state
    python scripts/mcp_call.py list_entities
    python scripts/mcp_call.py screenshot
    python scripts/mcp_call.py wait_frames '{"frames":30}'
    python scripts/mcp_call.py set_cvar '{"name":"r_decals_debug","value":"1"}'

Opens SSE on /sse, POSTs JSON-RPC to /message, reads response events.
"""
import http.client
import json
import sys
import threading
import time
import urllib.parse

HOST = "127.0.0.1"
PORT = 9876

responses = {}
endpoint_path = "/message"
sse_done = threading.Event()


def sse_reader():
    global endpoint_path
    conn = http.client.HTTPConnection(HOST, PORT, timeout=30)
    conn.request("GET", "/sse", headers={"Accept": "text/event-stream"})
    resp = conn.getresponse()
    if resp.status != 200:
        print(f"SSE failed: {resp.status}", file=sys.stderr)
        sse_done.set()
        return
    buf = b""
    while not sse_done.is_set():
        chunk = resp.read(1)
        if not chunk:
            break
        buf += chunk
        while b"\n\n" in buf:
            event, buf = buf.split(b"\n\n", 1)
            event_type = "message"
            data_lines = []
            for line in event.split(b"\n"):
                line = line.decode("utf-8", errors="replace")
                if line.startswith("event: "):
                    event_type = line[7:]
                elif line.startswith("data: "):
                    data_lines.append(line[6:])
            data = "\n".join(data_lines)
            if event_type == "endpoint":
                parsed = urllib.parse.urlparse(data)
                endpoint_path = parsed.path or "/message"
            elif event_type == "message" and data:
                try:
                    obj = json.loads(data)
                    if "id" in obj:
                        responses[obj["id"]] = obj
                except json.JSONDecodeError:
                    print(f"bad json: {data!r}", file=sys.stderr)


def post(req):
    body = json.dumps(req).encode("utf-8")
    conn = http.client.HTTPConnection(HOST, PORT, timeout=10)
    conn.request(
        "POST",
        endpoint_path,
        body=body,
        headers={"Content-Type": "application/json", "Content-Length": str(len(body))},
    )
    resp = conn.getresponse()
    resp.read()
    if resp.status not in (200, 202):
        raise RuntimeError(f"POST failed: {resp.status}")


def call(method, params, msg_id, timeout=10.0):
    post({"jsonrpc": "2.0", "id": msg_id, "method": method, "params": params})
    deadline = time.time() + timeout
    while time.time() < deadline:
        if msg_id in responses:
            return responses.pop(msg_id)
        time.sleep(0.05)
    raise TimeoutError(f"no response to id={msg_id} method={method} within {timeout}s")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    tool = sys.argv[1]
    args_json = sys.argv[2] if len(sys.argv) > 2 else "{}"
    try:
        arguments = json.loads(args_json)
    except json.JSONDecodeError as e:
        print(f"bad args json: {e}", file=sys.stderr)
        sys.exit(1)

    t = threading.Thread(target=sse_reader, daemon=True)
    t.start()

    deadline = time.time() + 5.0
    while endpoint_path == "/message" and time.time() < deadline:
        time.sleep(0.05)

    call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "mcp_call.py", "version": "1"}}, 1)
    post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    result = call("tools/call", {"name": tool, "arguments": arguments}, 2, timeout=15.0)
    print(json.dumps(result, indent=2))

    sse_done.set()


if __name__ == "__main__":
    main()
