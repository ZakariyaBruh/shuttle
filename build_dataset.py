"""Build train/val/test JSONL: one example per day. Prompt = yesterday only; answer = hindsight-best buy/sell times today."""
import json
import sys
from pathlib import Path

import pandas as pd

from cryptobro.data import TZ, best_trade, build_prompt, fetch_fng, format_answer, load_market, load_news_history, pick_headlines

SPLITS = {"train": ("2019-11-01", "2024-03-31"), "val": ("2024-04-01", "2024-06-30"), "test": ("2024-07-01", "2100-01-01")}  # news data ends 2025-02
OUT = Path("data")

if __name__ == "__main__":
    tz = sys.argv[1] if len(sys.argv) > 1 else TZ
    intr, daily = load_market(tz)
    fng, news = fetch_fng(), load_news_history()
    lo, hi = news.date.min() + pd.Timedelta(days=1), min(news.date.max(), daily.index.max())
    by_day = {d: g for d, g in news.groupby("date")}
    OUT.mkdir(exist_ok=True)
    for name, (a, b) in SPLITS.items():
        n, profits = 0, []
        with open(OUT / f"{name}.jsonl", "w") as f:
            for d in daily.index:
                if not (max(pd.Timestamp(a), lo) <= d <= min(pd.Timestamp(b), hi)):
                    continue
                prompt = build_prompt(d, daily, fng, pick_headlines(by_day.get(d - pd.Timedelta(days=1))), intr)
                if prompt is None:
                    continue
                day = intr.loc[d : d + pd.Timedelta(hours=23, minutes=59)]
                t = best_trade(day)
                f.write(json.dumps(dict(
                    date=str(d.date()), prompt=prompt, answer=format_answer(t["buy"], t["sell"], t["profit"]),
                    best=dict(buy=str(t["buy"]), sell=str(t["sell"]), profit=t["profit"]),
                    opens=day.open.round(2).tolist(), closes=day.close.round(2).tolist(),
                )) + "\n")
                profits.append(t["profit"])
                n += 1
        print(f"{name}: {n} days, best possible same-day profit avg {sum(profits) / max(n, 1):.2%}")
