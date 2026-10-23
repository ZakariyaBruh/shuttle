"""Grade the model on held-out days with the teacher. `--baselines` needs no GPU (fixed-time and hindsight reference lines).
   python evaluate.py outputs/cryptobro-lora [--split test] [--show 3]"""
import argparse

import numpy as np
import pandas as pd

from cryptobro.data import STEP, read_jsonl
from cryptobro.teacher import feedback, grade

ap = argparse.ArgumentParser()
ap.add_argument("adapter", nargs="?")
ap.add_argument("--split", default="test")
ap.add_argument("--show", type=int, default=3, help="print teacher feedback for this many days")
ap.add_argument("--baselines", action="store_true")
a = ap.parse_args()
rows = read_jsonl(f"data/{a.split}.jsonl")
print(f"{a.split}: {rows[0]['date']} -> {rows[-1]['date']} ({len(rows)} days)\n")


def summarize(name, texts):
    gs = [grade(t, r) for t, r in zip(texts, rows)]
    ok = [g for g in gs if g]
    real = np.array([g["realised"] if g and g["realised"] is not None else 0.0 for g in gs])
    print(f"{name:<26} avg/day {real.mean():+.3%}  win-days {np.mean(real > 0):.0%}  "
          f"buy off {np.mean([abs(g['buy_err']) for g in ok]) / 60:.1f}h  sell off {np.mean([abs(g['sell_err']) for g in ok]) / 60:.1f}h  "
          f"profit-pred off {np.mean([abs(g['profit_err']) for g in ok]) * 100:.2f}pts  compounded {np.prod(1 + real) - 1:+.0%}")
    return gs


fmt = lambda b, s, p: f"BUY HERE: {b}\nSELL HERE: {s}\nPROFIT PREDICTION: {p}%"
summarize("fixed 06:00 -> 21:00", [fmt("06:00 AM", "09:00 PM", "0.00")] * len(rows))
summarize("fixed 00:00 -> 23:45", [fmt("12:00 AM", "11:45 PM", "0.00")] * len(rows))
summarize("hindsight best (cap)", [r["answer"] for r in rows])
if a.adapter:
    from cryptobro.model import generate, load_with_adapter

    model, tok = load_with_adapter(a.adapter)
    outs = generate(model, tok, [r["prompt"] for r in rows])
    gs = summarize("CryptoBro model", outs)
    for o, g, r in list(zip(outs, gs, rows))[: a.show]:
        print(f"\n--- {r['date']} ---\n{o}\n\nTeacher:\n{feedback(g)}")
