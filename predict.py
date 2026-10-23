"""Today's plan: yesterday's candles, Fear & Greed and headlines (RSS) -> BUY HERE / SELL HERE / PROFIT PREDICTION.
`--date YYYY-MM-DD` replays a past day (historical headlines) and lets the teacher grade it."""
import argparse
from datetime import datetime, timedelta, timezone

import pandas as pd

from cryptobro.data import TZ, build_prompt, fetch_fng, fetch_news_live, load_market, load_news_history, pick_headlines

ap = argparse.ArgumentParser()
ap.add_argument("adapter", nargs="?", help="LoRA adapter dir or Hub repo (omit with --show-prompt)")
ap.add_argument("--date")
ap.add_argument("--tz", default=TZ)
ap.add_argument("--show-prompt", action="store_true")
a = ap.parse_args()

day = pd.Timestamp(a.date) if a.date else pd.Timestamp(datetime.now(timezone.utc).date())
prev = day - timedelta(days=1)
intr, daily = load_market(a.tz)
if a.date:
    news = load_news_history()
    news = news[news.date == prev]
else:
    news = fetch_news_live(prev)
prompt = build_prompt(day, daily, fetch_fng(), pick_headlines(news), intr)
if prompt is None:
    raise SystemExit(f"No complete price history for {prev.date()}.")
print(prompt, "\n")
if a.adapter and not a.show_prompt:
    from cryptobro.model import generate, load_with_adapter

    model, tok = load_with_adapter(a.adapter)
    out = generate(model, tok, [prompt])[0]
    print(out)
    if a.date:  # past day: teacher grades it
        from cryptobro.data import best_trade, format_answer
        from cryptobro.teacher import feedback, grade

        d = intr.loc[day : day + pd.Timedelta(hours=23, minutes=59)]
        t = best_trade(d)
        row = dict(date=str(day.date()), opens=d.open.tolist(), closes=d.close.tolist(), best=dict(buy=str(t["buy"]), sell=str(t["sell"]), profit=t["profit"]))
        print("\nTeacher:\n" + feedback(grade(out, row)))
