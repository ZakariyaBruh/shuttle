# Tuner for Playdate

Internet radio, Apple Podcasts, BBC radio, TuneIn and dithered IPTV video on the Playdate, written in C against the Playdate SDK 3.1
(uses the SDK's HTTP client and `fileplayer->setMP3StreamSource` to stream MP3 straight from the network).

| Menu | Source | Needs relay? |
|---|---|---|
| Radio | [radio-browser.info](https://www.radio-browser.info) (top, by genre/country, search) | only for non-MP3 stations (marked `*`) |
| Podcasts (Apple) | Apple top charts + iTunes Search/Lookup API, episodes from the show's RSS feed | only for AAC/M4A episodes, and for hosts the Playdate can't follow redirects through |
| BBC Radio | Radio 1-6 Music, 4 Extra, World Service, nations (live HLS, found through radio-browser) | yes |
| TuneIn radio | TuneIn OPML directory (trending, search) | yes |
| IPTV (video) | channels from your own M3U playlist: search, groups, paging; **dithered 1-bit video + audio** | yes |
| Search | type once, then choose radio / podcasts / TuneIn / IPTV | IPTV and TuneIn need the relay |

**Why a relay?** The Playdate can only decode MP3, has no HLS/AAC support, and its 1-bit screen needs video converted first.
`relay/relay.py` is a small Python+ffmpeg server that turns those streams into 64 kbps mono MP3, follows podcast tracking redirects
server-side, and for IPTV **dithers the video to 1 bit** (ordered/Bayer dither, 400x240, 15 fps) and streams it together with
48 kbps audio, about 20-60 KB/s in total. The app draws the frames straight into the Playdate framebuffer. No hosted service can
do this for you: it needs ffmpeg doing the conversion, and Cloudflare Workers and similar can't run it. What Cloudflare *does*
offer, with no account or key, is a tunnel so the Playdate can reach the relay from anywhere (see below).

In the IPTV list, **A** plays video, **Right** plays audio only. A channel with no picture falls back to audio by itself.

## Install
Sideload `dist/Tuner.pdx.zip` at [play.date/account/sideload](https://play.date/account/sideload) (or unzip and open `tuner.pdx` in
the Simulator). It contains both the device binary (`pdex.bin`) and the simulator library (`pdex.so`).
On first launch allow internet access.

## Relay (optional but recommended)
```bash
python3 relay/relay.py --host 0.0.0.0 --m3u https://example.com/your-playlist.m3u   # needs ffmpeg
```
`--host 0.0.0.0` makes it reachable from the Playdate on your Wi-Fi (default is localhost only, which is enough for the Simulator).
Only expose it on a network you trust: it will fetch whatever URL it is asked to (add `--token SECRET` to require a key). Then edit `Data/<bundle>/settings.json` on the
Playdate (mount it via Settings > System > Reboot to Data Disk, or edit it in the Simulator's Data folder):
```json
{ "relay": "192.168.1.20:8787", "relay_token": "" }
```

### From anywhere: Cloudflare quick tunnel (no account, no key)
```bash
python3 relay/relay.py --token SOMETHING-LONG --m3u https://example.com/list.m3u
cloudflared tunnel --url http://localhost:8787        # prints https://<random>.trycloudflare.com
```
```json
{ "relay": "https://<random>.trycloudflare.com", "relay_token": "SOMETHING-LONG" }
```
Always use `--token` here: the tunnel URL is public. (Tunnels are untested by me; the app accepts any `http(s)://` relay address.)

## Controls
D-pad or crank to move, **A** select, **B** back, **Right** audio-only on IPTV channels. In search: Up/Down or crank to choose a letter, Right adds it, Left deletes, A searches.

## Build
```bash
export PLAYDATE_SDK_PATH=~/PlaydateSDK
mkdir build_dev && (cd build_dev && cmake -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake -DCMAKE_BUILD_TYPE=Release .. && make)
mkdir build && (cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make)     # produces tuner.pdx with device + simulator code
```
Needs `gcc-arm-none-eabi` for the device build.

## Status / known limits
- Tested in the Playdate Simulator 3.1.2 (radio, podcasts incl. redirect fallback, BBC, IPTV video with audio, TuneIn, search, token). **Not tested on
  real hardware**: I only had the Simulator and the ARM build links and packages, but memory use and Wi-Fi behaviour on-device are unverified.
- The Playdate HTTP client follows some redirects itself and drops very long `Location` headers; the app follows what it can and
  falls back to the relay otherwise.
- IPTV video is 1-bit dithered by design; fast action is rough. Audio/video sync is approximate (fixed 1.5 s offset). Only the Simulator's timing is verified.
- Streams are MP3 only (the Playdate's decoder); many stations and podcast hosts only publish AAC, hence the relay.
- Titles are shown in plain Latin (accents stripped, CJK replaced by `?`) because the system font is Latin-only.
