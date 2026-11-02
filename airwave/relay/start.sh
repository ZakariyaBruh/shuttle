#!/usr/bin/env bash
# Airwave relay launcher.
#   ./start.sh                 relay on your Wi-Fi (the Playdate must be on the same network)
#   ./start.sh --tunnel        also opens a free Cloudflare quick tunnel (no account, no key) so it works from anywhere,
#                              with a random token so nobody else can use it
# Extra arguments go to relay.py, e.g.  ./start.sh --tunnel --m3u https://example.com/list.m3u
set -euo pipefail
cd "$(dirname "$0")"
PORT="${PORT:-8787}"
TUNNEL=0; ARGS=()
for a in "$@"; do if [ "$a" = "--tunnel" ]; then TUNNEL=1; else ARGS+=("$a"); fi; done

command -v ffmpeg >/dev/null || { echo "ffmpeg is not installed (brew install ffmpeg / sudo apt install ffmpeg)"; exit 1; }
python3 -c "import numpy" 2>/dev/null || { echo "Installing numpy (video dithering)..."; python3 -m pip install --user numpy; }
python3 -c "import yt_dlp" 2>/dev/null || echo "Tip: pip install yt-dlp (and have node or deno) to enable YouTube."

TOKEN_ARGS=()
if [ "$TUNNEL" = 1 ]; then
  command -v cloudflared >/dev/null || { echo "cloudflared is not installed: https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/"; exit 1; }
  TOKEN="$(python3 -c 'import secrets;print(secrets.token_urlsafe(9))')"
  TOKEN_ARGS=(--token "$TOKEN")
fi

python3 relay.py --host 0.0.0.0 --port "$PORT" "${TOKEN_ARGS[@]}" "${ARGS[@]}" &
RELAY_PID=$!
trap 'kill $RELAY_PID 2>/dev/null; [ -n "${TUN_PID:-}" ] && kill $TUN_PID 2>/dev/null' EXIT
sleep 1

echo
echo "Airwave relay is running. In Airwave: Setup > Relay address, then type one of:"
for ip in $(hostname -I 2>/dev/null || ipconfig getifaddr en0 2>/dev/null); do echo "    $ip:$PORT        (same Wi-Fi)"; done

if [ "$TUNNEL" = 1 ]; then
  LOG="$(mktemp)"
  cloudflared tunnel --no-autoupdate --url "http://127.0.0.1:$PORT" >"$LOG" 2>&1 &
  TUN_PID=$!
  for _ in $(seq 1 40); do
    URL="$(grep -o 'https://[a-z0-9-]*\.trycloudflare\.com' "$LOG" | head -1 || true)"
    [ -n "$URL" ] && break; sleep 0.5
  done
  if [ -n "${URL:-}" ]; then
    echo "    $URL        (from anywhere)"
    echo "Then Setup > Relay token:   $TOKEN"
  else
    echo "Couldn't get a tunnel address; see $LOG"
  fi
fi
echo
wait $RELAY_PID
