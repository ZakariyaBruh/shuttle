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
    "You are CryptoBro, a Bitcoin trading assistant. You are told what happened "
    "yesterday and must decide the position for today. Answer BUY if we should "
    "hold Bitcoin today, or SELL if we should be in cash today."
)


def read_jsonl(path):
    return [json.loads(l) for l in open(path)]


def fetch_prices(start="2018-06-01"):
    """Daily BTCUSDT candles (UTC). Today's unfinished candle is dropped."""
    CACHE.mkdir(parents=True, exist_ok=True)
    t = int(pd.Timestamp(start, tz="UTC").timestamp() * 1000)
    rows = []
    while True:
        r = requests.get(KLINES, params=dict(symbol="BTCUSDT", interval="1d", startTime=t, limit=1000), timeout=30)
        r.raise_for_status()
        batch = r.json()
        if not batch:
            break
        rows += batch
        t = batch[-1][0] + 86_400_000
        if len(batch) < 1000:
            break
        time.sleep(0.2)
    df = pd.DataFrame(rows).iloc[:, :6]
    df.columns = ["ts", "open", "high", "low", "close", "volume"]
    df["date"] = pd.to_datetime(df.ts, unit="ms").dt.normalize()
    df = df.set_index("date").drop(columns="ts").astype(float)
    today = pd.Timestamp(datetime.now(timezone.utc).date())
    return df[df.index < today]


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


def build_prompt(day, prices, fng, headlines):
    """Prompt for deciding day `day` (a date); uses only data up to day-1."""
    p = pd.Timestamp(day).normalize() - timedelta(days=1)
    h = prices.loc[:p]
    if len(h) < 91 or h.index[-1] != p:
        return None
    c = h.close
    last = h.iloc[-1]
    pct = lambda a, b: f"{(a / b - 1) * 100:+.1f}%"
    vol_ratio = last.volume / h.volume.iloc[-31:-1].mean()
    lines = [
        f"Date today: {pd.Timestamp(day).date()}. Summary of yesterday ({p.date()}):",
        f"- BTC closed at ${last.close:,.0f} ({pct(last.close, last.open)} on the day, range {pct(last.high, last.low)}).",
        f"- Volume was {vol_ratio:.1f}x the 30-day average.",
        f"- Trend: {pct(c.iloc[-1], c.iloc[-8])} over 7 days, {pct(c.iloc[-1], c.iloc[-31])} over 30 days, "
        f"{pct(c.iloc[-1], c.rolling(50).mean().iloc[-1])} vs 50-day average, {pct(c.iloc[-1], c.iloc[-90:].max())} from the 90-day high.",
    ]
    if p in fng.index:
        f = fng.loc[p]
        lines.append(f"- Fear & Greed index: {f.value} ({f.value_classification}).")
    lines.append("- Headlines:" + ("" if headlines else " none found."))
    lines += [f"  * {t}" for t in headlines]
    lines.append("Should we hold Bitcoin today? Answer BUY or SELL.")
    return "\n".join(lines)
