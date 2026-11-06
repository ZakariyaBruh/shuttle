# Airwave for Playdate

Radio, podcasts, an audio library, TV and an AI chat on the Playdate, written in C (SDK 3.1). UI font: Roobert. Text entry uses the
system keyboard (only available to Lua, hence the tiny `Source/main.lua`); the UI re-lays itself out in the space beside the keyboard as it slides in.

## It works on its own (no relay, no setup)
| Home card | What you get | Source |
|---|---|---|
| Radio | thousands of MP3 stations, genres, countries, search, **BBC World Service** | radio-browser.info |
| Podcasts | Apple top charts, search, episodes | Apple + the shows' RSS feeds |
| Library | **old-time radio, LibriVox audiobooks**, radio programs, live concerts, search | Internet Archive |
| TuneIn | trending, music, news, talk, sports, search | TuneIn directory |
| AI Chat | chat bubbles, remembers the last turns, no sign-in | pollinations.ai |

## With the relay (TV and more)
The Playdate only decodes MP3 and has a 1-bit screen, so **video, YouTube Shorts and BBC Radio 1-6** are converted by a small helper
server, the *relay*. Without one, those rows show a padlock and open a friendly explanation instead of an error.
With it you get: live channels (Pluto TV), documentary channels, Shorts, free live news, classic TV/films, your own IPTV playlist, and
AAC-only radio stations/podcasts. Up/Down while watching = next/previous channel.

### Setting it up, no files to edit
1. Start a relay (below).
2. In Airwave: **Setup > Relay address**, type the address with the system keyboard, then **Test the relay**.

**Option A, on your computer** (needs python3 + ffmpeg):
```bash
./relay/start.sh                  # prints the address to type, e.g. 192.168.1.20:8787 (same Wi-Fi)
./relay/start.sh --tunnel         # also opens a free Cloudflare quick tunnel: no account, no key, works from anywhere,
                                  # with a random token printed for Setup > Relay token (needs the cloudflared program)
```
**Option B, any US server / VPS** (Docker): `docker build -t airwave-relay relay/ && docker run -p 8787:8787 -e AIRWAVE_TOKEN=secret airwave-relay`

**Option C, Cloudflare Containers**: see `cloudflare/README.md`. **Untested** (I can't deploy to a Cloudflare account from my environment), and it needs a paid Workers plan:
Cloudflare Workers themselves can't run ffmpeg, which is why there's no free, keyless, hosted way to do the conversion.

### Please read: where the relay runs matters for live TV
Pluto TV (and some other services) license channels by country and often refuse data-centre IP addresses. From a US Google Cloud address I got
Pluto's "where to watch" card instead of the channel, so a cloud-hosted relay may not get Pluto video even when it is in the US. A relay on a
home connection is the most reliable for live TV. Airwave doesn't spoof locations. Nat Geo Wild (the TV channel) is pay-TV only and isn't available free anywhere I could find.

### Relay details
Video: 400x240, 15 fps, ordered dithering with interleaved-gradient noise (static pattern, so still areas stay still), after stabilised
auto-contrast, light denoise and sharpening; XOR-delta + PackBits frames with 48 kbps audio, about 20-80 KB/s. HLS inputs use the smallest
rendition. `/health` reports what the relay can do (video / YouTube). `--token` (or `AIRWAVE_TOKEN`) requires `?k=` on every request.
YouTube needs yt-dlp plus node or deno, and sometimes `--yt-cookies` (YouTube blocks data-centre IPs with a bot check; I could verify
listings from my environment but **not YouTube playback**).

### Vercel relay (v2.6)
`vercel/` is the same relay as one Vercel Function (Node + `ffmpeg-static`): deploy the folder, set `AIRWAVE_TOKEN` (and optionally
`AI_GATEWAY_API_KEY` for AI chat), then enter `https://<project>.vercel.app` and the token in Setup (IPTV channels come from the free iptv-org lists, or your own playlist via `IPTV_URL`; this personal build has them baked in via `src/defaults.h`; Setup overrides). Verified live: audio, Pluto TV
and free news channels, 1-bit video. YouTube does not work there (YouTube blocks cloud IPs). Hobby plan requests last at most 300 s;
the app re-requests live streams automatically. Without a gateway key, chat falls back to the free Pollinations service.

### Stream queue (v2.6)
Starting a stream stops the old one, frees its buffers and waits ~320 ms; skipping quickly (Left/Right/Up/Down) collapses into one request.

### Dictation and episode search (v2.6)
**Voice:** Search menu (home) -> "Speak a search", or press **Left** in AI chat. Press A, speak, it stops when you pause; the relay transcribes it
(`/stt`; Whisper on Cloudflare Workers AI's free tier when `CF_ACCOUNT_ID` + `CF_API_TOKEN` (Workers AI permission) are set on the relay, otherwise through the Vercel AI Gateway with `AI_GATEWAY_API_KEY`) and you confirm what it heard before it is used.
**Episodes:** Podcasts -> "Search episodes" finds episodes across every show; every episode list starts with "Search this show" (title filter over the
newest part of the feed) and "Speak to search".

## Install
Sideload `dist/Airwave.pdx.zip` at play.date/account/sideload (it has the device binary `pdex.bin` and the simulator library `pdex.so`). Allow internet access on first launch.

## Controls
D-pad or crank to move, **A** select, **B** back. On TV rows **A** watches, **Right** = audio only. While playing, **Up/Down** = next/previous.
Text entry is the standard Playdate keyboard (OK submits, B deletes).

## Build
```bash
export PLAYDATE_SDK_PATH=~/PlaydateSDK
mkdir build_dev && (cd build_dev && cmake -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake -DCMAKE_BUILD_TYPE=Release .. && make)
mkdir build && (cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make)        # airwave.pdx with device + simulator code
python3 tools/make_art.py                                                       # regenerates the launcher artwork
```
Source layout: `src/main.c` app logic, `src/ui.c` all drawing (carousel, animated lists, cards, cover art, icons), `src/net.c` HTTP, `src/parse.c` JSON/RSS, `Source/main.lua` loop + keyboard.

## If it crashes on a real Playdate
Set `"debug": true` in `Data/<bundle>/settings.json`; Airwave then writes breadcrumbs to `airwave.log` beside it (the last line is the last thing that happened).
An earlier "Error accessing buffer at 0x000000.." was a null Wi-Fi callback plus multi-kilobyte stack frames; both are fixed (largest frame now ~312 bytes).

## Status
Tested in the Playdate Simulator 3.1.2 (home carousel, radio, BBC World Service, TuneIn direct, Library incl. archive.org playback, podcasts, search,
system keyboard + chat bubbles, Setup + relay test, relay video from Free live news). **Not tested on hardware.** 1-bit video is rough on fast action; A/V sync is approximate.
