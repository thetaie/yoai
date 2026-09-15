# Voice in, one pass

A recorded clip goes in whole and comes back as text carrying both what was
said and how it sounded. Not transcription with a mood label bolted on
afterward — one model, one pass, tone included.

**The gate experiment has been run, and for a 3B model the answer is no.**
Qwen2.5-Omni-3B describes a speaker's vocal state from what they *said*, not
from how they sounded. The evidence is below, along with the method, which is
cheap to repeat and is the part worth keeping.

Numbers here are labeled: **measured** on this project's hardware, **measured
elsewhere** and reproduced with the method that produced them, or **derived**
from documented architecture.

## What this replaces

The obvious build is a pipeline: voice-activity detection, then ASR, then a
speech-emotion classifier, then the language model over the transcript. Every
stage is small, swappable, and streams. It is the right shape for most
products.

It is the wrong shape here because **the transcript is the lossy step**. "I
slept fine" typed and "I slept fine" said flatly at 7am are the same string,
and that difference is the whole reason to accept voice input. Recovering it
separately means a speech-emotion model, and those cap out near 70% on real
speech — non-acted, cross-speaker, cross-language.

An omni model keeps the audio in the context. That makes it *positioned* to
hear tone. It does not make it able to.

## The result

Two controls, run in opposite directions, on
`Qwen2.5-Omni-3B-f16` + `mmproj-f16` through `llama-mtmd-cli` on an RTX 3090.
f16 throughout, so quantization is not an available excuse.

**Control 1 — hold the sound fixed, change the words.** Two clips from the same
Windows TTS voice: same synthesizer, same speaker, same flat synthetic prosody.
Only the words differ. Prompt asked for pace, pitch and tiredness, and
explicitly forbade transcribing.

| Words | Model's description of the *voice* |
|---|---|
| "I did not sleep well last night…" | "slow pace, low pitch, and **a sense of exhaustion**" |
| "I feel great today. I slept nine hours…" | "steady and even… they are **not tired** but rather calm and collected" |

The acoustic signal was effectively identical. The verdict flipped with the
semantics.

**Control 2 — hold the words fixed, change the sound.** One real 14-second
recording of Chinese speech, re-rendered at three tape speeds by patching the
sample rate in the WAV header. The audio samples are byte-identical; only
playback rate changes, which shifts tempo and pitch together.

| | Sound | Model's description |
|---|---|---|
| slow | 19.3 s, pitch well down | "moderate pace… pitch **neither high nor low**… calmness and composure" |
| original | 14.0 s | "moderate pace, **low pitch**, calm steady energy… tranquility" |
| fast | 10.3 s, pitch well up | "steady, measured pace, **low pitch**, calm collected energy… tranquility" |

A 1.9× tempo ratio and a large pitch shift left no trace. The model called the
fastest, highest-pitched rendering "measured pace, low pitch" — the same words
it used for the original.

Together: **the vocal-state output is driven by semantic content and is
invariant to acoustics.**

**This is not a wiring failure.** Transcription of that same clip came back
accurate and fluent — 「人活一辈子，生老病死…三十年河东三十年河西呢？」 — so the
audio reaches the model and the encoder is working. The acoustic detail
transcription needs survives. The acoustic detail tone needs either does not
survive the encoder or is not attended to downstream.

Limits worth stating: one speaker, one clip, one sample per condition at
temperature 0.2. A tape-speed change is not real emotional prosody. But it is a
*much larger* acoustic change than the gap between a tired voice and an alert
one — a model blind to this is not going to catch that.

### Why the two-way control is the part to keep

Either control alone proves nothing. Change only the words and a model that
genuinely hears would still shift its answer, because tired people do say tired
things. Change only the sound and a model that is merely hedging toward
"neutral" looks the same as one that is listening.

Run both and the failure mode has nowhere to hide. Neither needs an actor:
Windows TTS supplies the fixed-voice pair, a WAV header edit supplies the
fixed-words triple. The whole thing is repeatable in an afternoon against any
new model.

**Never evaluate one of these models without holding one of the two channels
fixed.** A clip where tone and content agree — which is most real speech —
cannot distinguish hearing from guessing.

## What llama.cpp does with an Omni model

Less than the upstream model card claims.

| | llama.cpp | Transformers · vLLM · DashScope |
|---|---|---|
| Text, audio, image input | yes | yes |
| Video input | no | yes |
| **Speech output (Talker)** | **no** | **yes** |

