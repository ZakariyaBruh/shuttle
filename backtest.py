"""Replay the held-out test days: trade the model's BUY/SELL calls (with fees) and compare to buy-and-hold.
Run `python backtest.py --baselines` for model-free reference lines, `python backtest.py outputs/cryptobro-lora` for the model."""
import argparse
import re

import numpy as np

from build_dataset import FEE
from cryptobro.data import read_jsonl


def run(pos, rets, fee=FEE):
    pos, rets = np.asarray(pos), np.asarray(rets)
    flips = np.abs(np.diff(np.r_[0, pos]))
    log = pos * np.log1p(rets) + flips * np.log1p(-fee)
    eq = np.exp(np.cumsum(log))
    dd = (eq / np.maximum.accumulate(eq) - 1).min()
    return eq[-1] - 1, dd, int(flips.sum())


def report(name, pos, rets):
    r, dd, n = run(pos, rets)
    print(f"{name:<22} return {r:+8.1%}   max drawdown {dd:7.1%}   trades {n}")


ap = argparse.ArgumentParser()
ap.add_argument("adapter", nargs="?")
ap.add_argument("--split", default="test")
ap.add_argument("--threshold", type=float, default=0.5)
ap.add_argument("--baselines", action="store_true")
a = ap.parse_args()

rows = read_jsonl(f"data/{a.split}.jsonl")
rets = [r["day_return"] for r in rows]
y = np.array([r["answer"] == "BUY" for r in rows], dtype=int)
print(f"{a.split}: {rows[0]['date']} -> {rows[-1]['date']} ({len(rows)} days, fee {FEE:.1%}/trade)\n")
report("buy & hold", np.ones(len(rows), dtype=int), rets)
report("hindsight oracle (cap)", y, rets)
if a.baselines:
    up = [float(re.search(r"\(([+-][\d.]+)% on the day", r["prompt"]).group(1)) > 0 for r in rows]
    report("yesterday-up momentum", np.array(up, dtype=int), rets)
if a.adapter:
    from cryptobro.model import load_with_adapter, p_buy

    model, tok = load_with_adapter(a.adapter)
    p = np.array(p_buy(model, tok, [r["prompt"] for r in rows]))
    pos = (p > a.threshold).astype(int)
    report("CryptoBro model", pos, rets)
    print(f"\nlabel accuracy {np.mean(pos == y):.1%} (always-BUY: {y.mean():.1%}); BUY signals {pos.mean():.0%} of days")
