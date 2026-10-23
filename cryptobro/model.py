import torch
from transformers import AutoModelForCausalLM, AutoTokenizer, BitsAndBytesConfig

from .data import SYSTEM, read_jsonl  # noqa: F401

BASE = "Qwen/Qwen2.5-1.5B-Instruct"  # small enough for a free Colab/Kaggle T4, strong enough to follow the format


def chat(tok, prompt, answer=None):
    msgs = [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}]
    text = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True)
    return text if answer is None else (text, answer + tok.eos_token)


def load_tokenizer(base=BASE):
    tok = AutoTokenizer.from_pretrained(base)
    tok.padding_side = "right"
    return tok


def load_base(base=BASE, quantize=True):
    kw = {}
    if torch.cuda.is_available():
        kw["device_map"] = {"": 0}
        if quantize:
            kw["quantization_config"] = BitsAndBytesConfig(
                load_in_4bit=True, bnb_4bit_quant_type="nf4", bnb_4bit_compute_dtype=torch.float16
            )
        else:
            kw["torch_dtype"] = torch.float16
    return AutoModelForCausalLM.from_pretrained(base, **kw)


def load_with_adapter(adapter_dir, base=BASE):
    from peft import PeftModel

    model = PeftModel.from_pretrained(load_base(base), adapter_dir)
    return model.eval(), load_tokenizer(base)


@torch.no_grad()
def generate(model, tok, prompts, batch=8, max_new_tokens=48):
    """Greedy answers in the BUY HERE / SELL HERE / PROFIT PREDICTION format."""
    tok.padding_side = "left"
    out = []
    for i in range(0, len(prompts), batch):
        enc = tok([chat(tok, p) for p in prompts[i : i + batch]], return_tensors="pt", padding=True).to(model.device)
        gen = model.generate(**enc, max_new_tokens=max_new_tokens, do_sample=False, pad_token_id=tok.pad_token_id)
        out += tok.batch_decode(gen[:, enc.input_ids.shape[1] :], skip_special_tokens=True)
    tok.padding_side = "right"
    return out
