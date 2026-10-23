"""Data sources and the prompt builder shared by training and live use.

Every prompt for trading day D is built ONLY from information that exists by
00:00 UTC on day D, i.e. the fully closed candle, Fear & Greed value and
headlines of day D-1. Training and live use call the same `build_prompt`.
"""
import html
import json
import re
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pandas as pd
import requests

CACHE = Path(__file__).resolve().parent.parent / "data" / "cache"
KLINES = "https://data-api.binance.vision/api/v3/klines"
NEWS_REPO = "maryamfakhari/crypto-news-coindesk-2020-2025"
NEWS_FILE = "coindesk-crypto-news-2020-2025.csv"
RSS_FEEDS = [
    "https://www.coindesk.com/arc/outboundfeeds/rss/",
    "https://cointelegraph.com/rss",
    "https://decrypt.co/feed",
]

SYSTEM = (
    "You are CryptoBro, a Bitcoin day-trading assistant. You are told what happened "
    "yesterday and must plan ONE round trip for today: buy at one time, sell later the same day. "
    "Reply in exactly this format:\nBUY HERE: HH:MM AM/PM\nSELL HERE: HH:MM AM/PM\nPROFIT PREDICTION: X.XX%"
)
TZ = "UTC"  # trading-day timezone; must be UTC or west of it (see load_market)
FEE = 0.001  # per side
STEP = 15  # minutes per candle


def read_jsonl(path):
    return [json.loads(l) for l in open(path)]


def fetch_intraday(start="2019-09-01"):
    """15-minute BTCUSDT candles in UTC. The unfinished current candle is dropped."""
    CACHE.mkdir(parents=True, exist_ok=True)
    cache = CACHE / "btc_15m.csv.gz"
    old = pd.read_csv(cache, index_col=0, parse_dates=True) if cache.exists() else None
    t = int(((old.index[-1] + pd.Timedelta(minutes=STEP)) if old is not None else pd.Timestamp(start)).tz_localize("UTC").timestamp() * 1000)
    rows = []
    while t < time.time() * 1000:
        r = requests.get(KLINES, params=dict(symbol="BTCUSDT", interval="15m", startTime=t, limit=1000), timeout=30)
        r.raise_for_status()
        batch = r.json()
        if not batch:
            break
        rows += batch
        t = batch[-1][0] + STEP * 60_000
        time.sleep(0.1)
    if rows:
        df = pd.DataFrame(rows).iloc[:, :6]
        df.columns = ["ts", "open", "high", "low", "close", "volume"]
        df.index = pd.to_datetime(df.pop("ts"), unit="ms")
        df = df.astype(float)
        df = df[df.index + pd.Timedelta(minutes=STEP) <= pd.Timestamp.now(tz="UTC").tz_localize(None)]
        old = df if old is None else pd.concat([old, df])
        old = old[~old.index.duplicated()]
        old.to_csv(cache)
    return old


def load_market(tz=TZ):
    """(intraday, daily) with timestamps in local `tz` days. Daily = the same candles resampled, so they agree."""
    intr = fetch_intraday()
    if tz != "UTC":
        if pd.Timestamp.now(tz=tz).utcoffset() > pd.Timedelta(0):
            raise ValueError("Timezones east of UTC would let UTC-dated news leak into the day's start; use UTC or a western zone.")
        intr = intr.tz_localize("UTC").tz_convert(tz).tz_localize(None)
    daily = intr.resample("D").agg(dict(open="first", high="max", low="min", close="last", volume="sum"))
    full = intr.groupby(intr.index.normalize()).size() == 24 * 60 // STEP
    return intr, daily[full.reindex(daily.index, fill_value=False)]


def best_trade(day):
    """Hindsight best single round trip inside one day's candles: buy at a candle open, sell at a later candle close."""
    o, c = day.open.values, day.close.values
    best = (-1.0, 0, 0)
    lo = 0
    for j in range(len(c)):
        if o[j] < o[lo]:
            lo = j
        p = c[j] / o[lo] * (1 - FEE) ** 2 - 1
        if p > best[0]:
            best = (p, lo, j)
    p, i, j = best
    return dict(profit=p, buy=day.index[i], sell=day.index[j] + pd.Timedelta(minutes=STEP))


def trade_profit(day, buy, sell):
    """Realised profit of buying at `buy` and selling at `sell` (both on the candle grid); None if invalid."""
    if sell <= buy:
        return None
    try:
        o = day.open.loc[buy]
        c = day.close.loc[sell - pd.Timedelta(minutes=STEP)]
    except KeyError:
        return None
    return c / o * (1 - FEE) ** 2 - 1


def fmt_time(ts):
    return pd.Timestamp(ts).strftime("%I:%M %p")


def format_answer(buy, sell, profit):
    return f"BUY HERE: {fmt_time(buy)}\nSELL HERE: {fmt_time(sell)}\nPROFIT PREDICTION: {profit * 100:.2f}%"


