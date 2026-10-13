# Docs

What was decided and why — the record of an argument, including what was tried
and rejected, so it is not argued twice. The interface's own design is in
[design/](../design/).

| File | What |
|---|---|
| [embedding.md](embedding.md) | Which embedding model, at what quantization, and how the vectors are stored |
| [voice.md](voice.md) | Recognition outside the model, what is read from the voice and by what, and why not one omni pass |
| [slides.md](slides.md) | Speech in and out as a Marp deck: the pipeline, echo cancellation, TTS shapes and runtimes, Qwen3-TTS, RTF, emotion |
| [moe-lora.md](moe-lora.md) | Domain knowledge in a MoE chat model: LoRA on the experts a domain routes to, desktop and server only |
| [plan.md](plan.md) | What is still to do, by module as on the first slide, each row linked to its reasoning |
