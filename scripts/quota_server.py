"""Read-only hotspot /quota endpoint. Authentication stays in the loopback bridge."""
from __future__ import annotations
import argparse
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import time
from urllib.error import URLError
from urllib.request import ProxyHandler, build_opener


def timestamp(value):
    if not isinstance(value, str):
        raise ValueError('missing timestamp')
    date = datetime.fromisoformat(value.replace('Z', '+00:00'))
    if date.tzinfo is None:
        raise ValueError('timestamp must contain a timezone')
    return date.timestamp()


def percent(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 <= value <= 100:
        raise ValueError('invalid percentage')
    return float(value)


def normalize(source, now=None):
    now = time.time() if now is None else now
    if not isinstance(source, dict) or source.get('status') != 'ok' or source.get('limitId') != 'codex':
        raise ValueError('quota unavailable')
    age = now - timestamp(source.get('capturedAt'))
    if not math.isfinite(age) or not -60 <= age <= 180:
        raise ValueError('quota stale')
    windows = {}
    for slot in ('primary', 'secondary'):
        duration = source.get(slot + 'WindowMinutes')
        if isinstance(duration, bool) or duration not in (300, 10080) or duration in windows:
            raise ValueError('invalid window duration')
        remaining = source.get(slot + 'RemainingPercent')
        if remaining is None:
            remaining = 100 - percent(source.get(slot + 'UsedPercent'))
        reset = timestamp(source.get(slot + 'ResetsAt'))
        seconds = max(0, math.ceil(reset - now))
        if seconds > 604800:
            raise ValueError('invalid reset time')
        windows[duration] = (percent(remaining), seconds)
    if set(windows) != {300, 10080}:
        raise ValueError('both windows required')
    return {
        'status': 'ok',
        'five_hour_remaining_percent': windows[300][0],
        'five_hour_reset_in_seconds': windows[300][1],
        'weekly_remaining_percent': windows[10080][0],
        'weekly_reset_in_seconds': windows[10080][1],
        'age_seconds': max(0, age),
    }


def fetch(source_url):
    # A system proxy must not route the local bridge request to the Internet.
    with build_opener(ProxyHandler({})).open(source_url, timeout=20) as response:
        body = response.read(65537)
        if len(body) > 65536:
            raise ValueError('response too large')
        return normalize(json.loads(body))


def handler(source_url):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != '/quota':
                self.send_error(404)
                return
            try:
                payload = fetch(source_url)
                code = 200
            except (ValueError, URLError, OSError, OverflowError):
                # No fabricated values, secrets or upstream payload on errors.
                payload = {'status': 'unavailable'}
                code = 503
            data = json.dumps(payload, allow_nan=False, separators=(',', ':')).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8787)
    parser.add_argument('--source', default='http://127.0.0.1:8786/quota')
    options = parser.parse_args()
    from urllib.parse import urlparse
    source = urlparse(options.source)
    if source.scheme != 'http' or source.hostname not in ('127.0.0.1', 'localhost') or source.path != '/quota':
        parser.error('--source must be a loopback HTTP /quota endpoint')
    server = ThreadingHTTPServer((options.bind, options.port), handler(options.source))
    server.daemon_threads = True
    print(f'Hotspot quota: http://{options.bind}:{options.port}/quota', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
