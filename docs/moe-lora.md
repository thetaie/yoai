# Domain knowledge in a MoE chat model

**Decided: tried on desktop and server only, as LoRA on the few experts per
layer that a domain's tokens are routed to.** Not on the phone: a 30B-A3B model
is about 18 GB at `Q4_K_M` (Qwen3-Omni-30B-A3B's is 17.3 GB on disk). Not
measured yet; nothing here is built.

## The method

Expert-specialized fine-tuning (ESFT, DeepSeek, 2024), with LoRA in place of
training the experts whole:

1. Run a sample of the domain corpus through the model and count, per layer,
   how often each expert is among the top 8.
2. Per layer, keep the 3–5 experts that take the largest share.
3. Train LoRA on those experts' gate, up and down projections. The router,
   attention and every other expert stay frozen.

ESFT reported results on par with full-parameter fine-tuning on domain tasks —
math, code, law and translation among them — and less loss of general ability.
It trained the selected experts whole, on DeepSeek-V2-Lite, which has shared
experts. Qwen3's MoE has none, so whether a domain's routing concentrates on a
few experts there is the first thing to measure.

**Per layer, not per model.** Qwen3-30B-A3B has 48 layers of 128 experts and 8
active per token, so 3–5 a layer is 144–240 experts.

## What it does not settle

Fine-tuning on facts the model did not know is slow to take, and the more of
them it learns the more it invents (Gekhman et al., 2024). ESFT measured
domain tasks, not new facts. So the evaluation is against retrieval — the base
model given passages from the vector index — and counts invented answers, not
only right ones.

## Accepted for now: an adapter sized for every expert

llama.cpp at the pinned commit applies LoRA to expert matrices
(`build_lora_mm_id` in `src/llama-graph.cpp`), and `convert_lora_to_gguf.py`
stacks per-expert LoRA into the 3-D expert tensors. Both expect a LoRA for all
128 experts of a layer: the expert axis is dense, and the unselected experts
are zeros. In training, target every expert and freeze the A and B of the
unselected ones; B starts at zero, so they stay zero and the converter finds a
full set.

**Derived**, from Qwen3-30B-A3B's published shape — hidden 2048, expert FFN
768: rank r on gate, up and down is 2816·r values per expert per matrix, 52M·r
in all. At r = 8 that is 0.42B values, 0.83 GB at f16, about 96% zeros with 5
experts a layer. The extra matmul runs for every routed token, zeros or not.

**If domain knowledge goes in reliably this way, llama.cpp gets a patch** that
stores only the selected experts behind an index map — 4% of the size at 5
experts a layer. Not before.

## Steps

1. **Routing concentration, no training.** Read `ffn_moe_topk` for each layer
   through llama.cpp's eval callback, over a domain corpus and a general one.
   Per layer: the share of the domain's top-8 slots that its top 3–5 experts
   take, and how busy those experts are on the general corpus. An expert busy
   on everything is not the domain's.
2. **Train** outside this repo, selected experts only; convert with
   `convert_lora_to_gguf.py`.
3. **Evaluate against retrieval:** held-out domain questions, paraphrases of
   trained facts, questions about domain facts absent from the training data
   (invented answers), and general ability before and after.
4. **Then the llama.cpp patch.**

## Open

- Which MoE checkpoint. Qwen3-Omni-30B-A3B's thinker is on disk with the same
  router shape, 8 of 128 over 48 layers — enough to build step 1 on.
- Which domain, and its corpus.
- Where the adapter is loaded. `LocalClient` loads none yet; llama.cpp's
  server takes one with `--lora`.
