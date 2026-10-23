# CryptoBro

A small LLM (Qwen2.5-1.5B-Instruct + LoRA) fine-tuned to answer one question each morning:
**"Given yesterday, should I hold Bitcoin today (BUY) or be in cash (SELL)?"**

## How it works
- **Input (no look-ahead).** For trading day D the prompt only contains day D-1: closed candle, volume, 7/30/90-day trend,
  Fear & Greed index, and up to 6 headlines published that UTC day. Training and live use share `cryptobro/data.py::build_prompt`.
- **Data.** Prices: Binance public mirror (`data-api.binance.vision`). Sentiment: alternative.me. Headlines: historical
  CoinDesk/Decrypt/Cointelegraph feed dataset (2019 -> Feb 2025) for training; the same outlets' public RSS feeds live.
- **Labels.** Hindsight-optimal long/cash path (Viterbi, 0.1% fee per flip) computed per split = "buy and sell for maximum profit".
- **Splits (by time).** train 2019-11 -> 2024-03, val 2024-Q2, test 2024-07 -> 2025-01. No shuffling across time.
- **Model.** 1.5B params, 4-bit QLoRA: trains in ~20 min and runs inference on a free Colab/Kaggle T4 (or CPU, slowly).

## Run (Colab, free T4 GPU)
```bash
git clone <this repo> && cd shuttle && pip install -r requirements.txt
python build_dataset.py                     # writes data/{train,val,test}.jsonl
python train.py                             # -> outputs/cryptobro-lora
python backtest.py outputs/cryptobro-lora   # test-period profit vs buy & hold (fees included)
python predict.py  outputs/cryptobro-lora   # today's call, from yesterday's data + live RSS news
python predict.py  outputs/cryptobro-lora --date 2024-09-10   # replay a random past day
```
`python backtest.py --baselines` (no GPU needed) prints buy-and-hold, a momentum rule and the oracle ceiling.

## Be realistic
Bitcoin's next day is mostly noise. The oracle (+796% on the test window) is an unreachable ceiling, not a target.
A model that does *not* beat buy & hold (+60% over the test window) after fees is not worth trading, so **run
`backtest.py` and trust that number, not the training loss.** If it fails, expect to iterate (more data, different
features, longer horizon) rather than get a money machine. This is not financial advice; only risk money you can lose.
Live RSS feeds only expose roughly the latest day or so of items, so run `predict.py` early on the morning of the trade.