def parse_answer(text):
    m = re.search(r"BUY HERE:\s*(\d{1,2}:\d{2}\s*[AP]M).*?SELL HERE:\s*(\d{1,2}:\d{2}\s*[AP]M).*?PROFIT PREDICTION:\s*(-?[\d.]+)%", text, re.S | re.I)
    if not m:
        return None
    t = lambda s: pd.to_datetime(s.upper().replace("  ", " "), format="%I:%M %p")
    return dict(buy=t(m[1]), sell=t(m[2]), profit=float(m[3]) / 100)


def fetch_fng():
    r = requests.get("https://api.alternative.me/fng/?limit=0", timeout=30)
    r.raise_for_status()
    d = pd.DataFrame(r.json()["data"])
    d["date"] = pd.to_datetime(d.timestamp.astype(int), unit="s").dt.normalize()
    return d.set_index("date").sort_index()[["value", "value_classification"]].astype({"value": int})


def _clean(t):
    return re.sub(r"\s+", " ", html.unescape(str(t))).strip()


def load_news_history():
    """Historical timestamped headlines (2019-2025) used for training/backtests."""
    out = CACHE / "news.csv.gz"
    if out.exists():
        return pd.read_csv(out, parse_dates=["date"])
    from huggingface_hub import hf_hub_download

    p = hf_hub_download(NEWS_REPO, NEWS_FILE, repo_type="dataset")
    d = pd.read_csv(p, usecols=["published_on", "title", "categories", "tags"])
    d["date"] = pd.to_datetime(d.published_on, errors="coerce").dt.normalize()
    d["title"] = d.title.map(_clean)
    cat = d.categories.fillna("") + "|" + d.tags.fillna("")
    d["btc"] = cat.str.contains(r"BTC|Bitcoin", case=False, regex=True) | d.title.str.contains("bitcoin|btc", case=False)
    d = d.dropna(subset=["date", "title"])[["date", "title", "btc"]]
    CACHE.mkdir(parents=True, exist_ok=True)
    d.to_csv(out, index=False)
    return d


def fetch_news_live(day):
    """Headlines published on UTC `day` from public RSS feeds (what you can get in the real world)."""
    import calendar

    import feedparser

    day = pd.Timestamp(day).normalize()
    rows = []
    for url in RSS_FEEDS:
        for e in feedparser.parse(url).entries:
            ts = e.get("published_parsed")
            if not ts:
                continue
            d = pd.Timestamp(datetime.fromtimestamp(calendar.timegm(ts), timezone.utc).date())
            if d == day:
                t = _clean(e.title)
                rows.append((day, t, bool(re.search("bitcoin|btc", t, re.I))))
    return pd.DataFrame(rows, columns=["date", "title", "btc"])


def pick_headlines(news_day, n=6):
    if news_day is None or len(news_day) == 0:
        return []
    nd = news_day.drop_duplicates("title")
    nd = pd.concat([nd[nd.btc], nd[~nd.btc]])
    seen, out = set(), []
    for t in nd.title:
        k = t.lower()[:40]
        if k not in seen:
            seen.add(k)
            out.append(t[:140])
        if len(out) == n:
            break
    return out


def build_prompt(day, daily, fng, headlines, intr):
    """Prompt for trading day `day` (a date); uses only data up to the end of day-1."""
    p = pd.Timestamp(day).normalize() - timedelta(days=1)
    h = daily.loc[:p]
    if len(h) < 91 or h.index[-1] != p:
        return None
    c = h.close
    last = h.iloc[-1]
    pct = lambda a, b: f"{(a / b - 1) * 100:+.1f}%"
    vol_ratio = last.volume / h.volume.iloc[-31:-1].mean()
    y = intr.loc[p : p + pd.Timedelta(hours=23, minutes=59)]
    lines = [
        f"Date today: {pd.Timestamp(day).date()}. Summary of yesterday ({p.date()}):",
        f"- BTC closed at ${last.close:,.0f} ({pct(last.close, last.open)} on the day, range {pct(last.high, last.low)}).",
        f"- The day's low was around {fmt_time(y.low.idxmin())} and the high around {fmt_time(y.high.idxmax())}.",
        f"- Volume was {vol_ratio:.1f}x the 30-day average.",
        f"- Trend: {pct(c.iloc[-1], c.iloc[-8])} over 7 days, {pct(c.iloc[-1], c.iloc[-31])} over 30 days, "
        f"{pct(c.iloc[-1], c.rolling(50).mean().iloc[-1])} vs 50-day average, {pct(c.iloc[-1], c.iloc[-90:].max())} from the 90-day high.",
    ]
    if p in fng.index:
        f = fng.loc[p]
        lines.append(f"- Fear & Greed index: {f.value} ({f.value_classification}).")
    lines.append("- Headlines:" + ("" if headlines else " none found."))
    lines += [f"  * {t}" for t in headlines]
    lines.append("Plan today's single trade: when to buy, when to sell, and the profit you expect.")
    return "\n".join(lines)
