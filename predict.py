"""Today's call: pulls yesterday's candle, Fear & Greed and headlines (RSS), then asks the model BUY or SELL.
`--date YYYY-MM-DD` replays a past day using the historical headline dataset instead."""
import argparse
from datetime import datetime, timedelta, timezone

import pandas as pd

from cryptobro.data import build_prompt, fetch_fng, fetch_news_live, fetch_prices, load_news_history, pick_headlines

ap = argparse.ArgumentParser()
ap.add_argument("adapter", nargs="?", help="LoRA adapter dir (omit with --show-prompt)")
ap.add_argument("--date")
ap.add_argument("--threshold", type=float, default=0.5)
ap.add_argument("--show-prompt", action="store_true")
a = ap.parse_args()

day = pd.Timestamp(a.date) if a.date else pd.Timestamp(datetime.now(timezone.utc).date())
prev = day - timedelta(days=1)
prices, fng = fetch_prices(), fetch_fng()
if a.date:
    news = load_news_history()
    news = news[news.date == prev]
else:
    news = fetch_news_live(prev)
prompt = build_prompt(day, prices, fng, pick_headlines(news))
if prompt is None:
    raise SystemExit(f"No complete price history for {prev.date()}.")
print(prompt, "\n")
if a.adapter and not a.show_prompt:
    from cryptobro.model import load_with_adapter, p_buy

    model, tok = load_with_adapter(a.adapter)
    p = p_buy(model, tok, [prompt])[0]
    print(f"==> {'BUY / stay invested' if p > a.threshold else 'SELL / stay in cash'}  (P(buy) = {p:.0%})")
