// Airwave relay for Vercel. One function: converts streams to small MP3s and dithers video to the Playdate's 1-bit screen.
// Stream format (same as relay/relay.py): packet = type:u8 len:u16le payload; type 1 = MP3 bytes; type 2 = video frame
// (pts_ms:u32le, flags:u8 bit0=key, PackBits of the XOR delta of 52*240 LCD bytes).
const { spawn } = require("child_process");
const crypto = require("crypto");

const FFMPEG = process.env.FFMPEG_PATH || require("ffmpeg-static");
const TOKEN = process.env.AIRWAVE_TOKEN || "";
const UA = "AirwaveRelay/2.5";
const W = 400, H = 240, STRIDE = 52, FPS = 15, SRC_ROW = 50, KEY_EVERY = 90;

const FREE_LIVE = [
  ["DW English", "https://dwamdstream102.akamaized.net/hls/live/2015525/dwstream102/index.m3u8"],
  ["Al Jazeera English", "https://live-hls-apps-aje-fa.getaj.net/AJE/index.m3u8"],
  ["Red Bull TV", "https://rbmn-live.akamaized.net/hls/live/590964/BoRB-AT/master.m3u8"],
];

const cache = new Map();
async function cached(key, ttlMs, fn) {
  const hit = cache.get(key);
  if (hit && Date.now() - hit.t < ttlMs) return hit.v;
  const v = await fn();
  cache.set(key, { t: Date.now(), v });
  return v;
}
const getText = async (url, ms = 15000) => {
  const r = await fetch(url, { headers: { "user-agent": UA }, signal: AbortSignal.timeout(ms) });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.text();
};
const getJson = async (url, ms) => JSON.parse(await getText(url, ms));

// ---------- sources ----------
async function plutoChannels() {
  return cached("pluto", 3600e3, async () => {
    const d = await getJson("https://api.pluto.tv/v2/channels", 30000);
    return d.filter((c) => c.isStitched && c.category !== "Testing").sort((a, b) => (a.category + a.number).localeCompare(b.category + b.number));
  });
}
async function plutoUrl(slug) {
  const c = (await plutoChannels()).find((x) => x.slug === slug);
  if (!c) throw new Error("unknown Pluto channel");
  const u = new URL(c.stitched.urls[0].url);
  for (const [k, v] of Object.entries({ deviceId: crypto.randomUUID(), sid: crypto.randomUUID(), appName: "web", appVersion: "9.0.0", deviceType: "web", deviceMake: "chrome", deviceModel: "web", clientTime: "0" })) u.searchParams.set(k, v);
  return u.toString();
}
async function iaList(q, page = 1) {
  const url = "https://archive.org/advancedsearch.php?" + new URLSearchParams([["q", `(${q}) AND mediatype:movies`], ["fl[]", "identifier"], ["fl[]", "title"], ["fl[]", "creator"], ["rows", "30"], ["page", String(page)], ["sort[]", "downloads desc"], ["output", "json"]]);
  const docs = (await getJson(url)).response.docs;
  const items = docs.map((d) => ({ name: String(d.title || d.identifier).slice(0, 90), sub: typeof d.creator === "string" ? d.creator.slice(0, 50) : "", path: `/ia/audio/${d.identifier}`, video: `/ia/video/${d.identifier}` }));
  if (docs.length === 30) items.push({ name: "More...", path: "/ia/search?" + new URLSearchParams({ q, page: String(page + 1) }), kind: "list" });
  return items;
}
async function iaMediaUrl(id) {
  return cached("ia:" + id, 3600e3, async () => {
    const files = (await getJson(`https://archive.org/metadata/${encodeURIComponent(id)}`)).files || [];
    const v = files.filter((f) => /\.(mp4|m4v|ogv|mkv|webm)$/i.test(f.name)).sort((a, b) => (/\.mp4$/i.test(b.name) - /\.mp4$/i.test(a.name)) || (Number(a.size || 0) - Number(b.size || 0)));
    if (!v.length) throw new Error("no video file in this item");
    return `https://archive.org/download/${encodeURIComponent(id)}/${encodeURIComponent(v[0].name)}`;
  });
}
// A 400x240 1-bit screen doesn't need 1080p: pick the smallest HLS rendition.
async function lowVariant(src) {
  if (!/\.m3u8/.test(src.split("?")[0])) return src;
  return cached("low:" + src, 120e3, async () => {
    try {
      const lines = (await getText(src, 10000)).split(/\r?\n/);
      let best = null;
      lines.forEach((l, i) => {
        if (l.startsWith("#EXT-X-STREAM-INF") && lines[i + 1]) {
          const m = /BANDWIDTH=(\d+)/.exec(l); const bw = m ? Number(m[1]) : Infinity;
          if (!best || bw < best.bw) best = { bw, url: lines[i + 1].trim() };
        }
      });
      return best ? new URL(best.url, src).toString() : src;
    } catch { return src; }
  });
}

