#!/usr/bin/env python3
"""Airwave relay: turns things the Playdate can't play (AAC, HLS, TV video, YouTube...) into small MP3 / dithered-video streams.

    python3 relay.py [--host 0.0.0.0] [--port 8787] [--m3u PLAYLIST_URL_OR_FILE ...] [--token SECRET] [--yt-cookies cookies.txt]
(or set AIRWAVE_TOKEN / AIRWAVE_M3U / PORT, which is how the Docker image is configured)

Needs ffmpeg and numpy (video dithering). Optional: yt-dlp + node (or deno) for YouTube playback. By default it only
listens on this computer (127.0.0.1, fine for the simulator). For a real Playdate pass --host 0.0.0.0 so it is reachable on
your Wi-Fi, and put "<computer-ip>:8787" in the app's Data/settings.json. The relay fetches any URL it is asked to, so only
expose it on a network you trust. To reach it from anywhere with no account or key, run a Cloudflare quick tunnel next to it:
    cloudflared tunnel --url http://localhost:8787      # prints https://something.trycloudflare.com
and ALWAYS start the relay with --token SECRET when it is reachable from the internet (put the same "relay_token" in settings.json).

Lists are JSON arrays of {"name", "path", "video"?, "kind"?}; "kind":"list" opens another list. Video paths return the stream
format described under VIDEO below. Sources: your IPTV playlist(s), Pluto TV (real free live channels), a few free news streams, official YouTube channels and
search (yt-dlp), the Internet Archive, TuneIn, BBC, radio-browser transcodes, and a generic /t.mp3 transcoder.
"""
import argparse
import json
import os
import shutil
import re
import subprocess
import sys
import threading
import time
import uuid
from collections import deque

try:
    import numpy as np
except ImportError:        # video then falls back to ffmpeg's own Bayer dither
    np = None
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UA = {"User-Agent": "AirwaveRelay/1.0"}
RADIO_BROWSER = "https://de1.api.radio-browser.info/json/stations/search"
BBC_NAMES = {
    "bbc_radio_one": "BBC Radio 1", "bbc_radio_two": "BBC Radio 2", "bbc_radio_three": "BBC Radio 3",
    "bbc_radio_fourfm": "BBC Radio 4", "bbc_radio_four_extra": "BBC Radio 4 Extra", "bbc_radio_five_live": "BBC Radio 5 Live",
    "bbc_6music": "BBC Radio 6 Music", "bbc_world_service": "BBC World Service", "bbc_asian_network": "BBC Asian Network",
    "bbc_radio_scotland_fm": "BBC Radio Scotland", "bbc_radio_wales_fm": "BBC Radio Wales", "bbc_radio_ulster": "BBC Radio Ulster",
}
CHANNELS = []      # [(name, url, group)] from the M3U
TOKEN = None
YT_COOKIES = None
FREE_LIVE = [      # official free live streams (URLs change now and then; add your own with --m3u)
    ("DW English", "https://dwamdstream102.akamaized.net/hls/live/2015525/dwstream102/index.m3u8"),
    ("Al Jazeera English", "https://live-hls-apps-aje-fa.getaj.net/AJE/index.m3u8"),
    ("Red Bull TV", "https://rbmn-live.akamaized.net/hls/live/590964/BoRB-AT/master.m3u8"),
]
# Official YouTube channels with full episodes.
SHOWS = [
    ("National Geographic", "https://www.youtube.com/@NatGeo/videos"),
    ("BBC Earth", "https://www.youtube.com/@bbcearth/videos"),
    ("DW Documentary", "https://www.youtube.com/@DWDocumentary/videos"),
    ("Smithsonian Channel", "https://www.youtube.com/@SmithsonianChannel/videos"),
    ("NASA", "https://www.youtube.com/@NASA/videos"),
]
_media_cache = {}
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


def _cached(key, fn, ttl=600):
    hit = _media_cache.get(key)
    if hit and time.time() - hit[0] < ttl:
        return hit[1]
    val = fn()
    _media_cache[key] = (time.time(), val)
    return val


def ytdlp(args, timeout=60):
    cmd = [sys.executable, "-m", "yt_dlp", "--no-warnings", *args]
    if shutil.which("node"):
        cmd[3:3] = ["--js-runtimes", "node"]
    elif shutil.which("deno"):
        cmd[3:3] = ["--js-runtimes", "deno"]
    if YT_COOKIES:
        cmd[3:3] = ["--cookies", YT_COOKIES]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode and not r.stdout.strip():
        raise RuntimeError(r.stderr.strip().splitlines()[-1][:160] if r.stderr.strip() else "yt-dlp failed")
    return r.stdout


