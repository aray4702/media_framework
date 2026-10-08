#!/usr/bin/env python3
"""Serves the repository root with the cross-origin isolation headers the web platform needs
(SharedArrayBuffer for the AudioWorklet's shared memory). Usage: serve.py [port] (default 8000),
then open http://localhost:8000/platform/web/app/"""

import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm", ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