// ---------- ffmpeg ----------
function inputOpts(src) { return /^https?:/.test(src) ? ["-user_agent", UA, "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"] : []; }

function streamMp3(src, res) {
  const p = spawn(FFMPEG, ["-hide_banner", "-loglevel", "error", ...inputOpts(src), "-i", src, "-vn", "-map", "0:a:0", "-ac", "1", "-ar", "44100", "-b:a", "64k", "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", "pipe:1"], { stdio: ["ignore", "pipe", "ignore"] });
  res.writeHead(200, { "content-type": "audio/mpeg", "cache-control": "no-store" });
  p.stdout.pipe(res);
  const stop = () => p.kill("SIGKILL");
  res.on("close", stop); p.on("close", () => res.end());
}

// interleaved-gradient-noise thresholds: a static pattern, so still areas stay still and deltas stay small
const TH = new Float32Array(W * H);
for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const v = (52.9829189 * ((0.06711056 * x + 0.00583715 * y) % 1)) % 1; TH[y * W + x] = v; }

function packbits(d) {
  const out = []; let i = 0; const n = d.length;
  while (i < n) {
    let j = i + 1;
    while (j < n && j - i < 128 && d[j] === d[i]) j++;
    if (j - i >= 3) { out.push(257 - (j - i), d[i]); i = j; }
    else {
      let k = i;
      while (k < n && k - i < 128) { if (k + 2 < n && d[k] === d[k + 1] && d[k + 1] === d[k + 2]) break; k++; }
      if (k === i) k = i + 1;
      out.push(k - i - 1); for (let q = i; q < k; q++) out.push(d[q]);
      i = k;
    }
  }
  return Buffer.from(out);
}

function streamVideo(src, res, fit) {
  const scale = fit === "cover" ? `scale=${W}:${H}:force_original_aspect_ratio=increase:flags=lanczos,crop=${W}:${H}` : `scale=${W}:${H}:force_original_aspect_ratio=decrease:flags=lanczos,pad=${W}:${H}:(ow-iw)/2:(oh-ih)/2`;
  const vf = `${scale},format=gray,normalize=blackpt=black:whitept=white:smoothing=15,hqdn3d=1.5:1.5:5:5,unsharp=5:5:1.0:5:5:0.0,eq=gamma=1.05`;
  const args = ["-hide_banner", "-loglevel", "error", "-re", ...inputOpts(src), "-i", src,
    "-map", "0:v:0", "-vf", vf, "-r", String(FPS), "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1",
    "-map", "0:a:0?", "-ac", "1", "-ar", "44100", "-b:a", "48k", "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", "pipe:3"];
  const p = spawn(FFMPEG, args, { stdio: ["ignore", "pipe", "ignore", "pipe"] });
  res.writeHead(200, { "content-type": "application/x-airwave-video", "cache-control": "no-store" });
  let audio = [];
  p.stdio[3].on("data", (b) => { for (let o = 0; o < b.length; o += 1024) audio.push(b.subarray(o, Math.min(o + 1024, b.length))); });
  let buf = Buffer.alloc(0), prev = Buffer.alloc(STRIDE * H), n = 0;
  const size = W * H;
  p.stdout.on("data", (chunk) => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
    while (buf.length >= size) {
      const gray = buf.subarray(0, size); buf = buf.subarray(size);
      for (const a of audio) { const h = Buffer.alloc(3); h[0] = 1; h.writeUInt16LE(a.length, 1); res.write(h); res.write(a); }
      audio = [];
      const cur = Buffer.alloc(STRIDE * H);
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (gray[y * W + x] / 255 > TH[y * W + x]) cur[y * STRIDE + (x >> 3)] |= 0x80 >> (x & 7);
      const key = n % KEY_EVERY === 0;
      const delta = Buffer.alloc(STRIDE * H);
      for (let i = 0; i < delta.length; i++) delta[i] = cur[i] ^ (key ? 0 : prev[i]);
      const body = Buffer.concat([Buffer.alloc(5), packbits(delta)]);
      body.writeUInt32LE(Math.floor((n * 1000) / FPS), 0); body[4] = key ? 1 : 0;
      const h = Buffer.alloc(3); h[0] = 2; h.writeUInt16LE(body.length, 1);
      res.write(h); res.write(body);
      prev = cur; n++;
    }
  });
  const stop = () => p.kill("SIGKILL");
  res.on("close", stop); p.on("close", () => res.end());
}

