"""Build train/val/test JSONL files: one example per day, prompt from day D-1, label = profit-optimal position for day D."""
import json
from pathlib import Path

import numpy as np
import pandas as pd

from cryptobro.data import build_prompt, fetch_fng, fetch_prices, load_news_history, pick_headlines

FEE = 0.001  # per side (0.1%), charged whenever the position flips
SPLITS = {"train": ("2019-11-01", "2024-03-31"), "val": ("2024-04-01", "2024-06-30"), "test": ("2024-07-01", "2100-01-01")}  # news data ends 2025-02
OUT = Path("data")


def optimal_positions(day_ret, fee=FEE):
    """Hindsight-optimal long/cash path (Viterbi) that maximises total log return after fees."""
    n = len(day_ret)
    cost = np.log1p(-fee)
    score = np.array([0.0, cost + np.log1p(day_ret[0])])
    back = np.zeros((n, 2), dtype=int)
    for i in range(1, n):
        r = np.log1p(day_ret[i])
        stay0, from1 = score[0], score[1] + cost
        stay1, from0 = score[1], score[0] + cost
        back[i, 0] = 0 if stay0 >= from1 else 1
        back[i, 1] = 1 if stay1 >= from0 else 0
        score = np.array([max(stay0, from1), max(stay1, from0) + r])
    s = int(score.argmax())
    path = np.zeros(n, dtype=int)
    for i in range(n - 1, -1, -1):
        path[i] = s
        s = back[i, s]
    return path


if __name__ == "__main__":
    prices, fng, news = fetch_prices(), fetch_fng(), load_news_history()
    lo, hi = news.date.min() + pd.Timedelta(days=1), min(news.date.max(), prices.index.max())
    by_day = {d: g for d, g in news.groupby("date")}
    OUT.mkdir(exist_ok=True)
    for name, (a, b) in SPLITS.items():
        days = [d for d in prices.index if max(pd.Timestamp(a), lo) <= d <= min(pd.Timestamp(b), hi)]
        rets = (prices.loc[days].close / prices.loc[days].open - 1).values
        labels = optimal_positions(rets)
        n = 0
        with open(OUT / f"{name}.jsonl", "w") as f:
            for d, y, r in zip(days, labels, rets):
                prompt = build_prompt(d, prices, fng, pick_headlines(by_day.get(d - pd.Timedelta(days=1))))
                if prompt is None:
                    continue
                f.write(json.dumps(dict(date=str(d.date()), prompt=prompt, answer="BUY" if y else "SELL", day_return=float(r))) + "\n")
                n += 1
        print(f"{name}: {n} days, {labels.mean():.0%} labelled BUY")
