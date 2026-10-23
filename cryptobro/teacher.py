"""The 'teacher': grades a prediction against what the market actually did that day."""
import pandas as pd

from .data import STEP, fmt_time, parse_answer


def _minutes(t):
    return t.hour * 60 + t.minute


def _grid(row):
    n = len(row["opens"])
    return pd.date_range(row["date"], periods=n, freq=f"{STEP}min")


def grade(answer_text, row):
    """Returns dict of errors + realised profit, or None if the model's answer didn't parse / isn't a valid trade."""
    pred = parse_answer(answer_text)
    if pred is None:
        return None
    idx = _grid(row)
    best = {k: pd.Timestamp(v) if k != "profit" else v for k, v in row["best"].items()}
    buy_err = _minutes(pred["buy"]) - _minutes(best["buy"])
    sell_err = _minutes(pred["sell"]) - _minutes(best["sell"])
    realised = None
    if pred["sell"] > pred["buy"]:
        bi = (_minutes(pred["buy"]) // STEP)
        sj = (_minutes(pred["sell"]) // STEP) - 1
        if 0 <= bi <= sj < len(idx):
            realised = row["closes"][sj] / row["opens"][bi] * 0.998001 - 1
    return dict(pred=pred, best=best, buy_err=buy_err, sell_err=sell_err, realised=realised,
                profit_err=pred["profit"] - best["profit"], miss=(realised - pred["profit"]) if realised is not None else None)


def _mins(m):
    h, mm = divmod(abs(m), 60)
    return f"{h}h {mm:02d}m" if h else f"{mm}m"


def feedback(g):
    """Human-readable teacher comments."""
    if g is None:
        return "I couldn't read your answer - stick to the exact format."
    b, s = g["buy_err"], g["sell_err"]
    say = lambda e: "spot on" if e == 0 else f"{_mins(e)} {'late' if e > 0 else 'early'}"
    lines = [
        f"BUY: you said {fmt_time(g['pred']['buy'])}, best was {fmt_time(g['best']['buy'])} - {say(b)}.",
        f"SELL: you said {fmt_time(g['pred']['sell'])}, best was {fmt_time(g['best']['sell'])} - {say(s)}.",
        f"PROFIT: you predicted {g['pred']['profit']:.2%}, the best possible was {g['best']['profit']:.2%} "
        f"(off by {abs(g['profit_err']) * 100:.2f} points).",
    ]
    if g["realised"] is None:
        lines.append("Your trade was invalid (sell must come after buy, inside the day) so it earned nothing.")
    else:
        lines.append(f"Your trade really made {g['realised']:.2%} after fees - {abs(g['miss']) * 100:.2f} points "
                     f"{'below' if g['miss'] < 0 else 'above'} your own prediction.")
    return "\n".join(lines)
