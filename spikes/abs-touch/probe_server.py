#!/usr/bin/env python3
"""Serves probe.html and appends what the page reports to events.jsonl, for reading results on the Mac.

Usage: probe_server.py [port]  (default 8000), then open http://<Mac LAN IP>:<port>/ in Safari on the phone.
"""
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / 'events.jsonl'


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.split('?')[0] not in ('/', '/probe.html'):
            return self.send_error(404)
        body = (HERE / 'probe.html').read_bytes()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        raw = self.rfile.read(min(int(self.headers.get('Content-Length', 0)), 4096))
        try:
            entry = json.loads(raw)
        except ValueError:
            return self.send_error(400)
        entry['at'] = time.strftime('%H:%M:%S')
        with OUT.open('a') as f:
            f.write(json.dumps(entry) + '\n')
        if entry.get('kind') != 'alive':
            print(entry, flush=True)
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
ThreadingHTTPServer(('0.0.0.0', port), Handler).serve_forever()