Qwen2.5-Omni is a Thinker–Talker architecture: the Thinker understands and
writes, the Talker speaks. **llama.cpp implements only the Thinker**; audio
output in `mtmd` is an open planning issue. So *not being able to speak* is a
property of this engine, not of the model — Qwen's own demo Space calls
DashScope with `modalities: ["text","audio"]` and gets native Talker speech
back in one request.

Read the `ggml-org` model card, never the upstream one. The upstream card
describes a model that can speak, and through this engine it cannot.

Long clips are handled: `mtmd` splits audio into chunks of at most 30 seconds,
**cut at pauses**, encoding each independently into the same context. Nothing
is truncated. The cost is context — roughly **25 audio tokens per second**
(derived: Whisper's 50 Hz frames, pooled by two), so a 60-second clip is about
1500 tokens of prefill.

If speech output is ever wanted, two separate lanes exist: `llama-tts`
(OuteTTS, and Qwen3-TTS since August 2026), or the platform's own TTS at no
memory cost. Neither is end-to-end — a TTS reading a transcript never sees the
hidden states that made the Talker worth having.

## The landscape is two points, far apart

| | Weights | Where |
|---|---|---|
| **Qwen2.5-Omni-3B** | 2.1–6.8 GB + mmproj | a phone |
| **Qwen3-Omni-30B-A3B** | 18.6–61.1 GB + mmproj | a server |

Nothing in between. The 3B is the only small Omni model that officially
exists. Both have `ggml-org` GGUF conversions with matching mmproj files, which
is the first thing to check in any repo — **without an mmproj the audio tower is
missing and the model silently degrades to a text-only LLM.**

### Two naming traps, both hit while researching this

**There is no official Qwen3.5-Omni.** Several sites describe one, with a
release date and a technical report. Searching Hugging Face returns 21
repositories named `Qwen3.5-Omni` and **not one belongs to the Qwen
organization**. The Qwen3.5 small models (0.8B/2B/4B/9B) are natively
multimodal for *vision*. Treat model facts from blog aggregators as unverified
until an official repository is open in front of you.

**`MARTHA-2B-Qwen3.5-Omni` is not a Qwen model.** Its neighbors in that search
are `OmniCoder-Claude-4.6-Opus-Uncensored`, `OmniClaw`, `Omnimerge` — "Omni" is
a naming fashion there, not a modality claim. The arithmetic settles it: its
f16 mmproj is 668 MB, about 334M parameters, while Qwen2.5-Omni's audio encoder
alone is roughly 640M. That mmproj cannot contain an audio tower.

**Generalize it: an mmproj below ~1.3 GB at f16 is not carrying a
Whisper-class audio encoder, whatever the repository is called.**

## On the device, the constraint is time, not memory

Measured elsewhere, on the real target: a Kirin 9020 at 16 GB running
Qwen3.5-2B `Q4_K_M` (1.28 GB) through the HarmonyOS llama.cpp lane gets
**~43 tok/s prefill and 11.2 tok/s decode**, against a read bandwidth of
**19.5 GB/s that a single core already saturates**. Method and full results:
[`docs/on-device-llm.md`](../../shanda/mirobody-v2/docs/on-device-llm.md) in
mirobody-v2.

Scaling to Qwen2.5-Omni-3B (`Q4_K_M` thinker, 2.10 GB) — derived, not measured:

| | |
|---|---|
| prefill, scaled by parameter count | ~26 tok/s |
| 60 s of audio ≈ 1500 tokens | **~58 s** |
| decode, at the observed fraction of the bandwidth ceiling | ~7 tok/s |

Plus the audio encoder's own forward pass. **Processing a clip takes about as
long as the clip.** It scales linearly, so short recordings do not rescue it: a
15-second clip still costs about 14 seconds of waiting.

Memory was never the problem. Thinker `Q4_K_M` plus mmproj `Q8_0` is 3.64 GB
and the target device has 16 GB. It is prefill throughput that makes this
unshippable, and that is a much harder wall — decode is bandwidth-bound and no
other compute unit on that SoC moves it. On the same device the Vulkan GPU
measured **3.5× slower** than CPU and the NPU's ceiling sat below today's CPU.

One related observation, **unverified**: audio encoding through `llama-mtmd-cli`
took about 3.1 s per chunk on an RTX 3090. That is high enough to suspect the
mmproj was not offloaded to the GPU. Worth resolving before any of the
above numbers are used to design around.

### The port exists; multimodal is not in it

mirobody-v2's `harmony/build-llama.sh` is a working HarmonyOS llama.cpp build,
with `armv8.6-a+i8mm+bf16+dotprod+fp16` read off a real device via `AT_HWCAP`
rather than guessed — a cross build with no `-march` silently lands on baseline
`armv8-a`, and i8mm is worth 2.9× prefill and 2.2× decode.

But it sets **`-DLLAMA_BUILD_TOOLS=OFF`**, and `libmtmd` lives under
llama.cpp's `tools/mtmd/`. There is no multimodal path in that build and no
`mtmd` reference anywhere in the shared C++ core. Audio input would have to be
enabled and wired first.

Note also that **Kirin 9020 reports `bf16` in `AT_HWCAP`**, so the usual warning
about bf16 falling back to FP32 on ARM does not apply to this device. f16 is
still the better pick for an encoder — it carries more mantissa bits than bf16
(10 vs 7) — but for portability reasons, not speed on this chip.

## MNN, if an engine change is ever on the table

Alibaba's MNN has first-class Omni support that llama.cpp lacks:
`-DMNN_BUILD_LLM_OMNI=ON`, export via `llmexport.py --omni`, and a complete
Talker configuration — `talker_speaker` (the same Chelsie/Ethan voices as the
official demo), `talker_max_new_tokens` (50 speech tokens per second of audio),
`dit_steps` for the codec decoder. Qwen3-TTS too. This is real end-to-end
on-device speech.

It is a fully separate lane: **MNN uses its own model format, so GGUF, mmproj
quantization and the tuned llama.cpp ARM flags all stop applying.** The Omni
and Talker code lives in MNN's C++ LLM library rather than its Android app, so
it is portable in principle — and the platform-specific backends
(`MNN_OPENCL`, `MNN_QNN`, `MNN_NEUROPILOT`) are all unwanted here anyway, since
GPU already measured slower than CPU on Kirin. But MNN's documentation does not
mention HarmonyOS at all.

**None of this addresses the result above.** MNN would let the app *speak*; it
does nothing about whether a 3B can *hear* tone. Revisit it if speech output
becomes a feature, not as a fix for this one.

## What is still open

**Does Qwen3-Omni-30B-A3B-Captioner pass the same two controls?** It is a
dedicated audio-captioning model, described upstream as low-hallucination
across speech, ambient sound and music — aimed at exactly the failure
documented here. `Q4_K_M` plus a bf16 mmproj is 20.81 GB, which fits a 24 GB
3090 with `-c 8192`; if it will not fit, `--no-mmproj-offload` frees the full
2.21 GB at no precision cost, only latency.

The three clips already exist. The outcomes:

- **It also cannot separate them** → the direction is dead at every scale. Go
  to the fallback.
- **It separates them cleanly** → this is a capacity problem, not an
  impossibility. On-device is still out, but voice could be a server feature
  with the local lane staying text-only.

## The fallback

SenseVoice-Small through sherpa-onnx: one ~230 MB int8 model doing ASR,
language ID, emotion and audio-event detection together, exposing `Lang`,
`Emotion` and `Event` as separate fields. sherpa-onnx supports HarmonyOS
upstream. The cost is languages — Chinese, Cantonese, English, Japanese,
Korean, not Whisper's 99.

Below that, prosody features computed directly: F0 contour, energy envelope,
speaking rate, pause ratio. No model, no memory, no latency. Enough for arousal
and for question-versus-statement, which may be most of what is actually
wanted.

`emotion2vec` was considered and rejected: no official ONNX export, a stale
standalone repository, and a fairseq dependency to carry, in exchange for a
~70% signal.

**Whatever is chosen, run the two-way control against it first.** The method in
this document is the deliverable; the rest is one model's result.

## What this must not become

**Never show a person a flat claim about their own emotional state.** The
result above is precisely why. The model did not say it could not tell — it
produced a confident, specific, plausible reading of a voice it was not
listening to, and that reading agreed with what the person had just said. In an
app about health, that failure is invisible: it looks right every time.

Use the signal only where being wrong is survivable — aggregated into trends
across many days, or as an input to how the model phrases its own replies.
Never as a displayed fact.