def fmt_duration(sec):
    if not sec:
        return ""
    sec = int(sec)
    return f" ({sec // 3600}h{sec % 3600 // 60:02d})" if sec >= 3600 else f" ({sec // 60}m)"


def yt_list(target, n=30):
    """Flat listing of a channel URL or a 'ytsearchN:...' query -> list items."""
    out = []
    for line in ytdlp(["--flat-playlist", "--playlist-end", str(n), "-j", target]).splitlines():
        try:
            d = json.loads(line)
        except ValueError:
            continue
        vid = d.get("id", "")
        if not re.fullmatch(r"[\w-]{11}", vid):
            continue
        out.append({"name": d.get("title", vid), "sub": fmt_duration(d.get("duration")).strip(" ()") or "YouTube", "path": f"/yt/audio/{vid}", "video": f"/yt/video/{vid}"})
    return out


def yt_media_url(vid):
    def resolve():
        # a small progressive stream is enough for a 400x240 1-bit screen
        out = ytdlp(["-g", "-f", "best[height<=480]/b", f"https://www.youtube.com/watch?v={vid}"], 90)
        return out.strip().splitlines()[0]
    return _cached(("yt", vid), resolve, 900)


def ia_list(query, page=1):
    q = f"({query}) AND mediatype:movies"
    url = ("https://archive.org/advancedsearch.php?" + urllib.parse.urlencode(
        [("q", q), ("fl[]", "identifier"), ("fl[]", "title"), ("rows", 30), ("page", page), ("sort[]", "downloads desc"), ("output", "json")]))
    docs = json.loads(fetch(url))["response"]["docs"]
    items = [{"name": d.get("title", d["identifier"])[:90], "sub": str(d.get("creator", ""))[:50] if not isinstance(d.get("creator"), list) else "", "path": f"/ia/audio/{d['identifier']}", "video": f"/ia/video/{d['identifier']}"} for d in docs]
    if len(docs) == 30:
        items.append({"name": "More...", "path": "/ia/search?" + urllib.parse.urlencode({"q": query, "page": page + 1}), "kind": "list"})
    return items


def ia_media_url(ident):
    def resolve():
        files = json.loads(fetch(f"https://archive.org/metadata/{urllib.parse.quote(ident)}"))["files"]
        vids = [f for f in files if f["name"].lower().endswith((".mp4", ".m4v", ".ogv", ".mkv", ".webm"))]
        if not vids:
            raise ValueError("no video file in this item")
        vids.sort(key=lambda f: (not f["name"].lower().endswith(".mp4"), int(f.get("size", 0) or 0)))   # prefer mp4, then smallest
        return f"https://archive.org/download/{urllib.parse.quote(ident)}/{urllib.parse.quote(vids[0]['name'])}"
    return _cached(("ia", ident), resolve, 3600)


def free_list(prefix, rows):
    return [{"name": n, "sub": "Live", "path": f"/{prefix}/audio/{i}", "video": f"/{prefix}/video/{i}"} for i, (n, _) in enumerate(rows)]


def pluto_channels():
    def load():
        data = json.loads(fetch("https://api.pluto.tv/v2/channels", 60))
        return sorted((c for c in data if c.get("isStitched") and c.get("category") != "Testing"), key=lambda c: (c.get("category", ""), c.get("number", 0)))
    return _cached(("pluto",), load, 3600)


def pluto_url(slug):
    """Pluto TV's own free live stream for a channel (ad-supported, US lineup unless your IP says otherwise)."""
    for c in pluto_channels():
        if c["slug"] == slug:
            u = c["stitched"]["urls"][0]["url"]
            p = urllib.parse.urlparse(u)
            q = dict(urllib.parse.parse_qsl(p.query, keep_blank_values=True))
            q.update(deviceId=str(uuid.uuid4()), sid=str(uuid.uuid4()), appName="web", appVersion="9.0.0", deviceType="web",
                     deviceMake="chrome", deviceModel="web", clientTime="0")
            return p._replace(query=urllib.parse.urlencode(q)).geturl()
    raise ValueError("unknown Pluto channel")


def paged(rows, off, params, base, per=39):
    """rows: list of item dicts. Adds a 'More...' entry that re-requests `base` with the next offset."""
    page = rows[off:off + per]
    if off + per < len(rows):
        more = dict(params)
        more["offset"] = off + per
        page.append({"name": f"More... ({len(rows) - off - per} left)", "path": base + "?" + urllib.parse.urlencode(more), "kind": "list"})
    return page


