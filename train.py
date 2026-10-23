"""QLoRA fine-tune of a small chat model on data/train.jsonl. Run on a free GPU (Colab/Kaggle T4)."""
import argparse

import torch
from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training
from transformers import DataCollatorForSeq2Seq, Trainer, TrainingArguments

from cryptobro.model import BASE, chat, load_base, load_tokenizer, read_jsonl

ap = argparse.ArgumentParser()
ap.add_argument("--base", default=BASE)
ap.add_argument("--out", default="outputs/cryptobro-lora")
ap.add_argument("--epochs", type=float, default=2)
ap.add_argument("--lr", type=float, default=1e-4)
ap.add_argument("--push-to", help="Hub repo id to upload the adapter to, e.g. user/CryptoBro (needs HF_TOKEN). Off by default.")
args = ap.parse_args()

tok = load_tokenizer(args.base)
model = prepare_model_for_kbit_training(load_base(args.base))
model = get_peft_model(model, LoraConfig(r=16, lora_alpha=32, lora_dropout=0.05, task_type="CAUSAL_LM", target_modules="all-linear"))
model.print_trainable_parameters()


def encode(row):
    prompt, answer = chat(tok, row["prompt"], row["answer"])
    p = tok(prompt, add_special_tokens=False).input_ids
    a = tok(answer, add_special_tokens=False).input_ids
    return dict(input_ids=p + a, attention_mask=[1] * (len(p) + len(a)), labels=[-100] * len(p) + a)  # loss on the answer only


train = [encode(r) for r in read_jsonl("data/train.jsonl")]
val = [encode(r) for r in read_jsonl("data/val.jsonl")]

Trainer(
    model=model,
    args=TrainingArguments(
        output_dir=args.out, num_train_epochs=args.epochs, learning_rate=args.lr, lr_scheduler_type="cosine", warmup_ratio=0.05,
        per_device_train_batch_size=4, gradient_accumulation_steps=4, per_device_eval_batch_size=4,
        fp16=torch.cuda.is_available(), logging_steps=10, eval_strategy="epoch", save_strategy="no", report_to="none",
        gradient_checkpointing=True,
    ),
    train_dataset=train, eval_dataset=val,
    data_collator=DataCollatorForSeq2Seq(tok, padding=True, label_pad_token_id=-100),
).train()
model.save_pretrained(args.out)
tok.save_pretrained(args.out)
print("saved adapter to", args.out)
if args.push_to:
    model.push_to_hub(args.push_to)
    tok.push_to_hub(args.push_to)
    print("pushed to", args.push_to)