// ---------- AI chat through the Vercel AI Gateway (key stays on the server) ----------
async function chat(req, res) {
  const key = process.env.AI_GATEWAY_API_KEY;
  if (!key) return json(res, 501, { error: "AI Gateway key not configured" });
  let raw = ""; for await (const c of req) raw += c;
  const body = JSON.parse(raw || "{}");
  const r = await fetch("https://ai-gateway.vercel.sh/v1/chat/completions", {
    method: "POST", headers: { authorization: `Bearer ${key}`, "content-type": "application/json" },
    body: JSON.stringify({ model: process.env.AI_MODEL || "openai/gpt-4o-mini", max_tokens: Math.min(body.max_tokens || 350, 500), messages: (body.messages || []).slice(-12) }),
  });
  res.writeHead(r.status, { "content-type": "application/json" });
  res.end(await r.text());
}

// ---------- routing ----------
const json = (res, code, obj) => { const b = JSON.stringify(obj); res.writeHead(code, { "content-type": "application/json", "content-length": Buffer.byteLength(b) }); res.end(b); };
const page = (rows, off, params, base, per = 39) => {
  const out = rows.slice(off, off + per);
  if (off + per < rows.length) out.push({ name: `More... (${rows.length - off - per} left)`, path: base + "?" + new URLSearchParams({ ...params, offset: String(off + per) }), kind: "list" });
  return out;
};

module.exports = async (req, res) => {
  try {
    const u = new URL(req.url, "http://x");
    const q = u.searchParams;
    if (TOKEN && q.get("k") !== TOKEN) return json(res, 403, { error: "bad token" });
    const p = u.pathname;
    if (p === "/health") return json(res, 200, { ok: true, version: "2.5", video: "yes", yt: "no", ffmpeg: "yes", region: process.env.VERCEL_REGION || "", chat: process.env.AI_GATEWAY_API_KEY ? "yes" : "no" });
    if (p === "/chat" && req.method === "POST") return await chat(req, res);
    if (p === "/t.mp3") { const s = q.get("u") || ""; if (!/^https?:/.test(s)) return json(res, 400, { error: "http(s) URLs only" }); return streamMp3(await lowVariant(s), res); }
    if (p === "/v") { const s = q.get("u") || ""; if (!/^https?:/.test(s)) return json(res, 400, { error: "http(s) URLs only" }); return streamVideo(await lowVariant(s), res, q.get("fit")); }
    let m = /^\/(free|pluto|ia)\/(audio|video)\/([\w.\-]+?)(?:\.mp3)?$/.exec(p);
    if (m) {
      const [, kind, mode, id] = m;
      let src = kind === "free" ? (FREE_LIVE[Number(id)] || [])[1] : kind === "pluto" ? await plutoUrl(id) : await iaMediaUrl(id);
      if (!src) return json(res, 404, { error: "unknown channel" });
      src = await lowVariant(src);
      return mode === "audio" ? streamMp3(src, res) : streamVideo(src, res, q.get("fit") === "cover" ? "cover" : "contain");
    }
    if (p === "/tv/free") return json(res, 200, FREE_LIVE.map(([name], i) => ({ name, sub: "Live", path: `/free/audio/${i}`, video: `/free/video/${i}` })));
    if (p === "/pluto/categories") {
      const counts = {}; for (const c of await plutoChannels()) counts[c.category] = (counts[c.category] || 0) + 1;
      return json(res, 200, Object.entries(counts).sort((a, b) => b[1] - a[1]).slice(0, 40).map(([k, v]) => ({ name: `${k} (${v})`, path: "/pluto/channels?cat=" + encodeURIComponent(k), kind: "list" })));
    }
    if (p === "/pluto/channels") {
      const cat = q.get("cat"), term = (q.get("q") || "").toLowerCase(), off = Number(q.get("offset") || 0);
      const rows = (await plutoChannels()).filter((c) => c.name.toLowerCase().includes(term) && (!cat || c.category === cat)).map((c) => ({ name: c.name, sub: c.category, path: `/pluto/audio/${c.slug}`, video: `/pluto/video/${c.slug}` }));
      const params = {}; for (const [k, v] of q) if (k !== "offset" && k !== "k") params[k] = v;
      return json(res, 200, page(rows, off, params, "/pluto/channels"));
    }
    if (p === "/ia/search") return json(res, 200, await iaList(q.get("q") || "", Number(q.get("page") || 1)));
    if (p.startsWith("/yt/") || p === "/tv/shows") return json(res, 501, { error: "YouTube isn't available on this host (YouTube blocks cloud servers)" });
    if (p.startsWith("/iptv/")) return json(res, 200, []);
    return json(res, 404, { error: "not found" });
  } catch (e) {
    if (!res.headersSent) json(res, 502, { error: String(e.message || e).slice(0, 120) }); else res.end();
  }
};