def yt_shorts(query):
    """Real Shorts: a hashtag's Shorts tab (e.g. 'funny animals' -> #funnyanimals) or a channel's (@NatGeo)."""
    if re.fullmatch(r"@[\w.\-]+", query):
        target = f"https://www.youtube.com/{query}/shorts"
    else:
        tag = re.sub(r"[^a-z0-9]", "", query.lower())
        if not tag:
            raise ValueError("empty search")
        target = f"https://www.youtube.com/hashtag/{tag}/shorts"
    out = []
    for line in ytdlp(["--flat-playlist", "--playlist-end", "40", "-j", target]).splitlines():
        try:
            d = json.loads(line)
        except ValueError:
            continue
        vid = d.get("id", "")
        if re.fullmatch(r"[\w-]{11}", vid) and "/shorts/" in d.get("url", ""):
            out.append({"name": (d.get("title") or "Short")[:90], "sub": "Short", "path": f"/yt/audio/{vid}", "video": f"/yt/video/{vid}"})
    return out


def low_variant(src):
    """HLS master playlist -> its smallest variant. A 400x240 1-bit screen doesn't need the 1080p rendition (and a Wi-Fi
    relay or slow link often can't keep up with it)."""
    if ".m3u8" not in src.split("?")[0] or not src.startswith("http"):
        return src

    def pick():
        try:
            text = fetch(src, 10)
        except Exception:
            return src
        best = None
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if line.startswith("#EXT-X-STREAM-INF") and i + 1 < len(lines):
                m = re.search(r"BANDWIDTH=(\d+)", line)
                bw = int(m[1]) if m else 1 << 60
                if best is None or bw < best[0]:
                    best = (bw, lines[i + 1].strip())
        return urllib.parse.urljoin(src, best[1]) if best and best[1] else src
    return _cached(("lowvar", src), pick, 120)


def net_input_opts(src, reconnect=True):
    """HTTP-only input options (a local file in the M3U would reject them)."""
    if not src.startswith("http"):
        return []
    opts = ["-user_agent", UA["User-Agent"]]
    return opts + ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"] if reconnect else opts


def ffmpeg_cmd(src):
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", *net_input_opts(src), "-i", src, "-vn", "-map", "0:a:0", "-ac", "1", "-ar", "44100", "-b:a", "64k", "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", "pipe:1"]


# ---------------------------------------------------------------------------------------------------------------------
# VIDEO. The Playdate's 400x240 1-bit screen is 52 bytes per row (50 used). we dither to 1 bit with a static ordered dither (interleaved gradient noise,
# which stays still between frames so frame deltas compress well); we XOR each frame against the previous one, PackBits it,
# and interleave it with MP3 audio chunks:
#     packet = type:u8  len:u16le  payload
#     type 1 = MP3 audio bytes
#     type 2 = video frame: pts_ms:u32le  flags:u8 (bit0 = keyframe, i.e. XOR against all-zero)  PackBits(XOR delta of 52*240 bytes)
# ---------------------------------------------------------------------------------------------------------------------
W, H, STRIDE, FPS = 400, 240, 52, 15
SRC_ROW = W // 8
KEY_EVERY = 90
DITHER = "ign"


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


def _thresholds():
    """Static per-pixel thresholds (0..1). A pattern that never changes keeps still areas still, so frame deltas stay small."""
    y, x = np.mgrid[0:H, 0:W]
    if DITHER == "bayer":
        m = np.array([[0]])
        for _ in range(3):
            m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
        return ((m + 0.5) / 64)[y % 8, x % 8]
    # interleaved gradient noise: blue-noise-like spectrum, no visible grid, no setup cost
    return (52.9829189 * ((0.06711056 * x + 0.00583715 * y) % 1.0)) % 1.0


def video_filter(fit="contain"):
    # contain = whole picture with bars (default); cover = fill the screen, cropping the edges
    scale = (f"scale={W}:{H}:force_original_aspect_ratio=increase:flags=lanczos,crop={W}:{H}" if fit == "cover" else
             f"scale={W}:{H}:force_original_aspect_ratio=decrease:flags=lanczos,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2")
    pre = (f"{scale},format=gray,"
           # stable auto-contrast (smoothed over time), light denoise (less flicker), then sharpen so detail survives 1 bit
           "normalize=blackpt=black:whitept=white:smoothing=15,hqdn3d=1.5:1.5:5:5,unsharp=5:5:1.0:5:5:0.0,eq=gamma=1.05")
    return pre


