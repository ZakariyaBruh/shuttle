#!/usr/bin/env python3
"""Tuner relay: turns things the Playdate can't play (AAC, HLS, IPTV, TuneIn) into small MP3 streams.

    python3 relay.py [--host 0.0.0.0] [--port 8787] [--m3u https://example.com/playlist.m3u] [--token SECRET]

Needs ffmpeg on PATH. By default it only listens on this computer (127.0.0.1, fine for the simulator).
For a real Playdate pass --host 0.0.0.0 so it is reachable on your Wi-Fi, and put "<computer-ip>:8787" in the
app's Data/settings.json. The relay fetches any URL it is asked to, so only expose it on a network you trust.
To reach it from anywhere with no account or key, run a Cloudflare quick tunnel next to it:
    cloudflared tunnel --url http://localhost:8787      # prints https://something.trycloudflare.com
and ALWAYS start the relay with --token SECRET when it is reachable from the internet (put the same "relay_token" in settings.json).

Endpoints (all GET):
  /t.mp3?u=<url>              transcode any audio URL (HLS/AAC/Icecast/...) to MP3
  /bbc/<service>.mp3          BBC radio, e.g. bbc_radio_fourfm, resolved through radio-browser
  /iptv/groups                JSON list of the playlist's groups
  /iptv/channels?offset=&q=&group=   JSON list of channels from the --m3u playlist (search, paging)
  /iptv/stream/<n>.mp3        audio of channel n
  /iptv/video/<n>             dithered 1-bit video + audio of channel n in the Tuner stream format (see VIDEO below)
  /tunein/browse, /tunein/search?q=   JSON lists from the TuneIn directory; /tunein/play?id=sNNN streams one
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from collections import deque
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
CHANNELS = []      # [(name, url, group)] from the M3U
TOKEN = None
_bbc_cache = {}


def fetch(url, timeout=15):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def load_m3u(src):
    """-> [(name, url, group)]"""
    text = fetch(src, 60) if src.startswith("http") else open(src, encoding="utf-8", errors="replace").read()
    name = group = None
    out = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXTINF"):
            name = line.split(",", 1)[-1].strip() or "Channel"
            m = re.search(r'group-title="([^"]*)"', line)
            group = m[1] if m else None
        elif line and not line.startswith("#"):
            out.append((name or line, line, group or "Other"))
            name = group = None
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


def net_input_opts(src, reconnect=True):
    """HTTP-only input options (a local file in the M3U would reject them)."""
    if not src.startswith("http"):
        return []
    opts = ["-user_agent", UA["User-Agent"]]
    return opts + ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"] if reconnect else opts


def ffmpeg_cmd(src):
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", *net_input_opts(src), "-i", src, "-vn", "-map", "0:a:0", "-ac", "1", "-ar", "44100", "-b:a", "64k", "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", "pipe:1"]


# ---------------------------------------------------------------------------------------------------------------------
# VIDEO. The Playdate's 400x240 1-bit screen is 52 bytes per row (50 used). ffmpeg dithers to 1 bit (ordered/Bayer dither,
# which stays still between frames so frame deltas compress well); we XOR each frame against the previous one, PackBits it,
# and interleave it with MP3 audio chunks:
#     packet = type:u8  len:u16le  payload
#     type 1 = MP3 audio bytes
#     type 2 = video frame: pts_ms:u32le  flags:u8 (bit0 = keyframe, i.e. XOR against all-zero)  PackBits(XOR delta of 52*240 bytes)
# ---------------------------------------------------------------------------------------------------------------------
W, H, STRIDE, FPS = 400, 240, 52, 15
SRC_ROW = W // 8
KEY_EVERY = 60


def packbits(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        j = i + 1
        while j < n and j - i < 128 and data[j] == data[i]:
            j += 1
        if j - i >= 3:                       # run
            out += bytes((257 - (j - i), data[i]))
            i = j
        else:                                # literal until the next run of >= 3
            k = i
            while k < n and k - i < 128:
                if k + 2 < n and data[k] == data[k + 1] == data[k + 2]:
                    break
                k += 1
            if k == i:
                k = i + 1
            out += bytes((k - i - 1,)) + data[i:k]
            i = k
    return bytes(out)


def video_filter():
    return (f"scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2,"
            f"format=gray,eq=gamma=1.35:contrast=1.15")


def video_cmd(src, audio_fd):
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re", *net_input_opts(src, reconnect=False), "-i", src,
            "-map", "0:v:0", "-vf", video_filter(), "-r", str(FPS), "-sws_dither", "bayer", "-pix_fmt", "monob", "-f", "rawvideo", "pipe:1",
            "-map", "0:a:0?", "-ac", "1", "-ar", "44100", "-b:a", "48k", "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", f"pipe:{audio_fd}"]


def pad_frame(raw):
    """50-byte rows -> 52-byte rows (the LCD's row stride)."""
    out = bytearray(STRIDE * H)
    for y in range(H):
        out[y * STRIDE:y * STRIDE + SRC_ROW] = raw[y * SRC_ROW:(y + 1) * SRC_ROW]
    return bytes(out)


def video_packets(src):
    """Generator of byte chunks in the stream format described above."""
    r_fd, w_fd = os.pipe()
    proc = subprocess.Popen(video_cmd(src, w_fd), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, pass_fds=(w_fd,))
    os.close(w_fd)
    audio = deque()
    lock = threading.Lock()

    def pump_audio():
        with os.fdopen(r_fd, "rb", 0) as f:
            while True:
                b = f.read(1024)
                if not b:
                    return
                with lock:
                    audio.append(b)
    threading.Thread(target=pump_audio, daemon=True).start()
    try:
        prev, n = bytes(STRIDE * H), 0
        size = SRC_ROW * H
        while True:
            raw = proc.stdout.read(size)
            if len(raw) < size:
                return
            with lock:
                chunks = list(audio)
                audio.clear()
            for c in chunks:
                yield b"\x01" + len(c).to_bytes(2, "little") + c
            cur = pad_frame(raw)
            key = n % KEY_EVERY == 0
            base = bytes(STRIDE * H) if key else prev
            delta = bytes(a ^ b for a, b in zip(cur, base))
            body = (n * 1000 // FPS).to_bytes(4, "little") + bytes((1 if key else 0,)) + packbits(delta)
            yield b"\x02" + len(body).to_bytes(2, "little") + body
            prev, n = cur, n + 1
    finally:
        proc.kill()


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

    def stream_video(self, src):
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/x-tuner-video")
            self.end_headers()
            for chunk in video_packets(src):
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if TOKEN and q.get("k", [""])[0] != TOKEN:
            return self.send_error(403, "bad token")
        try:
            if u.path == "/t.mp3":
                return self.stream(q["u"][0])
            m = re.fullmatch(r"/bbc/([a-z0-9_]+)\.mp3", u.path)
            if m:
                return self.stream(bbc_url(m[1]))
            if u.path == "/iptv/groups":
                counts = {}
                for _, _, g in CHANNELS:
                    counts[g] = counts.get(g, 0) + 1
                return self.send_json([{"name": f"{g} ({c})", "path": "/iptv/channels?group=" + urllib.parse.quote(g), "kind": "list"}
                                       for g, c in sorted(counts.items())][:40])
            if u.path == "/iptv/channels":
                off, term = int(q.get("offset", ["0"])[0]), q.get("q", [""])[0].lower()
                group = q.get("group", [None])[0]
                rows = [(i, n) for i, (n, _, g) in enumerate(CHANNELS) if term in n.lower() and (group is None or g == group)]
                page = [{"name": n, "path": f"/iptv/stream/{i}.mp3"} for i, n in rows[off:off + 39]]
                if off + 39 < len(rows):
                    more = {k: v[0] for k, v in q.items() if k not in ("offset", "k")}
                    more["offset"] = off + 39
                    page.append({"name": f"More... ({len(rows) - off - 39} left)", "path": "/iptv/channels?" + urllib.parse.urlencode(more), "kind": "list"})
                return self.send_json(page)
            m = re.fullmatch(r"/iptv/stream/(\d+)\.mp3", u.path)
            if m:
                return self.stream(CHANNELS[int(m[1])][1])
            m = re.fullmatch(r"/iptv/video/(\d+)", u.path)
            if m:
                return self.stream_video(CHANNELS[int(m[1])][1])
            if u.path == "/v":
                if not re.match(r"https?://", q["u"][0]):
                    raise ValueError("http(s) URLs only")
                return self.stream_video(q["u"][0])
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
    ap.add_argument("--token", help="require ?k=TOKEN on every request (use this when exposing the relay to the internet)")
    a = ap.parse_args()
    TOKEN = a.token
    if a.m3u:
        CHANNELS[:] = load_m3u(a.m3u)
        print(f"loaded {len(CHANNELS)} channels")
    print(f"relay listening on {a.host}:{a.port}")
    ThreadingHTTPServer((a.host, a.port), Handler).serve_forever()
