#!/usr/bin/env python3
"""Tuner relay: turns things the Playdate can't play (AAC, HLS, IPTV, TuneIn) into small MP3 streams.

    python3 relay.py [--host 0.0.0.0] [--port 8787] [--m3u https://example.com/playlist.m3u]

Needs ffmpeg on PATH. By default it only listens on this computer (127.0.0.1, fine for the simulator).
For a real Playdate pass --host 0.0.0.0 so it is reachable on your Wi-Fi, and put "<computer-ip>:8787" in the
app's Data/settings.json. The relay fetches any URL it is asked to, so only expose it on a network you trust.

Endpoints (all GET):
  /t.mp3?u=<url>              transcode any audio URL (HLS/AAC/Icecast/...) to MP3
  /bbc/<service>.mp3          BBC radio, e.g. bbc_radio_fourfm, resolved through radio-browser
  /iptv/channels?offset=&q=   JSON list of channels from the --m3u playlist
  /iptv/stream/<n>.mp3        audio of channel n (the Playdate can't decode TV video)
  /tunein/browse, /tunein/search?q=   JSON lists from the TuneIn directory; /tunein/play?id=sNNN streams one
"""
import argparse
import json
import re
import subprocess
import sys
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UA = {"User-Agent": "PlaydateTunerRelay/1.0"}
RADIO_BROWSER = "https://de1.api.radio-browser.info/json/stations/search"
BBC_NAMES = {
    "bbc_radio_one": "BBC Radio 1", "bbc_radio_two": "BBC Radio 2", "bbc_radio_three": "BBC Radio 3",
    "bbc_radio_fourfm": "BBC Radio 4", "bbc_radio_four_extra": "BBC Radio 4 Extra", "bbc_radio_five_live": "BBC Radio 5 Live",
    "bbc_6music": "BBC Radio 6 Music", "bbc_world_service": "BBC World Service", "bbc_asian_network": "BBC Asian Network",
    "bbc_radio_scotland_fm": "BBC Radio Scotland", "bbc_radio_wales_fm": "BBC Radio Wales", "bbc_radio_ulster": "BBC Radio Ulster",
}
CHANNELS = []      # [(name, url)] from the M3U
_bbc_cache = {}


def fetch(url, timeout=15):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def load_m3u(src):
    text = fetch(src, 60) if src.startswith("http") else open(src, encoding="utf-8", errors="replace").read()
    name = None
    out = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXTINF"):
            name = line.split(",", 1)[-1].strip() or "Channel"
        elif line and not line.startswith("#"):
            out.append((name or line, line))
            name = None
    return out


def bbc_url(service):
    if service in _bbc_cache:
        return _bbc_cache[service]
    want = BBC_NAMES.get(service)
    if not want:
        raise ValueError("unknown BBC service")
    q = urllib.parse.urlencode({"name": want, "hidebroken": "true", "order": "clickcount", "reverse": "true", "limit": 20})
    for st in json.loads(fetch(f"{RADIO_BROWSER}?{q}")):
        if service in st.get("url_resolved", ""):
            _bbc_cache[service] = st["url_resolved"]
            return st["url_resolved"]
    raise ValueError("no live stream found for " + service)


def tunein_list(url):
    out = []
    for o in json.loads(fetch(url)).get("body", []):
        for s in [o] + o.get("children", []):
            if s.get("item") == "station" and s.get("guide_id"):
                sub = s.get("subtext")
                out.append({"name": s["text"] + (f" - {sub}" if sub else ""), "path": "/tunein/play?id=" + s["guide_id"]})
    return out


def tunein_stream(station_id):
    data = json.loads(fetch(f"https://opml.radiotime.com/Tune.ashx?id={urllib.parse.quote(station_id)}&render=json"))
    for b in data.get("body", []):
        if b.get("url"):
            return b["url"]
    raise ValueError("no stream for station")


def ffmpeg_cmd(src):
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5",
            "-user_agent", UA["User-Agent"], "-i", src, "-vn", "-map", "0:a:0", "-ac", "1", "-ar", "44100", "-b:a", "64k", "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", "pipe:1"]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"      # no keep-alive / chunking: the Playdate reads until close

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def send_json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def stream(self, src):
        proc = subprocess.Popen(ffmpeg_cmd(src), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.end_headers()
            while True:
                chunk = proc.stdout.read(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            proc.kill()

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        try:
            if u.path == "/t.mp3":
                return self.stream(q["u"][0])
            m = re.fullmatch(r"/bbc/([a-z0-9_]+)\.mp3", u.path)
            if m:
                return self.stream(bbc_url(m[1]))
            if u.path == "/iptv/channels":
                off, term = int(q.get("offset", ["0"])[0]), q.get("q", [""])[0].lower()
                rows = [(i, n) for i, (n, _) in enumerate(CHANNELS) if term in n.lower()]
                return self.send_json([{"name": n, "path": f"/iptv/stream/{i}.mp3"} for i, n in rows[off:off + 40]])
            m = re.fullmatch(r"/iptv/stream/(\d+)\.mp3", u.path)
            if m:
                return self.stream(CHANNELS[int(m[1])][1])
            if u.path == "/tunein/browse":
                return self.send_json(tunein_list("https://opml.radiotime.com/Browse.ashx?c=trending&render=json")[:40])
            if u.path == "/tunein/search":
                return self.send_json(tunein_list("https://opml.radiotime.com/Search.ashx?render=json&query=" + urllib.parse.quote(q["q"][0]))[:40])
            if u.path == "/tunein/play":
                return self.stream(tunein_stream(q["id"][0]))
            self.send_error(404)
        except (KeyError, IndexError, ValueError) as e:
            self.send_error(400, str(e))
        except Exception as e:  # network errors etc.
            self.send_error(502, str(e)[:100])


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--m3u", help="IPTV playlist URL or file")
    a = ap.parse_args()
    if a.m3u:
        CHANNELS[:] = load_m3u(a.m3u)
        print(f"loaded {len(CHANNELS)} channels")
    print(f"relay listening on {a.host}:{a.port}")
    ThreadingHTTPServer((a.host, a.port), Handler).serve_forever()
