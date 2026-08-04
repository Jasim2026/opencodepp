#!/usr/bin/env python3
"""py_binding_test.py -- Phase 12 commit 4: Python binding vs a live mock.

Hosts a stdlib-only HTTP SSE mock (http.server) speaking OpenAI-compat
chat.completion.chunk frames, then drives a full session through
bindings/python/opencode.py and asserts the run completes.

Usage: OPENCODE_LIB=/path/to/libopencodepp.so python3 py_binding_test.py
Exits 0 on success, 1 on failure.
"""

import os
import sys
import tempfile
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
import opencode  # noqa: E402

FRAMES = [
    # round 0: file.write tool call
    b'{"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_py1",'
    b'"function":{"name":"file.write","arguments":"{\\"path\\":\\"py.txt\\",'
    b'\\"content\\":\\"hello python\\\\n\\",\\"create\\":true}"}}]},'
    b'"finish_reason":"tool_calls"}]}',
    b'{"usage":{"prompt_tokens":20,"completion_tokens":8}}',
    # round 1: final text + stop
    b'{"choices":[{"delta":{"content":"wrote py.txt"},"finish_reason":null}]}',
    b'{"choices":[{"delta":{},"finish_reason":"stop"}]}',
    b'{"usage":{"prompt_tokens":40,"completion_tokens":2}}',
]

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer  # noqa: E402


class Handler(BaseHTTPRequestHandler):
    rounds = 0

    def do_POST(self):  # noqa: N802
        if self.path != "/chat/completions":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        start = Handler.rounds
        Handler.rounds += 1
        if start == 0:
            chunk = b"\n\n".join([b"data: " + f for f in FRAMES[:2]])
        else:
            chunk = b"\n\n".join([b"data: " + f for f in FRAMES[2:]])
        body = chunk + b"\n\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # silence the default logger
        pass


class Mock:
    def __init__(self):
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.port = self.httpd.server_address[1]

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join()


def main():
    failures = 0

    def check(cond, label):
        nonlocal failures
        if cond:
            print("  %s: OK" % label)
        else:
            print("  %s: FAIL" % label)
            failures += 1

    mock = Mock()
    ws = tempfile.mkdtemp(prefix="opencode_py_ws_")

    cfg = opencode.Config(workspace=ws,
                          base_url="http://127.0.0.1:%d" % mock.port,
                          tool_policy=opencode.Policy.ALLOW)
    eng = opencode.Engine(cfg)
    check(eng.valid(), "engine create")

    events = []
    res = eng.run("write py.txt", on_event=lambda ev: events.append(ev))
    check(res.status == opencode.Status.OK, "run status ok (got %s)"
          % res.status_name)
    check(res.saw_done and not res.saw_failed, "saw DONE, no FAILED")
    kinds = [ev.kind for ev in events]
    check(opencode.EventKind.PREPARING in kinds, "PREPARING event seen")
    check(opencode.EventKind.DONE in kinds, "DONE event seen")

    out_path = os.path.join(ws, "py.txt")
    check(os.path.isfile(out_path), "file.write applied")
    if os.path.isfile(out_path):
        with open(out_path, "rb") as f:
            check(f.read() == b"hello python\n", "file content")

    metrics = eng.metrics()
    check(metrics >= 1, "metrics snapshot emitted %d" % metrics)

    mem_id = eng.memory_write(opencode.MemoryKind.FACT, "py_key", "py_value",
                              tags=["py"])
    check(mem_id is not None, "memory_write returned id")
    ctx = eng.memory_read(opencode.MemoryKind.FACT)
    check("py_key" in ctx and "py_value" in ctx, "memory_read round-trip")

    bad = opencode.Engine(None)
    rc = bad.set_config(None)
    check(rc == opencode.Status.VALIDATION, "set_config(NULL) -> VALIDATION")

    mock.close()
    print("py_binding_test: %s" % ("all sections OK" if failures == 0
                                   else "%d failure(s)" % failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
