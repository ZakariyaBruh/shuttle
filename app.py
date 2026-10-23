"""Gradio app for a Hugging Face Space (free CPU): pick a day -> BUY HERE / SELL HERE / PROFIT PREDICTION (+ teacher for past days).
Set the Space variable ADAPTER to your Hub repo (e.g. Zakariya142623262/CryptoBro)."""
import os
from datetime import datetime, timedelta, timezone

import gradio as gr
import pandas as pd

from cryptobro.data import TZ, best_trade, build_prompt, fetch_fng, fetch_news_live, load_market, load_news_history, pick_headlines
from cryptobro.model import generate, load_with_adapter
from cryptobro.teacher import feedback, grade

model, tok = load_with_adapter(os.environ.get("ADAPTER", "Zakariya142623262/CryptoBro"))


def plan(date):
    today = pd.Timestamp(datetime.now(timezone.utc).date())
    day = pd.Timestamp(date) if date else today
    live = day >= today
    intr, daily = load_market(TZ)
    news = fetch_news_live(day - timedelta(days=1)) if live else load_news_history().pipe(lambda n: n[n.date == day - timedelta(days=1)])
    prompt = build_prompt(day, daily, fetch_fng(), pick_headlines(news), intr)
    if prompt is None:
        return "No complete history for that day.", ""
    out = generate(model, tok, [prompt])[0]
    if live:
        return out, "Trade day hasn't finished, so there is nothing to grade yet."
    d = intr.loc[day : day + pd.Timedelta(hours=23, minutes=59)]
    t = best_trade(d)
    row = dict(date=str(day.date()), opens=d.open.tolist(), closes=d.close.tolist(), best=dict(buy=str(t["buy"]), sell=str(t["sell"]), profit=t["profit"]))
    return out, feedback(grade(out, row))


gr.Interface(plan, gr.Textbox(label="Date (YYYY-MM-DD, blank = today)"), [gr.Textbox(label="CryptoBro"), gr.Textbox(label="Teacher")], title="CryptoBro").launch()