def video_cmd(src, audio_fd, fit="contain"):
    fmt = ["-pix_fmt", "gray", "-f", "rawvideo"] if np is not None else ["-sws_dither", "bayer", "-pix_fmt", "monob", "-f", "rawvideo"]
    vf = video_filter(fit) if np is not None else f"scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2,format=gray,eq=gamma=1.35:contrast=1.15"
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re", *net_input_opts(src, reconnect=False), "-i", src,
            "-map", "0:v:0", "-vf", vf, "-r", str(FPS), *fmt, "pipe:1",
            "-map", "0:a:0?", "-ac", "1", "-ar", "44100", "-b:a", "48k", "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", f"pipe:{audio_fd}"]


def to_lcd_rows(raw):
    """gray bytes (W*H) or monob bytes -> 52-byte LCD rows (1 = white, MSB first)."""
    out = bytearray(STRIDE * H)
    if np is not None:
        bits = np.frombuffer(raw, np.uint8).reshape(H, W).astype(np.float32) / 255.0 > _TH
        rows = np.packbits(bits, axis=1)
        buf = np.zeros((H, STRIDE), np.uint8)
        buf[:, :SRC_ROW] = rows
        return buf.tobytes()
    for y in range(H):
        out[y * STRIDE:y * STRIDE + SRC_ROW] = raw[y * SRC_ROW:(y + 1) * SRC_ROW]
    return bytes(out)


_TH = _thresholds() if np is not None else None


