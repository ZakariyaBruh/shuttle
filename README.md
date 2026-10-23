# CryptoBro

A small LLM (Qwen2.5-1.5B-Instruct + LoRA) that plans **one Bitcoin trade inside a single day**:

```
BUY HERE: 06:00 AM
SELL HERE: 09:00 PM
PROFIT PREDICTION: 0.09%
```
and a **teacher** that grades it afterwards:
```
BUY: you said 06:00 AM, best was 05:00 AM - 1h 00m late.
SELL: you said 09:01 PM, best was 11:30 PM - 2h 29m early.
PROFIT: you predicted 0.09%, the best possible was 3.93% (off by 3.84 points).
Your trade really made 2.65% after fees - 2.56 points above your own prediction.
```

## How it works
- **Input (no look-ahead).** For trading day D the prompt only contains day D-1: closed candle, when its low/high happened,
  volume, 7/30/90-day trend, Fear & Greed index, and up to 6 headlines from that day. Training and live use share `build_prompt`.
- **Data.** 15-minute BTCUSDT candles (Binance public mirror), alternative.me Fear & Greed, CoinDesk/Decrypt/Cointelegraph
  headlines (historical dataset 2019 -> Feb 2025 for training; the same outlets' RSS feeds live).
- **Labels.** For each day, the hindsight-best buy-open / later sell-close on the 15-minute grid, with 0.1% fee per side.
  Day = UTC by default (`TZ` in `cryptobro/data.py`, or pass `python build_dataset.py America/New_York`; zones east of UTC are refused to avoid news leakage).
- **Splits (by time).** train 2019-11 -> 2024-03, val 2024-Q2, test 2024-07 -> 2025-01 (216 days).

## Run (free Colab/Kaggle T4)
```bash
pip install -r requirements.txt
python build_dataset.py                      # data/{train,val,test}.jsonl
python train.py                              # -> outputs/cryptobro-lora   (add --push-to USER/CryptoBro to upload to the Hub)
python evaluate.py outputs/cryptobro-lora    # teacher grades the test days + reference lines
python predict.py  outputs/cryptobro-lora    # today's plan from yesterday's data + live RSS news
python predict.py  outputs/cryptobro-lora --date 2024-09-10   # replay a past day, teacher grades it
```
`python evaluate.py --baselines` needs no GPU. `app.py` is a Gradio app for a free CPU Hugging Face Space
(set the `ADAPTER` variable to your Hub repo). Hugging Face's free tier has no GPU for training, so train on Colab/Kaggle and push the adapter.

## Reference lines on the test window (no model)
| Strategy | Avg/day | Win days |
|---|---|---|
| Fixed buy 06:00 / sell 21:00 | -0.05% | 51% |
| Fixed buy 00:00 / sell 23:45 | +0.07% | 48% |
| Hindsight best (unreachable cap) | +2.57% | 96% |

## Be realistic
Timing the intraday low and high of Bitcoin from yesterday's summary is very hard; expect the buy/sell times to be hours off
and the profit prediction to be rough. Only trust `evaluate.py` numbers on unseen days, after fees. Not financial advice.
