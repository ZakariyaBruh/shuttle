# Airwave for Playdate

Radio, podcasts, TV and an AI chat on the Playdate, written in C against the Playdate SDK 3.1. Streams MP3 straight from the
network (`fileplayer->setMP3StreamSource`) and draws dithered video straight into the framebuffer. UI font: Roobert (built into the Playdate).
Text entry uses the **system keyboard**, which the Playdate only offers to Lua, so there is a tiny `Source/main.lua` that loads
`CoreLibs/keyboard` and drives the frame loop; everything else is C. While the keyboard slides in, the UI re-lays itself out in the free
width (the AI chat transcript re-wraps and the input box rides above the keyboard).

| Tile | What | Needs relay? |
|---|---|---|
| Search | type once (on-screen keyboard), then pick radio / podcasts / YouTube / Internet Archive / TuneIn / your IPTV | YouTube, Archive, TuneIn, IPTV: yes |
| Radio | [radio-browser.info](https://www.radio-browser.info): top, genres, countries, search | only for non-MP3 stations (marked `*`) |
| Podcasts | Apple top charts + iTunes search; episodes from the show's RSS feed | only for AAC/M4A episodes and some tracking redirects |
| BBC | Radio 1 to 6 Music, 4 Extra, World Service, nations | yes |
| TV | **real live channels (Pluto TV, ~450 free channels)**, documentary channels (YouTube), **YouTube Shorts**, free live news, Internet Archive classic TV, your own IPTV list, YouTube search | yes |
| TuneIn | trending + search | yes |
| AI Chat | [pollinations.ai](https://pollinations.ai) text API, no key; remembers the last few turns | no |
| Setup | shows Wi-Fi and relay status | |

### About the Nat Geo Wild channel
The Nat Geo Wild *TV channel* is pay-TV only (Disney), and I found no legal free live stream of it: I checked Pluto TV's whole
lineup (467 channels) and it isn't there. What you get instead:
- **Live TV channels (Pluto TV)**: real 24/7 channels such as BBC Earth, PBS Nature, Love Nature, Naturescape, Dinos 24/7, plus news, movies,
  classic TV, kids, sport... searchable by name or browsable by category. Pluto's licensing is regional: if you see a "where to watch" card
  the channel isn't available in your country (that is what I got from my environment, so I could not verify real Pluto *content*).
- **Documentary channels (YouTube)**: official full-episode channels such as National Geographic, BBC Earth, DW Documentary, Smithsonian Channel and NASA.
- If you have a TV subscription, the channel is in the Nat Geo app / Disney+ (they need your login, which a Playdate can't do).

### Channel surfing and Shorts
In any video or station list, press **Up/Down while watching** to jump to the previous/next channel, short or station, TikTok style.
**YouTube Shorts** (Funny, Funny animals, Cats, Fails, Wildlife, Nat Geo, or search a word/#tag) come from the hashtag/channel Shorts pages.
Shorts are vertical, so they are shown whole with side bars; the relay also supports `?fit=cover` (fill the screen, cropping the edges).

## Why a relay?
The Playdate can only decode MP3, has no HLS/AAC support, and its 1-bit screen needs video converted first. `relay/relay.py` is a
small Python + ffmpeg server that
- turns streams into 64 kbps mono MP3 and follows tracking redirects; for HLS it picks the *smallest* variant (a 400x240 1-bit screen doesn't need 1080p),
- **dithers video to 1 bit**: 400x240, 15 fps, ordered dithering with interleaved-gradient noise (crisper than Bayer, no visible grid,
  and a *static* pattern so still areas stay still and frame deltas stay tiny), after stabilised auto-contrast, light denoise and
  sharpening; sends XOR-delta + PackBits frames with 48 kbps audio (about 20-80 KB/s),
- resolves YouTube (yt-dlp), Internet Archive and TuneIn, reads your M3U playlists.

No hosted service can do the dithering for you (it needs ffmpeg; Cloudflare Workers and similar can't run it). What Cloudflare *does*
offer with no account or key is a tunnel so the Playdate can reach your relay from anywhere (below).

```bash
pip install -r relay/requirements.txt          # numpy (+ yt-dlp and node/deno for YouTube); ffmpeg must be on PATH
python3 relay/relay.py --host 0.0.0.0 --m3u https://example.com/your-playlist.m3u
```
`--host 0.0.0.0` makes it reachable from the Playdate on your Wi-Fi (default is localhost only, enough for the Simulator).
It fetches whatever URL it is asked to, so only expose it on networks you trust (add `--token SECRET` to require a key).
Then edit `Data/<bundle>/settings.json` on the Playdate (mount it via Settings > System > Reboot to Data Disk, or the Simulator's Data folder):
```json
{ "relay": "192.168.1.20:8787", "relay_token": "" }
```

### From anywhere: Cloudflare quick tunnel (no account, no key)
```bash
python3 relay/relay.py --token SOMETHING-LONG
cloudflared tunnel --url http://localhost:8787        # prints https://<random>.trycloudflare.com
```
```json
{ "relay": "https://<random>.trycloudflare.com", "relay_token": "SOMETHING-LONG" }
```
Always use `--token` here: the tunnel URL is public. (Tunnels are untested by me; the app accepts any `http(s)://` relay address.)

### YouTube
yt-dlp needs a JavaScript runtime (`node` or `deno`) on the relay machine. If YouTube answers "sign in to confirm you're not a bot"
(common from servers/VPNs, not usually from home), export a `cookies.txt` and start the relay with `--yt-cookies cookies.txt`.

## Install
Sideload `dist/Airwave.pdx.zip` at [play.date/account/sideload](https://play.date/account/sideload) (or unzip and open `airwave.pdx`
in the Simulator). It contains the device binary (`pdex.bin`) and the simulator library (`pdex.so`). Allow internet access on first launch.
(The app was renamed from "Tuner": new bundle id `com.zakariya.airwave`, so settings from the old app aren't carried over.)

## Controls
D-pad or crank to move, **A** select, **B** back. On TV lists: **A** watch, **Right** audio only; Up/Down while watching = next/previous.
Text entry: the standard Playdate keyboard (**OK** submits, **B** deletes, cancel/B on empty closes it).

## Build
```bash
export PLAYDATE_SDK_PATH=~/PlaydateSDK
mkdir build_dev && (cd build_dev && cmake -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake -DCMAKE_BUILD_TYPE=Release .. && make)
mkdir build && (cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make)     # produces airwave.pdx with device + simulator code
```
Needs `gcc-arm-none-eabi` for the device build.

## Status / known limits
- Tested in the Playdate Simulator 3.1.2: home grid, radio, podcasts (incl. redirect fallback), BBC, TuneIn, search, keyboard, AI chat,
  dithered video with audio from your IPTV list, free live news (DW, Al Jazeera) with channel surfing, Internet Archive, the Pluto TV lists, and the relay token. **Not tested on real
  hardware** (the ARM build links and packages; Wi-Fi behaviour and memory use on the device are unverified).
- **YouTube playback (videos and Shorts) could not be verified**: from my environment YouTube blocks yt-dlp with a bot check, so I only verified the
  channel/hashtag/search *listings* (documentary-channel episodes and funny Shorts list correctly). On a normal home connection it usually works; if not, see above.
- Pollinations' anonymous tier is rate-limited and its model/output can change; replies drop emoji (the font is Latin-only).
- 1-bit video is rough on fast action; audio/video sync is approximate (fixed 1.5 s offset).
- Streams are MP3 only (the Playdate's decoder); many stations and podcast hosts only publish AAC, hence the relay.