def video_packets(src, fit="contain"):
    """Generator of byte chunks in the stream format described above."""
    r_fd, w_fd = os.pipe()
    proc = subprocess.Popen(video_cmd(src, w_fd, fit), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, pass_fds=(w_fd,))
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
        size = W * H if np is not None else SRC_ROW * H
        while True:
            raw = proc.stdout.read(size)
            if len(raw) < size:
                return
            with lock:
                chunks = list(audio)
                audio.clear()
            for c in chunks:
                yield b"\x01" + len(c).to_bytes(2, "little") + c
            cur = to_lcd_rows(raw)
            key = n % KEY_EVERY == 0
            if np is not None:
                base = bytes(STRIDE * H) if key else prev
                delta = (np.frombuffer(cur, np.uint8) ^ np.frombuffer(base, np.uint8)).tobytes()
            else:
                delta = bytes(a ^ b for a, b in zip(cur, bytes(STRIDE * H) if key else prev))
            body = (n * 1000 // FPS).to_bytes(4, "little") + bytes((1 if key else 0,)) + packbits(delta)
            yield b"\x02" + len(body).to_bytes(2, "little") + body
            prev, n = cur, n + 1
    finally:
        proc.kill()


def yt_ready():
    try:
        import importlib.util
        return importlib.util.find_spec("yt_dlp") is not None and bool(shutil.which("node") or shutil.which("deno"))
    except Exception:
        return False


def ascii_msg(e):
    """HTTP status lines are latin-1 only; yt-dlp errors often contain curly quotes."""
    return str(e).replace("\n", " ")[:100].encode("ascii", "replace").decode()


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

    def stream_video(self, src, fit="contain"):
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/x-tuner-video")
            self.end_headers()
            for chunk in video_packets(src, fit):
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if TOKEN and q.get("k", [""])[0] != TOKEN:
            return self.send_error(403, "bad token")
        try:
            if u.path == "/health":
                return self.send_json({"ok": True, "version": "3", "video": "yes" if (np is not None and shutil.which("ffmpeg")) else "no",
                                       "yt": "yes" if yt_ready() else "no", "ffmpeg": "yes" if shutil.which("ffmpeg") else "no"})
            if u.path == "/t.mp3":
                if not re.match(r"https?://", q["u"][0]):
                    raise ValueError("http(s) URLs only")
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
                rows = [(i, n, g) for i, (n, _, g) in enumerate(CHANNELS) if term in n.lower() and (group is None or g == group)]
                page = [{"name": n, "sub": g, "path": f"/iptv/audio/{i}", "video": f"/iptv/video/{i}"} for i, n, g in rows[off:off + 39]]
                if off + 39 < len(rows):
                    more = {k: v[0] for k, v in q.items() if k not in ("offset", "k")}
                    more["offset"] = off + 39
                    page.append({"name": f"More... ({len(rows) - off - 39} left)", "path": "/iptv/channels?" + urllib.parse.urlencode(more), "kind": "list"})
                return self.send_json(page)
            m = re.fullmatch(r"/(iptv|free|yt|ia|pluto)/(audio|video)/([\w.\-]+?)(?:\.mp3)?", u.path)
            if m:
                kind, mode, ident = m.groups()
                fit = "cover" if q.get("fit", [""])[0] == "cover" else "contain"
                if kind == "pluto":
                    src = pluto_url(ident)
                elif kind == "iptv":
                    src = CHANNELS[int(ident)][1]
                elif kind == "free":
                    src = FREE_LIVE[int(ident)][1]
                elif kind == "yt":
                    src = yt_media_url(ident)
                else:
                    src = ia_media_url(ident)
                src = low_variant(src)
                return self.stream(src) if mode == "audio" else self.stream_video(src, fit)
            if u.path == "/tv/shows":
                return self.send_json([{"name": n, "sub": "YouTube channel", "path": "/yt/channel?c=" + urllib.parse.quote(url, safe=""), "kind": "list"} for n, url in SHOWS])
            if u.path == "/pluto/categories":
                counts = {}
                for c in pluto_channels():
                    counts[c["category"]] = counts.get(c["category"], 0) + 1
                top = [{"name": f"{k} ({v})", "path": "/pluto/channels?cat=" + urllib.parse.quote(k), "kind": "list"} for k, v in sorted(counts.items(), key=lambda kv: -kv[1])]
                return self.send_json(top[:40])
            if u.path == "/pluto/movies":
                rows = [{"name": c["name"], "sub": c.get("category", "Movies"), "path": f"/pluto/audio/{c['slug']}", "video": f"/pluto/video/{c['slug']}"}
                        for c in pluto_channels() if c.get("category") in ("Movies", "Westerns", "Sci-Fi") or re.search(r"movie|cinema|film", c["name"], re.I)]
                return self.send_json(paged(rows, int(q.get("offset", ["0"])[0]), {k: v[0] for k, v in q.items() if k not in ("offset", "k")}, "/pluto/movies"))
            if u.path == "/pluto/channels":
                cat, term, off = q.get("cat", [None])[0], q.get("q", [""])[0].lower(), int(q.get("offset", ["0"])[0])
                rows = [{"name": c["name"], "sub": c.get("category", "Live"), "path": f"/pluto/audio/{c['slug']}", "video": f"/pluto/video/{c['slug']}"}
                        for c in pluto_channels() if term in c["name"].lower() and (cat is None or c["category"] == cat)]
                params = {k: v[0] for k, v in q.items() if k not in ("offset", "k")}
                return self.send_json(paged(rows, off, params, "/pluto/channels"))
            if u.path == "/yt/shorts":
                return self.send_json(_cached(("shorts", q["q"][0]), lambda: yt_shorts(q["q"][0]), 600))
            if u.path == "/tv/free":
                return self.send_json(free_list("free", FREE_LIVE))
            if u.path == "/yt/channel":
                target = q["c"][0]
                if not re.match(r"https://www\.youtube\.com/(@[\w.\-]+|channel/[\w\-]+)(/videos)?$", target):
                    raise ValueError("YouTube channel URLs only")
                return self.send_json(_cached(("ytlist", target), lambda: yt_list(target), 600))
            if u.path == "/yt/search":
                return self.send_json(_cached(("ytsearch", q["q"][0]), lambda: yt_list("ytsearch25:" + q["q"][0], 25), 600))
            if u.path == "/ia/search":
                return self.send_json(ia_list(q["q"][0], int(q.get("page", ["1"])[0])))
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
            self.send_error(400, ascii_msg(e))
        except Exception as e:  # network errors etc.
            self.send_error(502, ascii_msg(e))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--m3u", action="append", help="IPTV playlist URL or file (repeatable), e.g. https://iptv-org.github.io/iptv/categories/documentary.m3u")
    ap.add_argument("--yt-cookies", help="cookies.txt for yt-dlp if YouTube asks you to sign in (bot check)")
    ap.add_argument("--token", help="require ?k=TOKEN on every request (use this when exposing the relay to the internet)")
    a = ap.parse_args()
    TOKEN, YT_COOKIES = a.token or os.environ.get("AIRWAVE_TOKEN") or None, a.yt_cookies
    if a.port == 8787 and os.environ.get("PORT"):
        a.port = int(os.environ["PORT"])
    for src in (a.m3u or []) + [x for x in os.environ.get("AIRWAVE_M3U", "").split(",") if x]:
        CHANNELS.extend(load_m3u(src))
    if CHANNELS:
        print(f"loaded {len(CHANNELS)} channels")
    if np is None:
        print("numpy not found: video will use ffmpeg's plain Bayer dither (pip install numpy for the better one)")
    print(f"relay listening on {a.host}:{a.port}")
    ThreadingHTTPServer((a.host, a.port), Handler).serve_forever()
