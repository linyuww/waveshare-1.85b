"""Signed Yaohud music lookup and bounded FFmpeg PCM streaming for the ESP32."""
import argparse
import hashlib
import hmac
import ipaddress
import json
import logging
from pathlib import Path
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import Request, urlopen


class MusicError(Exception):
    pass


def signed_headers(api_key, secret_key, timestamp=None):
    timestamp = str(int(time.time()) if timestamp is None else timestamp)
    signature = hmac.new(secret_key.encode(),
                         f"key={api_key}&timestamp={timestamp}".encode(), hashlib.sha256).hexdigest()
    return {"X-Api-Key": api_key, "X-Api-Timestamp": timestamp, "X-Api-Sign": signature}


def media_url(value):
    if not isinstance(value, str):
        raise MusicError("Music URL missing")
    parsed = urlsplit(value)
    host = (parsed.hostname or "").lower()
    allowed = any(host == domain or host.endswith("." + domain)
                  for domain in ("music.163.com", "music.126.net"))
    if parsed.scheme not in ("https", "http") or not allowed or parsed.username or parsed.password:
        raise MusicError("Music host not allowed")
    if parsed.port not in (None, 80, 443):
        raise MusicError("Music port not allowed")
    return value


def parse_track(body):
    if not isinstance(body, dict) or body.get("code") != 200 or not isinstance(body.get("data"), dict):
        raise MusicError("Music provider rejected the lookup")
    data = body["data"]
    return {"title": str(data.get("name") or "Music")[:120],
            "artist": str(data.get("songname") or "")[:120],
            "media": media_url(data.get("musicurl") or data.get("url"))}


class Provider:
    def __init__(self, config):
        self.config = config
        self.cache = {}
        self.lock = threading.Lock()

    def resolve(self, song, artist):
        cache_key = (song, artist)
        with self.lock:
            now = time.monotonic()
            self.cache = {key: entry for key, entry in self.cache.items() if entry[0] > now}
            cached = self.cache.get(cache_key)
            if cached:
                return cached[1]
            query = urlencode({"key": self.config["api_key"],
                               "msg": " ".join(filter(None, (song, artist))), "n": 1})
            request = Request("https://api.yaohud.cn/api/music/wy?" + query,
                              headers=signed_headers(self.config["api_key"], self.config["secret_key"]))
            try:
                with urlopen(request, timeout=15) as response:
                    raw = response.read(131073)
                if len(raw) > 131072:
                    raise MusicError("Music provider response too large")
                track = parse_track(json.loads(raw))
            except (HTTPError, URLError, OSError, ValueError) as error:
                raise MusicError("Music lookup failed; check provider credentials/network") from error
            if len(self.cache) >= 32:
                self.cache.pop(next(iter(self.cache)))
            self.cache[cache_key] = (time.monotonic() + 180, track)
            logging.info("Music resolved")
            return track


class MusicServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, config):
        self.config = config
        self.provider = Provider(config)
        self.stream_slots = threading.BoundedSemaphore(1)
        self.request_slots = threading.BoundedSemaphore(8)
        super().__init__((config["bind"], config["port"]), MusicHandler)

    def process_request(self, request, client_address):
        if not self.request_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.request_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.request_slots.release()

    def handle_error(self, request, client_address):
        logging.error("Music request failed")


class MusicHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(20)

    def log_message(self, format_string, *arguments):
        pass

    def respond(self, code, body):
        raw = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        parsed = urlsplit(self.path)
        parts = parsed.path.strip("/").split("/")
        peer = ipaddress.ip_address(self.client_address[0])
        token = self.server.config["token"]
        if (not (peer.is_private or peer.is_loopback) or len(parts) != 3 or
                not hmac.compare_digest(parts[0], token) or parts[1] != "music"):
            self.respond(404, {"ok": False, "message": "Not found"})
            return
        if parts[2] == "health":
            self.respond(200, {"ok": True, "sampleRate": 16000, "channels": 1})
            return
        if parts[2] not in ("resolve", "stream") or len(parsed.query) > 2048:
            self.respond(400, {"ok": False, "message": "Invalid music request"})
            return
        try:
            query = parse_qs(parsed.query, max_num_fields=2, strict_parsing=True)
            song = query.get("song", [""])[0].strip()
            artist = query.get("artist", [""])[0].strip()
            if (set(query) - {"song", "artist"} or any(len(value) != 1 for value in query.values()) or
                    not song or len(song.encode()) > 120 or len(artist.encode()) > 120 or
                    any(ord(char) < 32 for char in song + artist)):
                raise ValueError("Invalid search")
        except ValueError:
            self.respond(400, {"ok": False, "message": "Invalid song/artist"})
            return
        try:
            track = self.server.provider.resolve(song, artist)
            if parts[2] == "resolve":
                self.respond(200, {"ok": True, "title": track["title"], "artist": track["artist"],
                                   "sampleRate": 16000, "channels": 1})
            else:
                self.stream(track)
        except MusicError as error:
            logging.warning("%s", error)
            self.respond(502, {"ok": False, "message": str(error)})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            logging.info("Music client disconnected")

    def stream(self, track):
        if not self.server.stream_slots.acquire(blocking=False):
            self.respond(409, {"ok": False, "message": "Another music stream is active"})
            return
        process = None
        watchdog = None
        try:
            process = subprocess.Popen([
                self.server.config["ffmpeg"], "-nostdin", "-hide_banner", "-loglevel", "error",
                "-protocol_whitelist", "http,https,tcp,tls,crypto", "-rw_timeout", "15000000",
                "-user_agent", "Mozilla/5.0", "-referer", "https://music.163.com/",
                "-i", track["media"], "-vn", "-ac", "1", "-ar", "16000", "-f", "s16le", "pipe:1"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            watchdog = threading.Timer(25, process.kill)
            watchdog.daemon = True
            watchdog.start()
            first = process.stdout.read(4096)
            watchdog.cancel()
            if not first:
                raise MusicError("No playable audio; song may be unavailable")
            watchdog = threading.Timer(1800, process.kill)
            watchdog.daemon = True
            watchdog.start()
            self.connection.settimeout(1800)
            self.send_response(200)
            self.send_header("Content-Type", "audio/pcm")
            self.send_header("Connection", "close")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.close_connection = True
            logging.info("PCM stream started: 16000 Hz mono s16le")
            self.wfile.write(first)
            while True:
                chunk = process.stdout.read(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)
            logging.info("PCM stream finished")
        finally:
            if watchdog:
                watchdog.cancel()
            if process:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
                process.stdout.close()
            self.server.stream_slots.release()


def load_config(path):
    config = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    for field in ("api_key", "secret_key", "token"):
        if not isinstance(config.get(field), str) or not config[field].strip():
            raise ValueError("Missing private music configuration")
    if len(config["token"]) < 32 or not config["token"].isascii() or not config["token"].isalnum():
        raise ValueError("Use an alphanumeric music token of at least 32 characters")
    address = ipaddress.ip_address(config["bind"])
    if address.version != 4 or not (address.is_private or address.is_loopback) or address.is_unspecified:
        raise ValueError("Bind only to a specific private IPv4 address")
    if not isinstance(config["port"], int) or not 1024 <= config["port"] <= 65535:
        raise ValueError("Invalid music bridge port")
    config["ffmpeg"] = shutil.which(config.get("ffmpeg", "ffmpeg"))
    if not config["ffmpeg"]:
        raise ValueError("FFmpeg is required")
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        config = load_config(args.config)
        if args.check:
            print("Music configuration and FFmpeg validated")
            return
        with MusicServer(config) as server:
            logging.info("Music bridge listening on %s:%d", config["bind"], config["port"])
            server.serve_forever()
    except (OSError, ValueError):
        logging.error("Music bridge setup failed; check private config, FFmpeg and bind address")
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
