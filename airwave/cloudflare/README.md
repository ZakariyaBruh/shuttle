# Hosting the relay on Cloudflare

**Status: untested.** I couldn't deploy to a Cloudflare account from where I work, so treat this as a ready-to-run starting point.

## What can and can't run on Cloudflare
- **Workers** (the free, keyless-ish tier) cannot run ffmpeg, so they can't convert audio or dither video. They are not enough for the relay.
- **Cloudflare Containers** can run the relay's Docker image (`../relay/Dockerfile`), but need a Cloudflare account on the **Workers Paid plan**.
- **Quick tunnels** (`cloudflared tunnel --url ...`) need **no account and no key**, but only publish a relay that is already running
  on some computer: see `../relay/start.sh --tunnel`.

## Deploy as a container
```bash
cd cloudflare
npm install
npx wrangler login                      # free account sign-up is in the browser; Containers needs Workers Paid
npx wrangler secret put AIRWAVE_TOKEN   # choose a long random string
npx wrangler deploy                     # builds ../relay/Dockerfile with local Docker and uploads it
```
Wrangler prints `https://airwave-relay.<you>.workers.dev`. In Airwave: **Setup > Relay address** = that URL, **Relay token** = your secret.
Use **Setup > Test the relay** to check it.

## Things to know before you rely on it
- **Where it runs:** Containers start in a Cloudflare data centre chosen by Cloudflare, not one you pick, and cold-start after idling.
- **Pluto TV and others may refuse data-centre IPs.** From a Google Cloud address I got Pluto's "where to watch" card even though the
  address was in the US, so a Cloudflare container may behave the same. A relay on a home connection is the most reliable for live TV.
- **Cost:** Containers bills for the time it runs. `sleepAfter` keeps it idle-cheap.
