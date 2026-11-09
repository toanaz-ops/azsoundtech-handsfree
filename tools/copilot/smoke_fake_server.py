#!/usr/bin/env python3
"""Tiny fake OpenAI-compatible server for the build's frozen-bundle smoke gate.

Binds 127.0.0.1 on an ephemeral port, prints that port on stdout, answers ONE
POST (any path ending in /chat/completions) with a fixed harmless Vietnamese
answer plus usage numbers, then exits. Also exits after 30 s with no request.
Stdlib only; never reachable from outside this machine.
"""
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ANSWER = "Đây là câu trả lời thử của máy chủ giả."
TIMEOUT_S = 30


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        body = json.dumps({
            "choices": [{"finish_reason": "stop",
                         "message": {"role": "assistant", "content": ANSWER}}],
            "usage": {"prompt_tokens": 5, "completion_tokens": 3},
        }, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        # shutdown() must not run on the serving thread.
        threading.Thread(target=self.server.shutdown, daemon=True).start()


def main() -> int:
    httpd = HTTPServer(("127.0.0.1", 0), Handler)
    print(httpd.server_address[1], flush=True)
    watchdog = threading.Timer(TIMEOUT_S, httpd.shutdown)
    watchdog.daemon = True
    watchdog.start()
    httpd.serve_forever()
    watchdog.cancel()
    httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
