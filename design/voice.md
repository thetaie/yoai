# Voice

How speech gets into the app and back out, what is read from it, and why none
of it is one model doing everything.

Numbers here are labeled: **measured** on this project's hardware, **measured
elsewhere** and reproduced with the method that produced them, or **derived**
from documented architecture.

## What was measured

Two machines. Everything below was run by this project, on these two, through
this app's own lanes or the engines' own CLIs — nothing is quoted from a model
card or a benchmark someone else ran.

**Which build produced a number is part of the number**, and the engines here
are other people's checkouts that move under us. [third/manifest.txt](../third/manifest.txt)
pins the commit of each one and [third/patches/](../third/patches/) carries
what we change in them; `third/sync.cmd check` says whether the trees on a
machine still match. Figures taken before a pin moved are labeled where the
move mattered — see the two ggml symbol tables further down, which are the
first case where it did.

- **The phone**: HUAWEI PLA-AL10, Kirin 9020, Maleoon GPU, 16 GB. Big-little,
  and the core count is not settled here: this file has long said one big plus
  three mid plus four little, while the runtime reports **twelve** logical CPUs.
  Nothing below turns on which is right, but the thread sweeps do assume four
  fast cores.
- **The PC**: Intel Core Ultra 9 185H, 16C/22T, 32 GB, with an integrated
  **Intel Arc Pro Graphics**. A laptop part — it throttles, which shows up
  below.

**Real time factor (RTF) is seconds of computation per second of audio.** Above
1 the engine cannot speak as fast as it talks; below 1 it outruns playback. For
recognition the same ratio runs the other way and small is fast.

### Five engines, and which half of the audio each one does

The shape of this app follows from one table. **Only one engine does both ends,
and it is the one that was retired**; everything shipping is a stage in a chain
because no single model does both halves at a price this phone will pay.

| engine | audio in | text | audio out | size |
| --- | :---: | :---: | :---: | --- |
| Qwen2.5-Omni-3B, on MNN | ✓ | ✓ | ✓ | 3.8 GB |
| SenseVoice-Small `q8`, on ggml | ✓ | | | 254 MB |
| llama.cpp + a GGUF | | ✓ | | 1.28 GB |
| Qwen3-TTS, on qwentts.cpp | | | ✓ | 1.4 GB |
| the platform's own TTS | | | ✓ | — |

Measured on the phone, by what each one does:

**Audio in.**

| | measured | status |
| --- | --- | --- |
| SenseVoice-Small `q8`, 4 threads | **RTF 0.173** | **shipping** |
| Qwen2.5-Omni's audio encoder, in MNN | RTF ~0.31 | retired with the rest of omni |

**Text in the middle.** No audio, and listed because the chain's wait is mostly
here and because it is what feeds the speaking half.

| | measured | status |
| --- | --- | --- |
| Qwen3.5-2B `Q4_K_M` | **14.0 tok/s** decode, 5.9 s to load | **shipping** |
| Qwen3.5-4B `Q4_K_M` | 6.70 tok/s, 14.5 s to load | too slow for a reply |

**Audio out.** RTF above 1 means it cannot speak as fast as it talks.

| | measured | status |
| --- | --- | --- |
| the platform's own TTS | **1.37 s** to the first word, warm | **shipping** — one broadcast voice, no alternative |
| Qwen3-TTS 1.7B VoiceDesign `Q4_K_M` | **RTF 2.14 fast, 3.52 sustained** | prepared playback only |
| Qwen3-TTS 0.6B CustomVoice `Q4_K_M` | RTF 1.94 / 3.30 | audibly worse voice |
| Qwen3-TTS base, cloning a 22.4 s reference | RTF 6.6–11.6; **first audio 30–44 s** | a fixed 24–40 s of setup per utterance |
| omni's Talker, `dit_steps` 5 | **RTF 3.70** (2.8 at its coarsest, with artefacts) | retired to `src/voice/attic/` |

**Both ends at once**, which only omni does.

| | measured | status |
| --- | --- | --- |
| Qwen2.5-Omni-3B: 4.8 s of speech in, a spoken answer out | **46.2 s** — thinker 17.2, Talker 37.5 | retired |
| the shipping chain: recognise → think → speak | **7.57 s cold, 1.37 s warm** to the first word | **shipping** |

**And two numbers that belong to the device rather than to any engine.**

| | measured | |
| --- | --- | --- |
| a read loop, no model | **24.4 GB/s** at 8 threads | the ceiling all of the above share |
| ggml's Vulkan backend on the Maleoon 920 | initialises, is selected, then the OS kills the process (`GPU_LEAK_KILL`) before any audio | the GPU is unavailable here |

**omni's 3.70 and Qwen3-TTS's numbers do not compare, and a shared column hides
it.** omni's covers its Talker alone, excluding a thinker that is 17.2 s of the
46.2, while every Qwen3-TTS figure is a whole synthesis. And omni's was taken
before the device's two clocks were understood — its Talker ran 37.5 s
continuously, so it belongs beside Qwen3-TTS's *sustained* 3.52, not its fast
2.14. On that footing omni was the faster of the two. It lost on size, on a
second inference framework, on having to think before it can speak, and on
having no way to ask for a voice.

### On the PC

**This is the deployment matrix**: one passage, one seed, two passes per cell.
Milliseconds per frame is the comparable column — a frame is 80 ms of audio, and
the checkpoints generate different lengths from the same seed, so their RTFs are
not directly against each other.

| model | quant | backend | ms/frame | RTF | |
| --- | --- | --- | --- | --- | --- |
| **1.7B VoiceDesign** | **`Q8_0`** | **Arc iGPU, Vulkan, MMVQ off** | **71.0** | **0.90** | **the one to deploy** — below 1, and the richest prosody |
| 1.7B VoiceDesign | `Q4_K_M` | Arc iGPU, Vulkan, MMVQ off | 46.1 | 0.63 | 35% cheaper, audibly flatter |
| 1.7B VoiceDesign | `Q8_0` | Arc iGPU, Vulkan | 78.3 | 0.80–1.00 | slight nasality |
| 1.7B VoiceDesign | `Q4_K_M` | Arc iGPU, Vulkan | 44.8 | 0.57–0.58 | first audio 196 ms; mechanical pauses |
| 1.7B VoiceDesign | `Q4_K_M` | CPU, 11 threads | 78.8 | 0.95–1.07 | |
| 1.7B VoiceDesign | `Q8_0` | CPU, 11 threads | 98.3 | 1.23–1.29 | |
| 0.6B CustomVoice | `Q4_K_M` | Arc iGPU, Vulkan, MMVQ off | 36.2 | 0.44–0.46 | cheapest by far, and **not deployable** — see below |
| 0.6B CustomVoice | `Q4_K_M` | CPU, 11 threads | 67.9 | 0.86 | |
| 0.6B CustomVoice | `Q8_0` | either | — | — | not downloaded |

Where the GPU wins, and where it does not, per frame. **One codec chunk
throughout** (`--codec-chunk-dur 300`): the default splits at 24 s, so a run of
23.9 s pays for one pass and one of 27.9 s pays for two, and an earlier version
of this table compared across that boundary without noticing:

| stage | CPU `Q4` | GPU `Q4` | | CPU `Q8` | GPU `Q8` | |
| --- | --- | --- | --- | --- | --- | --- |
| talker decode | 23.9 | 17.3 | 1.38× | 33.8 | 26.5 | 1.27× |
| code predictor | 20.0 | 20.5 | **0.98×** | 29.3 | 29.6 | **0.99×** |
| codec decode | 34.9 | **6.3** | **5.5×** | 35.2 | **6.4** | **5.5×** |

**The codec costs the same 6.3–6.4 ms a frame in every GPU configuration**, at
both quantizations and at both model sizes — which is what it should do, being
the same file each time, and which the chunking confound had been hiding.

#### One kernel switch, and it is not a trade

**`GGML_VK_DISABLE_MMVQ=1`.** MMVQ is ggml's quantized mat-vec path — a
performance optimization — and on this Intel driver turning it off made the
engine both faster and better:

| | MMVQ on | MMVQ off | |
| --- | --- | --- | --- |
| `Q8_0`, ms/frame | 78.3 | **71.0** | 9% faster |
| `Q4_K_M`, ms/frame | 44.8 | 46.1 | 2.9% slower |

By ear, across three seeds: with MMVQ on, `Q4` produced takes with flat,
mechanical pauses and one that ran 57% long; with it off the pace varies and the
pauses do not sound stamped. `Q8` on it has a slight nasality that goes away
with it off. **A kernel that is slower and lossier is not a trade**, and it is
set in `apply_vulkan_env()` rather than offered as an option.

Two neighbours were tried and rejected. `DISABLE_INTEGER_DOT_PRODUCT` costs
another 3.8% and, stacked on MMVQ, was indistinguishable by ear.
`DISABLE_F16` costs 17% and introduced audible artefacts — the opposite of what
disabling a lower precision should do, unexplained, and enough to leave alone.

**This is also where a proxy metric failed.** Length was being used to judge
which numeric path was behaving: the baseline ran 471 frames where the CPU ran
299, and `DISABLE_INTEGER_DOT_PRODUCT` brought it to 308 — the closest match,
and by that reading the winner. `DISABLE_MMVQ` came out at 349, furthest from
the CPU of the three, and is the one that sounds right. **Agreeing with a
reference on a number is not the same as being correct**, and there was no
reason to think it would be.

#### And `Q8_0` is the one to deploy

With MMVQ off on both, judged on the same seed: `Q4` is audibly flatter — less
variation in pace, less variation in the pauses — where `Q8`'s only remark was
that its pitch sat high, which the written description and the seed both reach.

| | ms/frame | RTF | |
| --- | --- | --- | --- |
| **`Q8_0`, MMVQ off** | **71.0** | **0.90** | richer prosody |
| `Q4_K_M`, MMVQ off | 46.1 | 0.63 | 35% cheaper, flatter |

**Below 1 either way, so the 35% cannot be spent on latency.** It is a
throughput cost — streams per machine — against an audible difference in the one
thing this engine was chosen for, which makes it worth paying. That is the
opposite of the phone's answer, and correctly so: nothing there is below 1, so
speed still buys something.

**The margin is thin, though.** 0.90 is ten percent of headroom on an integrated
GPU, and RTF moves with utterance length because prefill amortises — the same
configuration read 0.80 to 1.00 across seeds. Per frame it is 71.0 ms against a
frame worth 80, and that is the number to plan capacity against, not the RTF of
any one take.

Three more things follow for a server, and two of them were surprises:

- **Quantization is not free on the GPU.** `Q4` is 29% cheaper than `Q8` there,
  almost the same margin as on the CPU. The hope was that a compute-bound
  backend would absorb the extra bytes and let `Q8`'s quality ride along; it
  does not.
- **The code predictor gets nothing from the GPU** — 1.00× at `Q4` and slightly
  worse at `Q8` — and it barely shrank when the talker went from 1.7B to 0.6B on
  the phone either. It is an MTP head sized by the sixteen codebooks rather than
  by the LM, so it moves with neither. At `Q4` on the GPU it is **44% of the
  bill**, and **the floor has changed hands**: on the phone the codec was
  immovable at 45% of the cost, and on a GPU the codec collapses to 18% while
  this takes its place.
- **The small checkpoint is the fragile one, and that is now the second time.**
  Its runaway on Vulkan — 2048 frames, the cap, twice — was MMVQ, and turning
  that off fixes it. What it does not fix is the delivery: at one seed the 0.6B
  on the GPU sounds distressed, close to tears, where the SAME seed on the CPU
  is ordinary and another seed on the GPU is fine. The 1.7B takes the same
  change in numerics without anything like that. It is the same shape as the
  polyphone failure, where `Q4` cost the 0.6B its grapheme-to-phoneme and left
  the 1.7B's intact: **a small checkpoint has less redundancy to spend, and a
  numeric perturbation — quantization or a different backend — takes something
  real.** So the 0.6B is off the table for serving despite being 42% cheaper a
  frame, and for a firmer reason than speed.

Older figures, kept for the shape and not the values:

| engine and model | backend | measured | |
| --- | --- | --- | --- |
| Qwen3-TTS Base, free-running | llama.cpp `libmtmd`, CPU | RTF 1.41 | changes speaker between texts |
| Qwen3-TTS Base, cloned in context | qwentts.cpp, CPU | RTF 1.97 | stable voice, good prosody |
| Qwen3-TTS VoiceDesign | qwentts.cpp, CPU | RTF 1.16 | **does not reproduce** |
| recognition, text | — | **not measured on this machine** | |
| a server CPU, a discrete GPU | — | **not measured at all** | |

**Those three are from an earlier session and read about 21% fast.** The same
host, passage and checkpoint that recorded 1.16 has since measured 1.40 three
times over, with all three stages moved together — a clock difference, not a
configuration one. **Every CPU figure on this machine carries about ±15%**: the
1.7B `Q4` cell above read 1.131 and 1.137 on one day and 0.948 and 1.067 on
another. The GPU cells repeat inside 1%. A server would not wander like this,
but nothing here can say by how much.

**Below 1 is a threshold rather than a slope.** A stream needs a pre-buffer of
`duration × (RTF − 1)` before playback can start without running dry: 132 s for
a minute of audio on the phone, 8 s on the PC's CPU, and **nothing at all on its
GPU**. That is the line between prepared playback and speaking as you go, and
the PC is the first machine here on the far side of it.

### What none of this covers

Nothing was measured on server hardware, on a discrete GPU, on Android, or on
any phone but this one. The thermal behaviour below is specific to this handset,
and the GPU result is specific to HarmonyOS's memory policy — the same backend
on the PC loaded 1.37 GB, which is the model, and nothing objected.

## The decision

**Recognition and synthesis sit outside the language model, and what is read
from the voice is read by signal processing, not by a model.**

- **In:** SenseVoice-Small, bundled, `q8` — ported into this core in `src/voice/`,
  linking the ggml this repo already ships and not libllama. Transcription,
  language ID and audio events for Chinese, Cantonese, English, Japanese and
  Korean.
- **Out:** the platform's own text-to-speech.
- **The model in the middle:** anything. It is a text model now.
- **What the voice says about the speaker:** computed from the audio, per
  utterance, against that same recording — no model, no history, no baseline.

The third line is the largest consequence and the easiest to skim past. With an
omni model in the loop, voice would work only on the local lane — you cannot
post audio to an arbitrary OpenAI-compatible endpoint. With recognition outside,
**voice works with every provider the user has configured**. And the C++ core's
ABI already takes `messages_json`; transcription produces text.

## Every Qwen3-TTS checkpoint, on this phone

The front table gives one row to this whole family. This is that row opened up,
and the sections near the end of the file are where each number comes from —
worth reading before quoting any of these, because several replaced an earlier
number that was wrong.

All at four threads with a pinned seed and `qwen-tokenizer-12hz` as the codec.
Real time factor is seconds of computation per second of audio; **above 1 it
cannot speak as fast as it talks.**

**The short utterances are not all the same text**, which bounds how finely this
column can be read. They are a few seconds each, and a pinned seed does not give
two checkpoints the same length anyway — the same seed through Q8 and Q4 of one
model produced 41 frames and 48. Differences of a tenth here are real; a
hundredth is not. The per-stage breakdowns later in this file are where the
like-for-like comparisons live.

| checkpoint | size | the voice is fixed by | RTF, short | RTF, fast / sustained | `调暗` | by ear |
| --- | --- | --- | --- | --- | --- | --- |
| **1.7B VoiceDesign Q4_K_M** | **1.4 GB** | **a written description** | **2.29–2.35** | **2.14 / 3.52** | correct | **the best here** |
| 1.7B VoiceDesign Q8_0 | 2.2 GB | a written description | 2.60–2.92 | — | correct | same voice |
| 0.6B CustomVoice Q4_K_M | 0.8 GB | one of nine named speakers, plus an optional written style | 1.88–2.04 | 1.94 / 3.30 | correct | audibly worse |
| 0.6B base Q4_K_M | 0.8 GB | a 22.4 s reference recording and its transcript | 6.60, 7.97 | — | **wrong** | timbre fine |
| 0.6B base Q8_0 | 1.2 GB | same | 8.32 | — | correct | — |
| 1.7B base Q4_K_M | 1.4 GB | same | 11.53 | — | correct | better than 0.6B |
| 1.7B base Q8_0 | 2.2 GB | same | 11.64 | — | correct | — |

**The four cloning rows are not four times slower at generating.** Their
per-frame cost is close to the others; what they carry is a fixed setup —
encoding the reference through the codec and prefilling it through the talker —
of about 24 s at 0.6B and 40 s at 1.7B, paid per utterance whatever its length.
First audio lands at 29.8 s and 43.8 s. That is why Q4 barely helps there:
1.7B goes 11.64 → 11.53, because the talker is not where the bill is.

**"Short" and "fast / sustained" measure different things.** The short column is
four to eight seconds of audio, which never leaves the device's fast clock — for
the cloning rows it is barely more than the setup. The pair is the rate before
and after the clock steps down, which happens about 30 seconds into continuous
generation from cold and 17 seconds when the device is already warm. A real
passage spends most of itself in the second number: 30 seconds of audio took 88
to 97 s.

### The engine this replaced, on the same phone

Qwen2.5-Omni-3B on MNN was the first attempt and is a different shape: one model
that listens, thinks and speaks, where the rows above only speak. Its code is in
`src/voice/attic/`.

| `dit_steps` | DiT forwards per chunk | RTF, Talker only | by ear |
| --- | --- | --- | --- |
| 5, the model's own config | 4 | 3.70 | — |
| 3 | 2 | 3.0 | acceptable |
| 2 | 1 | 2.8 | audible artefacts |

3.8 GB, 7.9 s to load, and the voice is the model's own with nothing to steer
it. A whole exchange — 4.8 s of recorded audio in, a spoken answer out — took
46.2 s, of which 17.2 was the thinker and 37.5 the Talker.

**Two things make the ratios above look closer to Qwen3-TTS than they are, and
one makes them look further.**

- **They exclude the thinker.** 3.70 covers `generateWavform()` alone. Qwen3-TTS
  numbers are the whole synthesis, and its text comes from a lane that was
  already paid for.
- **They were taken before the device's two clocks were understood.** That
  Talker ran 37.5 s continuously, so most of it was past the step down. The
  honest comparison is against Qwen3-TTS's **sustained** 3.52, not its fast
  2.14 — and on that footing 2.8 against 3.52 says omni's coarsest setting was
  the faster of the two.
- **Against that, the coarsest setting is the one with audible artefacts**, and
  the diffusion sampler is the only lever it has; spending it spends exactly
  what the engine was carried for.

So **omni did not lose on speed, and this file used to imply it did.** It lost
on 3.8 GB against 1.4, on a second inference framework against the ggml already
linked, on needing to think before it can speak — and most of all on having no
way to ask for a voice. A model that sounds however it sounds is not a voice you
can design an app around.

### What that adds up to

**1.7B VoiceDesign at Q4_K_M, for prepared playback.** It is the only row that
is both the best-sounding and affordable, and the three ways of fixing a voice
are not equal:

- **A written description** holds across passages, costs nothing, and is the
  only one that lets pace, energy, microphone distance and breath be asked for
  in words.
- **A named speaker** is a stronger guarantee of identity — it is an enumeration
  rather than a description holding steady — and can still take a written style
  on top. It exists only at sizes and checkpoints that sound worse.
- **A reference recording** is the only one that can copy a specific voice, and
  it costs half a minute before the first sample. The lever there is the
  reference's length. It has since been pulled — on CosyVoice3, not on this
  checkpoint — and 17.3 s down to 6 s took **74% off the fixed charge and 22%
  off the per-second rate** with no audible change. Still untried here; see
  "CosyVoice3 was built and measured" for the numbers and for why they do not
  transfer on their own.

The polyphone row is the one to carry forward as a rule rather than a fact:
grapheme-to-phoneme is **per checkpoint, and quantization can take it away**.
Only `0.6B base Q4_K_M` reads `调暗` as `diào`; the same checkpoint at Q8 and the
same quantization at 1.7B both read it correctly, so neither variable explains
it alone. It does not carry across a model swap or a requantization, and there
is no way to correct it from the API — nothing in the runtime takes pinyin or
phonemes.

## On a PC: the integrated GPU crosses 1

The front table gives the number. This is where it comes from, and what a PC
client would have to know to get it.

Measured with ggml's Vulkan backend on the same laptop, same 1.7B VoiceDesign
Q4, same passage. The device is an **Intel Arc Pro Graphics** — integrated,
sharing the CPU's memory, `uma: 1`, `fp16: 1`, and **`matrix cores: none`**.

| stage | CPU, 11 threads | Arc iGPU | |
| --- | --- | --- | --- |
| Talker decode | 23.9 ms/frame | 17.6 | 1.36× |
| Code predictor | 20.0 | 20.0 | **1.00×** |
| **Codec decode** | **34.9** | **8.2** | **4.3×** |
| Prefill | 653 ms | 159 | 4.1× |
| per frame | 78.8 ms | **45.8** | |
| **RTF** | 0.95–1.07 | **0.57–0.58** | |
| first audio | ~683 ms | **196 ms** | |

**The codec is the stage that moves, and it was the immovable one.** Five
attempts on the phone left it at 73.5 ms a frame — streaming, buffering, two
quantizations, a third-size talker, and the GPU that never ran. It is 45% of the
bill there and the reason the ceiling looked structural. On a GPU it is 4.3×
faster and drops to 18% of a much smaller bill. A convolutional decoder with 21
conv contexts was the part expected to fall back to the CPU for lack of op
coverage; it is the part that gained most.

**And the stage that does not move takes its place.** The code predictor is
1.00× — the GPU buys it nothing, exactly as a third-size talker bought it
nothing on the phone. It is an MTP head sized by the sixteen codebooks rather
than by the LM, so neither dimension reaches it, and at 20.0 ms of a 45.8 ms
frame it is **44% of the GPU's bill**. Whatever the next lever on this lane is,
that is what it has to move.

Five things a PC client would need to know:

- **Shader compilation is a one-off per process**, and it is not small: the
  first run measured 0.863 against 0.60 warm, with prefill at 3652 ms instead of
  165 and prompt build at 894 instead of 8.5. A resident client pays it once at
  startup. A client that spawns a process per utterance pays it every time and
  never sees 0.60.
- **The GPU's timings are far steadier than the CPU's.** Across three seeds:
  0.558, 0.583, 0.612 on the GPU against 1.018, 1.112, 1.300 on the CPU. The
  spread on the CPU side is the laptop's own clock, the same throttle the phone
  has in a milder form — a GPU run is short enough not to provoke it.
- **Different numerics are different takes.** The same seed and text gave 471
  frames on the GPU and 299 on the CPU. Not a defect: at seeds 7 and 42 the GPU
  produced 287 and 305 against the CPU's 337 and 316, so neither backend speaks
  systematically slower. But a seed chosen for how it sounds on one backend is
  not the same utterance on the other, and a client that pins seeds has to pin
  them per backend.
- **HarmonyOS's `GPU_LEAK_KILL` does not appear here.** The desktop backend
  loaded about 1.37 GB of tensors, which is the model, and nothing objected.
  That was a platform policy, not something ggml does everywhere, and it should
  not be carried forward as a reason to avoid the GPU on a PC.
- **The 0.6B CustomVoice checkpoint does not run correctly on this backend.**
  Two passes, both identical: it never emits EOS, runs to the 2048-frame cap and
  produces 163.8 s of audio for a 22-second passage, where the same seed on the
  CPU stops at 349 frames. Unexplained, and enough to keep the small checkpoint
  off a GPU server until it is.

**This is a floor for PC hardware, not a ceiling.** It is an integrated GPU with
no matrix cores, sharing bandwidth and thermal budget with the CPU that is
feeding it. Anything discrete would start from here.

## Where each axis gets its numbers

The rest of this file is the argument. This is the answer:

| axis | what it reads | from | status |
|---|---|---|---|
| **valence** | what is said | **text** | **built** — chat model, constrained reply |
| **arousal** | how it is said: pitch, continuity, loudness | **audio** | **validated** — the table further down |
| **dominance** | who is the subject of the verbs | **text** | **built**, unproven — same call as valence |
| **intensity** | distance from neutral, in any direction | **audio**, second pass | **built** — norm of the z-vector |
| **unpredictability** | how much the arousal series jumps | **audio**, second pass | **built** — mean \|Δ arousal\| |
| **approach–avoidance** | going-toward against putting-off wording | **text**, checked against the log | not built — weakest signal here |
| **potency** | — | — | not a separate axis; it is dominance |

"Second pass" means a function of numbers the arousal pass already produced —
no extra feature, no extra column in the database.

The two text axes carry a cost this table cannot show: **they are only as good
as the transcript, and the transcript has been measured getting a sentence's
polarity backwards.** See "Its transcription is the weak link" below before
treating either as settled.

### The text pair costs a sampler setting, and getting it wrong is silent

Valence and dominance come from one turn against whichever chat model is
configured, with a GBNF constraining the reply to
`{"valence":<level>,"dominance":<level>}` over five enumerated levels. The
grammar is not decoration: it makes every reply in range and parseable by
construction, and five levels is already finer than a small model resolves.

**A constrained turn must also be decoded greedily**, and this was learned the
expensive way. Asked to score one plainly cheerful sentence about lunch, a 2B at
the conversational defaults this project ships for chat — temperature 0.7,
top-k 20, top-p 0.8 — answered:

    -1  -1  -1   0  -1   0

six seeds, never twice the same, never right. Greedy, it answered 0 every time;
a 4B answered +0.5. **The sampling was the entire difference**, and on screen it
looked exactly like a model that could not read Chinese.

The repetition penalty has to go too, for a reason specific to scoring: it
demotes tokens seen in the last `repeat_last_n`, and a scoring prompt
necessarily shows the model its own scale — "-1 very unpleasant, 0 neutral" — so
the penalty lands on the handful of tokens the grammar allows as the answer.

Both are now automatic: a non-empty grammar makes the turn deterministic in
`llm/chat/local.cpp`, documented at `yoai_chat`. The generalisation is worth
keeping — **a reply that is a value, not prose, must not be sampled like
prose.**

Model size matters here as well, though far less than for arousal: the 2B says
0 for that sentence even greedily, and the 4B says +0.5. Valence from text is a
task small models are good at, which is exactly why the axes are split by
channel — but 2B is below the floor.

All of it is computed **per utterance**, and the split that produces those
utterances decides what every number means. That has its own section below.

## Three axes, not a list of emotions

Emotion in speech is better described by a few independent axes than by a list
of labels. This is the PAD model — pleasure, arousal, dominance — and it is not
academic decoration. It is what stops three different questions from being
collapsed into one.

- **Valence** — pleasant to unpleasant. Is this good or bad?
- **Arousal** — activated to deactivated. How much energy is behind it?
- **Dominance** — in control or controlled. Is this person acting, or being
  acted on?

They are independent, and the states that agree on two axes but differ on the
third are exactly the ones a smaller model cannot tell apart:

|  | valence | arousal | dominance |
|---|---|---|---|
| anger | negative | **high** | **high** |
| fear | negative | **high** | **low** |
| sadness | negative | low | low |
| fatigue | **negative** | low | low |
| calm | **positive** | low | high |

Rows three to five are why valence and arousal are both needed: *depleted* and
*rested* are both "low energy" and mean opposite things, and a one-dimensional
tiredness scale scores them the same.

**What the third axis adds** is the first two rows, which are identical without
it. Anger and fear on their own would not pay for it — neither is a common daily
state. What pays for it is the everyday form of the same split: **"busy and on
top of it" against "busy and drowning."** Same high arousal, similar valence,
and opposite meaning for someone looking back over their own week. Likewise "I
decided to stop" against "I gave up," which V and A score alike.

**The three axes come from different channels**, and this is the part that
decides what has to be built:

- **Valence is semantic** — "today was awful" states it outright.
- **Arousal is acoustic** — activation shows in rate, pitch range and continuity
  before it shows in word choice.
- **Dominance is grammatical** — not which words are chosen but who is the
  subject of the verbs. "It got away from me," "I had to," "I couldn't," 「只能」
  「没办法」 put the speaker on the receiving end; "I stopped at four," "I decided
  to" put them at the wheel. The words can be identical in sentiment while the
  agency is reversed.

There is also an asymmetry worth knowing: **valence is easy to mask in words,
arousal is hard to mask in voice.** Someone saying "I'm fine" controls the
valence; the flat slow delivery leaks the arousal. Dominance sits with valence
here — it is said, so it can be dressed up.

**Measured**, on four ten-second segments of one real recording — the same model
asked the same question, once with audio and once with the transcript only:

| segment | words are about | from text | from audio |
|---|---|---|---|
| 1 | a birthday, thanking his daughter | happy | **slightly tired** |
| 2 | "glad and sad at once" | sad | slightly tired |
| 3 | "I lost control of my emotions" | sad | **sadness** |
| 4 | "let us live happily, stay safe" | happy | **calm** |

The two columns are not disagreeing. They are answering different questions.
Segment 1 is cheerful words in a tired voice, and only having both channels
shows that.

### Dominance is the weakest of the three, and that changes how it is used

Three things about it, none of them measured on this project's material:

**Annotators agree on it least.** Across the emotion-corpus literature, valence
and arousal ratings are reliable enough to train on and dominance is
consistently behind them; several well-known corpora publish V and A only. So
the axis is real but noisy, and it should never be the thing an answer turns on.

**Its best acoustic cue is already spent.** The cue most often reported for
dominance is pitch lowered against the speaker's own baseline — which is the
feature carrying arousal here. Inside one recording a drop in F0 cannot be
evidence for both at once, and arousal has the better claim on it — that is the
reading with a table behind it. So the main read on dominance comes from the
transcript, where it competes with nothing.

**Two cheap acoustic features are still open**, because arousal does not use
them: the **terminal contour** over the last fifth of an utterance — falling
reads as assertion, rising as asking for agreement — and the **disfluency rate**,
filled pauses and restarts, which is already listed below under fluency. Both
fall out of what is computed anyway.

The birthday monologue has a plain case of why the axis is worth having:
segment 3 is "I lost control of my emotions." Valence and arousal score that the
same as anger. It is the opposite of anger, and the difference is entirely
dominance — stated in the words, invisible in the sound.

**Nothing here has its own version of the arousal table yet.** That result rests
on measurements; this rests on reading. Compute it, store what it is computed
from, and keep it out of anything shown to a person until it has been checked
the same way.

## Segmentation is the unit, not a preprocessing step

Every number in this file is per utterance. Cutting the recording is not
something that happens before the analysis — it is the analysis's coordinate
system, and the same features give opposite answers under different cuts.

**That is measured, and it is the strongest single result here.** The prosody
features were first asked about ten-second fixed windows, found nothing, and the
conclusion written down at the time was that the path was closed. Re-cut at
pause boundaries, the same features on the same recording put the arousal
minimum exactly on the sentence where the speaker turns to his mother. Nothing
about the features changed. It is the third of the three withdrawn conclusions
below, and the only one where the fix was a change of unit.

Why a fixed window loses it: emotion in speech moves on the scale of a sentence,
and a ten-second window spanning the cheerful opening and the turn that follows
averages the two into nothing. Averaging is all a window does.

Windows are also not comparable to one another, which matters more than it
sounds. The whole method is a z-score against the recording's own distribution,
and a distribution needs units that mean the same thing. One window half full of
silence and one window of continuous speech are not two samples of one
population.

And an utterance is something a person can find again. "You slowed down thirty
percent after the two-minute mark" is a claim about a stretch nobody remembers.
"You slowed down on the sentence about your mother" can be checked. The rule at
the end of this file — never a label, always a deviation the speaker can verify
— needs the unit to be something the speaker recognizes.

### How the cut is made now

**Measured**, the working version, no model: frame the audio at 40 ms with a
20 ms hop, take RMS per frame, and treat anything under **8% of that recording's
own peak** as silence. A **pause is 0.30 s or more** of it. Runs of speech
shorter than **0.8 s** are dropped. On the 37-second monologue that gives eleven
utterances.

Each number is doing a job:

- **8% of the clip's own peak, not a fixed dBFS.** Level varies with microphone
  distance and gain, and a fixed threshold drifts between takes without saying
  so.
- **0.30 s.** Under about 0.15 s a gap is a stop closure inside a word, not a
  boundary between phrases. Too short and phrases fragment; too long and two
  sentences become one unit and average each other out, which is the fixed
  window problem again in miniature.
- **0.8 s.** Below that there are too few voiced frames for an F0 distribution
  to mean anything. A one-word segment has a pitch, not a range.

### Three different things want to cut the audio, and only one of them is this

- **The VAD** (`fsmn-vad`) is there for **recognition** — keeping silence out of
  the encoder and chunking long audio to fit. It may put a boundary anywhere
  convenient.
- **Prosodic segmentation** is there for **analysis**, and its boundaries become
  the units compared against one another and possibly described to a person.
- **The transcript supplies boundaries too, and they are now free.** Greedy CTC
  knows which encoder frame emitted each token, and an encoder frame is 60 ms,
  so every token carries a timestamp out of the same forward pass at no extra
  cost. `src/voice/asr/sensevoice.hpp` returns them. This was an open question until
  the port was written; it is answered, and it means punctuation is available as
  a boundary. Punctuation beats pauses — people pause where sentences do not
  end, and end sentences without pausing — and a pause confirmed by punctuation
  beats either alone.

They can share an energy envelope. **They must not share parameters.** Reusing
the VAD's chunks because they are already computed would mean adopting
boundaries chosen for a different objective, which is the failure this file
keeps recording in other forms.

### Where it stops working

- **A recording too short to have a distribution.** Under about five utterances
  the z-scores are noise, and at the rate measured here that is roughly fifteen
  to twenty seconds of speech. "Going to bed" is two seconds and one utterance.
  The right output there is **nothing**, not a weak number.
- **Speech with no pauses** — fast, continuous, or read aloud from a page —
  gives one long segment and nothing to compare it against.
- **A noisy room** lifts the floor until nothing falls under the threshold and
  no boundary is found at all.

All three are detectable before any analysis runs, which is what makes them
tolerable: the pipeline can decline instead of guessing.

#### The second one is not an edge case — measured on the device

A 16-second note, recorded on the phone, ordinary conversational Mandarin, cut
into **two** utterances: a 0.9 s fragment and then one run of **1.3 s to
15.8 s**. Its transcript has five commas and a full stop in it, so there were
six clause boundaries in that audio and the energy floor found none of them.

The giant segment reports `range 28.7 st, p10 76 Hz, p90 400 Hz` — both ends
pinned to the F0 search bounds. That is not a wide pitch range, it is the
absence of a measurement: fifteen seconds of speech averaged into one "unit"
whose pitch distribution spans everything the estimator can represent.

Two corrections follow.

**"Fast, continuous, or read aloud" understates it.** This was none of those. It
was someone describing their lunch. Fluent speech simply does not stop for
0.30 s between clauses as often as the threshold assumes, and Mandarin — where
the comma does most of the work an English full stop does — gives it even fewer
chances.

**The five-utterance floor is therefore not mainly about duration.** Earlier
text here estimated "roughly fifteen to twenty seconds of speech" to reach five
utterances, and a separate 14-second clip did reach seven. The variable is not
length, it is **how the speaker phrases**. Two recordings of the same duration
can yield two segments or seven, so no recording-length rule can predict
whether the analysis will run.

That makes the transcript boundaries in the section above stop being an
optimization. **They are what makes the floor reachable at the lengths people
actually record** — notes here run 5 to 15 seconds, not 37. The cost is real
and is recorded with the design of the new cut: `speech_ratio` measures pause
structure *inside* a unit, so a unit cut at a clause boundary has nothing left
to measure and continuity — 0.35 of the arousal weight — degenerates. A cut
that keeps continuity meaningful has to either confirm punctuation against a
pause, as the section above suggests, or compute continuity over a window
rather than over the unit.

### The cut has to be versioned

Change the pause threshold and every stored number changes, because every stored
number is per segment. Rows cut by two different segmenters are not comparable,
and the cross-day work described below — if it is ever built — would compare
them without noticing. The segmenter's parameters belong stored beside the
features, so a later version can re-cut from the audio where it still exists and
leave the old rows out where it does not.

## Within the recording, against itself

**This is the part that is validated, and it is deliberately the smallest
claim.** Score each utterance, cut as above, as a deviation from *that
recording's own* distribution of pitch, continuity and loudness.

Microphone distance, recording gain and the speaker's own pitch range are all
constant within a take, so they cancel. No personal history is needed. It works
on the first recording anyone ever makes.

**Measured** on a 37.4-second monologue, eleven utterances:

| # | at | arousal | what he is saying |
|---|---|---|---|
| 0 | 0.1 s | **+1.40** highest | "Today is my sixty-third birthday." |
| 5 | 15.9 s | **−0.85** lowest | "What hurts is, I thought of my mother — the mother who bore me and raised me." |
| 6 | 21.7 s | −0.34, lowest F0 of the whole take | "They say a birthday is the day your mother suffered." |
| 9 | 32.0 s | +0.80 | "From now on let us live happily." |

**The arousal minimum lands exactly on the sentence where he turns to his
mother**, and the maximum on the opening announcement. That is not a statistical
wobble; it is an acoustic turn at a content turn, found without reading a word
of the content.

Pitch carries most of it. Loudness is the least trustworthy feature and is
weighted down: across a recording it tracks microphone distance as much as
effort, and on a coarser cut of this same clip it ranked the segments in the
*opposite* order to everything else.

### It is in the core now

`src/voice/prosody/prosody.cpp`, needing neither a model nor ggml — which is the whole
point of taking arousal from the signal. `yoai-prosody-test` prints the
table above from a WAV.

**Measured**: against the tool that produced the original result, on the same
16 kHz samples, it agrees on **every column of all eleven rows** — the same
utterance boundaries, F0 to a tenth of a hertz, all three deviations and the
arousal to two decimals, the same baseline of 123.5 Hz and sd 11.1. The columns
that matter are scale-invariant, so the two agree exactly or they do not.

It runs in **0.07 s**, against 5.4 s for the Python: RTF 0.0019 against 0.14 on
the 37.4-second monologue, and 77× what the prototype did. The earlier note
here that 5.4 s was "already 7× real time" undersold the headroom by two orders
of magnitude.

Two things are deliberately *not* in it. **Voice quality** — jitter, shimmer,
HNR — is not ported, because it does not work yet (see below). And the
autocorrelation normalizes by the whole frame's energy rather than by the
overlapping part, which leans slightly toward reporting a pitch an octave high;
that is a known flaw, kept on purpose, because it is what the validated result
was produced with and changing it would leave the numbers with nothing to be
checked against.

### The cross-day version is much harder, and is not being built yet

Normalizing within a recording means every recording averages to zero by
construction. It can say "this sentence was the low point of this note"; it
cannot say "today was lower than last week".

That second question needs a baseline accumulated over days, and the obvious
baselines do not survive contact with reality:

- **A single personal average is meaningless.** A voice on waking — swollen
  folds, low, creaky — and a voice after an afternoon outdoors are not
  comparable, and lumping them produces a number that mostly reports which of
  those happened.
- **Conditioning on the declared state is worse than not conditioning.** It is
  tempting: `voice_features` hangs off `entry_id`, so the state comes free from
  a join. But the state log is **user-declared data with known decay** — this
  repo has `end_at` precisely because people stop working at 17:00 and notice at
  23:00, and the day view has gaps because people forget. Comparing an
  after-nap voice against a "working" baseline because the label was stale
  produces a confident, systematic error — the same shape as every other
  failure recorded here: a wrong answer that looks right.

If it is built later, **condition on time of day, not on state**. Time of day is
observed rather than declared, is always correct, and captures the circadian
part of voice variation, which is the largest confound. Require a minimum sample
count per bucket and say so in the interface — "three more notes before this
means anything" — rather than computing a confident number from four samples.

**The state log is weak evidence, and that applies beyond voice.** The design's
own insight lines — "on days when your nap ran past 25 minutes, your working
time averaged 47 minutes more" — rest on nap durations being accurate. They are
self-reported. Anything built on them should say so.

## The dimensions past the three

The literature carries more axes than three, and they are not all the same kind
of thing. One is an axis already here under an older name, one has no path from
a microphone, and two fall out of the numbers above for almost nothing. Sorting
them is worth doing once, because the failure mode is adding an axis because it
exists rather than because it changes an answer.

Where each of them would read from is in the table at the top of this file. The
question here is the other one — whether it is a separate axis at all:

| | a distinct axis? |
|---|---|
| **potency** | no — dominance in an older vocabulary |
| **unpredictability** | yes, though from appraisal theory rather than PAD |
| **intensity** | yes — magnitude rather than direction |
| **approach–avoidance** | yes — anger is unpleasant *and* approach |

**Potency is dominance renamed.** Osgood's semantic differential factored
judgments into evaluation, potency and activity; PAD is those three under other
names — pleasure for evaluation, arousal for activity, dominance for potency.
There is a shade between them, potency reading as the strength of the thing
being judged and dominance as the speaker's own felt control, but no feature set
or corpus separates them. It is written down here only so that someone arriving
from the other tradition does not add a fourth axis that is the third.

**Unpredictability is one pass away from being computed.** Its home is
appraisal theory rather than PAD — the study that argued explicitly that the
emotion space is not two-dimensional landed on four, adding unpredictability to
pleasantness, potency-control and activation. What makes it cheap here is
mechanical: the arousal profile produces a per-utterance z-score series, and
unpredictability is the **second-order** statistic of that series — how much it
jumps, not where it sits. A note that walks steadily and a note that lurches can
have the same mean arousal. It is one pass over numbers already in memory.

One trap, and it is the same one this document keeps recording: two different
things share the word. Whether the *day* was surprising is semantic; whether the
*delivery* lurches is acoustic. Measuring the second and reporting the first is
how this becomes the fourth withdrawn conclusion.

**Intensity is the magnitude, not the level.** Arousal asks which direction;
intensity asks how far from neutral in any direction. They come apart on exactly
the states this app exists for: grief is low arousal and high intensity, and so
is genuine exhaustion, which is why one "how activated" scale reads both as
nothing much happening. Mechanically it is the norm of the same z-vector that
arousal takes a weighted sum of — identical inputs, magnitude instead of sign.

Its limit is one already recorded above: within-recording normalization sets
every take's average to zero, so this can say which utterance in a note was the
most marked and never that the whole note was heavy. Global intensity needs the
cross-day baseline that is not being built. That constraint belongs to the
pipeline, not to the axis, and it applies the same way to all of them.

Its acoustic side also sits close to the **vocal effort** row below — how hard
the voice is working, independent of activation. If effort is implemented, check
the two against each other before treating them as separate signals.

**Approach–avoidance has the only outside check, and the weakest signal.** It is
genuinely not valence: anger is unpleasant and approach-motivated, which is the
observation that established the axis in the first place. But it is measured in
the literature with frontal EEG asymmetry and behavioral tasks, not with
microphones, and its acoustic correlates are weak and tangled with arousal and
dominance. From the transcript it is partly readable — "I'm going to," "let's"
against "I've been putting it off," "I didn't want to deal with it."

What keeps it in view is that it is the only dimension here that predicts
something outside the audio: avoidance predicts an **absence** in the log.
Someone who says three times in a week that they should go running, with no
exercise entry all week, is saying something the sound cannot. But see the
section above — the absence might be a forgotten tap. That is evidence across
many days, never on one.

A word this document now uses twice: **agency**. Above it means who is the
subject of the verbs, which is dominance. In the motivation literature it is
often paired with approach–avoidance instead. Where this file says agency, it
means the first.

**Neither of the two worth building adds a stored column.** Both are functions
of the per-utterance measurements in the next section, which is the same rule
the axes already follow.

**And the rule for adding any of them:** each axis multiplies the ways to be
confidently wrong, and this file already records three conclusions withdrawn for
being read past what they could support. An axis earns its place by changing an
answer the app would otherwise give — not by appearing in a paper. Like
dominance, the two being built stay out of anything shown to a person until they
have their own version of the arousal table.

## Store measurements, not conclusions

Per utterance: `f0_mean`, `f0_p10`, `f0_p90`, `f0_end_slope`, `rate_syl`,
`pause_ratio`, `pause_count`, `rms_rel`, and the voice-quality measures once they
are correct. Hung off `entry_id`, with the utterance's offsets into the
recording and the `seg_version` that cut it.

`f0_end_slope` is the terminal contour, and it is in the list for dominance
rather than arousal — one extra regression over frames already computed.
Disfluencies are not a separate measurement at all: they are counted off the
transcript against those same offsets.

The axes are computed from these at read time, not stored. The reason is the
one the timeline schema already uses for dates: **the derived thing will change
and the measured thing will not.** Today's weighting of pitch against continuity
is a guess; re-deriving it must not require keeping the audio, because keeping
the audio is a privacy cost and a storage cost that features are not.

`rate_syl` looked close to free — the transcript gives the character count, the
energy envelope gives the duration, so speaking rate is a division rather than an
estimate. **It is free only with inverse text normalization turned off.** With it
on, which is what the GGUF asks for, the same clip comes back as `约567，大卡`
instead of `约五百六十七大卡`: three characters where four syllables were spoken.
Rate is then wrong at every number, and wrong in one direction, so an utterance
that happens to contain a figure reads as slower than its neighbours for a reason
that has nothing to do with the speaker. Within-recording comparison is exactly
where that does damage.

`SenseVoice::set_inverse_text_normalization(false)` turns it off. The catch is
measured and awkward: **the same switch also removes all punctuation**, and the
segmentation section wants punctuation as a boundary source. So the two uses pull
opposite ways — analysis wants it off, boundaries and display want it on — and
the choice is either two passes at RTF 0.05 each or a syllable count that
expands digits back out per language. Not yet decided; what is decided is that
counting characters off an ITN transcript is not an option.

## What else the voice carries

Past the three emotional axes are the dimensions that are not emotion at all,
and for an app about the body they are the more interesting half:

| | what it is | cost |
|---|---|---|
| **vocal effort / strain** | how hard speaking is — independent of arousal; one can be low-energy and high-effort | needs spectral work |
| **voice quality** | breathy, creaky, tense — jitter, shimmer, HNR | needs proper glottal pulse marking |
| **fluency** | filled pauses, restarts, prolongations — cognitive load and disturbance both raise it; also the acoustic half of dominance | shares the features already computed |

These are **falsifiable in a way emotion is not**. "Your voice is hoarser than
usual" is something the speaker can check against their own throat. "You sound
sad" is something they can only accept or resent.

A caution on the literature: voice as a health biomarker is an active field with
mixed results. The near signals — respiratory illness, vocal fatigue — hold up
much better than the distant ones. Build the near ones; leave the screening
claims alone.

**Voice quality is implemented and wrong.** A first pass produced jitter around
6.5% and HNR around 4 dB, where healthy voices sit under 1% and above 20 dB.
The per-cycle peak picking is not stable enough on real recordings. It needs the
proper pulse-marking approach before any of those numbers are used.

## Choosing the recognition model

### SenseVoice-Small, and why an old model is still the answer

SenseVoice-Small has not been updated in about two years, and its transcription
has been overtaken: **Qwen3-ASR** (0.6B and 1.7B) covers thirty languages, holds
up better in difficult acoustics, and unifies streaming with offline inference.
On transcription alone it wins.

But nothing has replaced the combination — transcription, language ID, emotion
and audio events in one small model. The two quiet years mean the niche is
uncontested, not that the model was surpassed in it.

**Its emotion labels are not what this app needs, and that is a label-space
problem rather than a perception one.** The vocabulary is `HAPPY SAD ANGRY
NEUTRAL FEARFUL DISGUSTED SURPRISED` — categories, and all of them high-arousal
except `SAD`. There is no word in it for tired, depleted, calm or content. The
whole low-arousal band collapses into `NEUTRAL`.

So when it returned `NEUTRAL` for all four segments of the birthday monologue —
including the one where a listener clearly heard grief — that is what the model
is able to say, not necessarily what it heard. **Measured**: the `f32` reference
returns `NEUTRAL` on all four too, so it is not a quantization artifact.

### Its transcription is the weak link, and the text axes eat the errors

**Measured** on the 37.4 s monologue, same 16 kHz file through all three, asked
for a verbatim transcript:

| | SenseVoice `q8` | Qwen2.5-Omni-3B | Qwen3-Omni-30B |
|---|---|---|---|
| 又是易拉宝又是… | 横**福** | 横**幅** | 横**幅** |
| 我是…高兴又难受 | **极**高兴 | **既**高兴 | **既**高兴 |
| 生活还算… | **忧郁** | **优越** | **优越** |
| 我多想告诉… | 告诉**他** | 告诉**她** | 告诉**她** |

Both omni models get all four right. **The third row is not a typo, it is a sign
flip**: the speaker said his life is *comfortable* (优越) in a sentence listing
what he is happy about, and the transcript says *melancholy* (忧郁). The sentence
now argues the opposite of what was said — and that sentence is what the valence
axis reads.

So the table at the top of this file is too generous. Valence is marked *free,
the model reads the transcript anyway*. **The transcript is free; its errors are
not.** A wrong word here does not look like a gap, it looks like a sentiment, and
it is fed to an axis that has no way to doubt it. This is the same failure shape
recorded throughout this file, arriving through a component that had been treated
as settled.

Worth being precise about what this does and does not say:

- **It is one clip.** Four errors is a case, not a rate. Whether this is
  occasional and tolerable or frequent enough to need a check is unmeasured.
- **Size is not the explanation.** The 3B gets it right too, and it is a dozen
  times SenseVoice, not a hundred. "It lost to a much bigger model" is not
  available as an excuse.
- **It separates two jobs cleanly, and that supports the design.**
  Transcription is easy for both omni sizes — 3B and 30B agree here. What
  separates them is *hearing tone*: on the three tape speeds below, the 30B
  tracks pitch and pace while the 3B says "moderate pace, low pitch, calm" to
  all three. Transcription does not need a large model; hearing tone does; and
  the one that hears tone does not fit on a phone. Which is why tone comes from
  signal processing — the same conclusion, reached from the transcript side.

Its transcription is what we want it for. Arousal comes from the signal
processing above.

### The four tags, and what each one is actually worth

The model answers four queries alongside the transcript. They are not equally
useful, and treating them as one feature would be a mistake in both directions —
ignoring the one that matters and trusting the one that does not.

| tag | verdict |
|---|---|
| **language** | **Use it.** `nospeech` is a gate, and the language decides the reply language and the TTS voice |
| **itn** | **Use it**, and control it — see the note on `rate_syl` above |
| **emotion** | Store it. Never show it. Worth one thing: disagreement |
| **event** | A hint that may only lower confidence, never gate |

**`language` is the one being wasted today.** `nospeech` means the pipeline
should decline rather than hand an empty or invented transcript to the language
model — and this file has already recorded that a wrong transcription does not
look like silence, it looks like text. The rest of the label decides which voice
speaks the answer back, and, in an app with eleven interface languages, what
language the answer should be in when someone speaks one the interface is not
set to.

**`itn` is an input as much as an output.** *Inverse text normalization*: a
speech synthesizer needs the written form turned into the spoken one, `567` into
"five hundred and sixty-seven", and that is text normalization; recognition needs
the opposite, hence *inverse*. `withitn` and `woitn` are with it and without it.

The four query tokens are prepended to the features, and the fourth selects
which; the answer just reports what was asked. **Measured**: the GGUF ships
`[0 1 2 14]` — automatic language detection, and normalization on.

**`emotion` stays out of sight, and has exactly one job.** Its label space has no
word for tired, depleted or calm, so a `NEUTRAL` carries almost nothing. But a
*non*-neutral label is a positive claim about a high-arousal state, and that can
be compared against the arousal computed from the signal. Agreement means
nothing; **disagreement is a cheap, per-recording version of the offline check
this file elsewhere proposes running an omni model for**. Log the disagreement
rate; never put the label on a screen.

**`event` is the weakest.** `BGM` would genuinely matter — background music
corrupts the F0 tracking and the energy envelope that arousal rests on — but this
file already records a clip where the tag read `Speech` throughout and was wrong.
So it can lower confidence in a measurement and must never be the thing that
decides whether one is taken.

**The replacement path, if transcription turns out not to be good enough:** move
recognition to Qwen3-ASR, which llama.cpp supports natively
(`PROJECTOR_TYPE_QWEN3A`) with no second runtime. Pay for it when the need is
demonstrated.

### `q8`, and not `f16` — the opposite of the obvious guess

Three builds are published: `f32` at 936 MB (the reference), `f16` at 470 MB,
`q8` at 254 MB. **Measured** here against `f32` on five clips — two Windows TTS
lines, a nutrition explainer, a monologue and a synthetic narration:

| | result |
|---|---|
| `f32` | punctuation, correct tags |
| **`f16`** | **the text-normalization head flips**: it answers `woitn` where `f32` answers `withitn`; on two clips the emotion and event tags vanish as well |
| `q8` | reproduces `f32` on every clip |

The missing punctuation and capitalization first recorded here as a second
symptom are **not** a second symptom. `woitn` is what removes them: the same
switch that writes `五百六十七` instead of `567` also drops the punctuation, as
the note on `rate_syl` above measures directly. One head flips, and everything
that looked like a second fault follows from that one.

`q8` is both the smaller file and the accurate one — likely because `q8_0` is
block-quantized with per-block scales, which suits this model's numeric range
better than a flat 16-bit mantissa.

What `f16` breaks is narrow and worth naming precisely: the transcribed **words
stay correct**. One of the four query heads answers wrongly, reproducibly, in one
direction — and it is the head, not the question. The build is still fed query
token 14; it answers as though it had been given 15, so
`set_inverse_text_normalization(true)` cannot straighten it out. **Anyone
verifying "is the transcription correct" would pass this build.** Upstream marks
`f16` and `q8` both recommended and reports `q8` at CER 7.99 against `f16`'s
identical figure — measuring transcription only, which is exactly the
measurement that cannot see this.

**Not a user-facing setting.** "Which quantization would you like" is not a
question anyone can answer about themselves, and choosing wrong is invisible —
demonstrably so. Per-*language* downloads are a different matter: the user knows
what language they speak, and the design already calls for exactly that, with
sizes shown in settings.

### A known failure: invented letters on some audio

On one 38-second synthetic narration, the CTC output interleaved isolated
capital letters — `S` eleven times, `E` and `N` six each — assembling into
`SESE`, `SNB`, `NCT` at sentence boundaries. **Measured**: all three precisions
do it, including `f32`, so it is the model and not the quantization. Adding the
VAD does not help; the energy envelope shows continuous speech with no silence
to cut. The cause is not established — two plausible explanations were checked
and both were wrong.

The three genuine human recordings were clean. But it means **the transcript
cannot go straight into the language model** — a wrong transcription here does
not look like silence, it looks like text. A filter for runs of isolated capital
letters is a few lines and belongs on the C++ side, shared by both platforms.

Also: the event tag was `<|Speech|>` throughout that clip, so the event field is
not dependable on anything unusual.

## How recognition gets built

FunASR ships its own llama.cpp runtime for SenseVoice, and it is far smaller
than that name suggests. **Measured** by building it here:

- It is **not a fork of llama.cpp** and does not patch it. Each binary links
  `ggml` only — not `llama` — and the CMake takes
  `-DFETCHCONTENT_SOURCE_DIR_LLAMA=<path>` to build against a checkout already
  present. It built against this repo's llama.cpp **unmodified**.
- The whole implementation is **401 lines**: `funasr-sensevoice.cpp` (191),
  `funasr_vad.h` (161), `funasr_audio.h` (49). The 191 carry the mel filterbank
  with its own FFT, the SAN-M attention and layer graphs, positional encoding,
  CTC decode and tag parsing. ggml graph code loops, so seventy encoder blocks
  are written once.
- The only heavy dependency, `miniaudio.h`, belongs to `funasr_audio.h` and
  decodes files from disk. On a device the microphone hands over PCM already, so
  it does not travel. (It also cannot open non-ASCII paths on Windows, which
  costs a desktop test nothing and would have cost a user everything.)
- **MIT for the code, a separate agreement for the weights.** The runtime and
  the rest of that repository are MIT. The official SenseVoiceSmall weights are
  not: they carry FunASR's model open-source agreement v1.1, which does permit
  commercial use but attaches attribution and model-name requirements, and which
  covers the official weights only — a third-party GGUF conversion has to be
  checked on its own terms. Bundling the model in the app is therefore a
  licensing step as well as a build step: the attribution has to land somewhere
  the user can reach, and Settings is where the other model information already
  is.

There is no second ggml, no engine to adopt, and nothing to keep in sync
upstream — recognition becomes a few source files inside this core, linked
against the ggml already built with this project's tuned `-march` flags. SAN-M
is attention plus depthwise convolution, which is what `i8mm` and `dotprod` are
for.

A separate VAD model (`fsmn-vad`, also GGUF) chunks long recordings, same shape,
one source file against the same ggml — **for recognition only**. Its boundaries
are not the analysis boundaries; see the segmentation section.

### It is built, and it matches the reference

`src/voice/` now carries the port: `fbank.*` for the front end and `sensevoice.*`
for the encoder, behind `-DYOAI_ASR=ON`, linking the ggml archives and not
libllama. `build.cmd asr` (or `./build.sh asr`) assembles a desktop ggml SDK and
turns the lane on; `yoai-sensevoice-test` exercises it.

The front end is a separate file for a reason: it is arithmetic with one right
answer, it needs no model, and getting it subtly wrong is the worst failure
available here — the encoder does not degrade gracefully on features that are
merely close, it produces confident text that is wrong. So it is checked exactly
and without a model: the filter a 1 kHz tone must land in, the frame count for
one second of audio, and byte comparisons that pin the low-frame-rate padding
and hop.

**Measured**, against the reference binary on three clips resampled to 16 kHz
beforehand so that both read the same file: the transcripts are
**character-for-character identical**, including punctuation and all four tags.
An earlier run differed by one character, which turned out to be the test
harness's own linear resampler rather than anything in the port — the kind of
difference that would have been easy to read as a porting error.

Two things fall out of writing it rather than calling it:

- **Timestamps.** Each token keeps the encoder frame that emitted it, so the
  transcript carries 60 ms alignment. See the segmentation section.
- **The stray-capital filter works.** On the clip where the model interleaves
  isolated capitals, the reference emits `SESE`, `NCT`, `SNB` and `ST`; the port
  removes exactly those and leaves the rest of the text identical. It runs on
  tokens, not on the finished string, so it cannot take "OK" or an initial with
  it.

**Measured on the desktop CPU, at `q8`:** 0.41 s for an 8.8-second clip, 1.79 s
for 32 seconds, 2.02 s for 38 — about **RTF 0.05** on four threads, with the
reference at 1.50 s for the same 32 seconds on eight.

An earlier note here claimed "32× to 53× real time" — RTF 0.031 to 0.019. That
was wrong, and the reason is worth keeping: the clips are 24 kHz, their
durations had been computed as though they were 16 kHz, and every one came out
1.5× too long. The times were real; the speed-ups derived from them were not.

### On the device: one call, and a page to read it with

The C ABI is `yoai_voice_*`, and `yoai_voice_analyze` returns **one JSON
object carrying both halves** — transcript, the four tags, per-token times, and
the prosody profile with each utterance's measurements and its deviations.

The halves are independent in the ABI the way they are in the argument.
`prosody` comes back on a build with no recognition engine at all, and on a
device where the user has never picked a model. `transcript` is the optional
one. That ordering is not a detail: the half that needs no model is the half the
emotional reading comes from.

Each utterance also carries the words spoken inside it, matched by the token
timestamps. That is the table from the top of this file, produced by one call —
two independent measurements of the same seconds, side by side, with nothing
having read the content to place them.

**The bridge, and the two things in it that are not obvious.** `nativeVoiceLoad`
and `nativeVoiceAnalyze` run off the ArkTS thread, because 254 MB off disk and a
second of encoder are both long enough to drop frames. And the samples are
**copied on the ArkTS thread before the worker starts**: the `ArrayBuffer` they
arrive in belongs to the JS heap, and nothing stops it being collected or
detached while a worker reads it.

**Capture is opened at 16 kHz mono**, which is what the analysis wants, so
nothing resamples. The core refuses any other rate rather than converting. That
is deliberate and this file has already paid for the lesson: a resampler that
differs from the one a measurement was taken with changes the answer quietly.

**`/asr` in the composer opens the debug page.** Typed, not discoverable — it is
a developer page, and someone who needs it knows its name. It shows the prosody
profile first, the transcript second, and the model's own emotion tag last,
labelled as not trusted with the reason. Below five utterances it shows the
reason it is showing nothing.

**It runs on the device now.** The signing profile that blocked this — issued
for the bundle name that predates `8dcabe5 rename: YoAI becomes Body Clock` —
has been reissued, and the page has been driven through recordings on a Kirin
9020.

**Speed in this file is RTF — compute time divided by audio duration, so under
1 is faster than the audio and over 1 is slower.** One figure, one direction,
because this file used to carry two. "5.8× real time" for recognition meant fast
and "3.7× real time" for the omni Talker meant slow, both written with an ×,
four hundred lines apart. So an × here is always a ratio between two
measurements — one device against another, one stage against itself — and never
a speed on its own. Tools print the reciprocal too: `llama-tts` reports "audio
time = 3.82x process time", which is RTF 0.26. Where a raw tool figure is quoted
it is marked as one.

**Encoder speed, measured**, swept with `yoai_voice_set_threads` without
reloading the weights:

| threads | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| RTF | 0.392 | 0.259 | **0.173** | 0.163 | 0.161 |
| against one thread | 1.0 | 1.51 | 2.27 | 2.40 | 2.43 |

**RTF 0.17 at four threads**, and four is where it stops paying: eight buys 7%,
which is 0.07 s on a four-second clip, against two more cores taken from the
UI. The default stays at 4.

**So recognition is not the term to attack.** A ten-second note costs 1.7 s.
That matters because the chain's own comment had it at "1.6× real time, already
known" — RTF 0.63, unmeasured, wrong by 3.6×, and wrong in the direction that
would have aimed the first optimisation at the cheapest stage.

Microphone gain and room noise are still unmeasured.

### The whole chain, measured: 1.4 seconds of silence

Recognition, an on-device answer, and the platform voice — end to end on the
Kirin. **The only number is the silence**: from the moment the person stops
talking to the first sound coming back. Everything after that first sound is
heard rather than waited through, because the platform voice speaks at RTF 1
while the model is still writing the next sentence.

| | first exchange, cold | second in a row | first exchange, warmed |
| --- | --- | --- | --- |
| model load | 3.42 s | 0 | 0 |
| recognition, per clip | 0.62 s (3.78 s of audio) | 0.51 s (3.10 s) | 0.55 s (3.30 s) |
| TTS engine start | 0.39 s | **0.00 s** — kept | **0.00 s** |
| prefill | 2.43 s, 0 of 119 reused | **0.29 s, 102 of 115** | **0.35 s, 102 of 116** |
| decode to the first sentence | 0.59 s (8 tok) | 0.62 s (9 tok) | 1.04 s (15 tok) |
| **silence** | **7.57 s** | **1.37 s** | **1.53 s** |

**The gap between the first two columns was the whole product decision, and the
third column closes it.** A session's first exchange cost five and a half times
its steady state, all of it in three costs that do not depend on what the person
said: the model load, an empty prefix cache, and creating the TTS engine.

**Warming on intent, not on entry.** They are paid when the chain is switched
on — about 5.5 s from the toggle to ready, spent while the person is still
deciding what to say — rather than when the page opens, because reading 1.28 GB
for somebody who came to look at a prosody profile is the page charging for a
feature they did not ask for. The prefix cache is filled by asking the model
something and discarding the answer; there is no prefill-only entry point.

With that, **the first recording of a session behaves like the steady state**:
1.53 s against 1.37 s, and the difference is a longer reply (15 decoded tokens
against 9), not a colder engine.

**In the steady state there is no dominant term left**: decode to the first
sentence 0.62 s (45%), recognition 0.51 s (37%), prefill 0.29 s (21%). That is
the signal to stop. Any single fix from here is worth two or three hundred
milliseconds.

It is also a caution about reading proportions. Recognition was 17% of the cold
exchange and the conclusion drawn from that was "not the term to attack" — true
of that exchange, and it became 37% the moment prefill collapsed. A share is a
statement about the other terms as much as its own.

**Against the omni lane, for the same job**: 1.37 s of silence here against
14.3 s before omni's first block of audio, and this one then speaks at RTF 1
while omni streams at 3.7. Three engines that each do one thing beat one that
does all three, by an order of magnitude, on the only number a person
experiences.

## The platforms do not offer the same thing

| | Recognition | Reliable? |
|---|---|---|
| **iOS** | `SFSpeechRecognizer` | Yes. On-device since iOS 13, broad language coverage, stable API |
| **HarmonyOS** | HMS Core Speech Kit | Likely. `@hms.ai.speechRecognizer` exposes an online/offline flag; every HarmonyOS device is Huawei hardware |
| **Android** | `SpeechRecognizer` | **No** |

Android is a façade over whatever recognition service is installed, normally
Google's — so it is **absent without Google Play Services**, which is most
phones in China. `createOnDeviceSpeechRecognizer()` arrived only in API 31 and
whether an on-device model exists is up to the vendor. Behaviour varies by OEM,
which is the kind of dependency that works on a developer's desk and fails on a
user's. Verify on target hardware; the specifics move.

**Hence bundled recognition as the baseline**, identical on all three platforms.
System recognition stays an optional fast path where it is genuinely better —
iOS especially, where it is free and covers more languages.

**Synthesis is the easier half.** All three platforms have usable offline
text-to-speech, and a wrong reading costs far less than a wrong transcription.
Use the system's.

## Why not an omni model

The obvious alternative was one model taking audio in and producing text with
tone included. It was tested at length. Two things rule it out; only the first
is settled.

### Throughput, which is the settled part

Measured elsewhere on the real target: a Kirin 9020 at 16 GB running Qwen3.5-2B
`Q4_K_M` (1.28 GB) gets **~43 tok/s prefill and 11.2 tok/s decode** against a
read bandwidth of ~~**19.5 GB/s that a single core already saturates**~~ — both
halves of that clause were later measured false, and the corrections are two
paragraphs down. The device reads **24.4 GB/s**, and it takes four cores.

That figure is worth being able to re-derive: decode reads every weight for
every token, so a model of size S cannot exceed bandwidth/S tokens per second —
15.2 for a 1.28 GB model, against 11.2 observed. No other compute unit moves
that wall; measured the same way, the Vulkan GPU came out **3.5× slower** than
CPU and the NPU's ceiling sat below it. Prefill is the exception, being
compute-bound.

**Confirmed at two sizes, through this app's own lane**, on the same prompt
(「您好，请问几点钟了？」, 19 tokens):

| | Size | Decode | Weights read | Of the 19.5 | Of the measured 24.4 | Load |
| --- | --- | --- | --- | --- | --- | --- |
| Qwen3.5-2B `Q4_K_M` | 1.281 GB | 14.0 tok/s | 17.9 GB/s | 92% | 73% | 5.9 s |
| Qwen3.5-4B `Q4_K_M` | 2.741 GB | 6.70 tok/s | 18.4 GB/s | 94% | 75% | 14.5 s |

**Decimal GB throughout**, because that is the unit the 19.5 is in and mixing
the two is a silent 7% error. A first version of this table used GiB under a GB
label and reported 85–88% against the 19.5 instead of 92–94%.

Two points, and the 4B's rate was predicted as ~6.5 before it was run. **The law
is what is settled**: doubling the model halves the tokens per second, because
decode reads every weight for every token. Which constant it runs into is a
separate question, and the last column is the one that turned out to matter.

**Two corrections that arrived later, both further down.** "A single core
already saturates it" is false — a thread sweep puts saturation at four, the
performance-core count. And **19.5 GB/s is not the chip's ceiling**: measured
with a read loop and no model at all, the device does at least 24.4 GB/s, of
which llama.cpp's decode uses 73–75%.

The inverse-with-size law holds and the two-point table above is still the way
to predict a model's decode rate **for this engine**. What does not hold is
reading that constant as physics. Anything below that argues "nothing can be
faster because the bus is full" needs re-deriving against 24.4, and the
headroom is real: MNN's thinker already decodes above what llama.cpp reaches.

Prefill came out 41.3 and 16.9 tok/s, but on a 19-token prompt that is mostly
fixed overhead and should not be quoted.

Audio is expensive to feed in: `mtmd` encodes roughly **25 audio tokens per
second** of clip (derived from Whisper's 50 Hz frames, pooled by two), so a
minute of speech is ~1500 tokens of prefill. Scaling to Qwen2.5-Omni-3B —
derived — gives ~26 tok/s prefill, so **60 seconds of audio costs about 58
seconds** before the answer starts. It scales linearly, so short clips do not
rescue it.

Recognition first changes that by an order of magnitude, because what reaches
the model is smaller: a minute of speech is ~200 text tokens, about 5 s of
prefill. Memory goes the same way — thinker `Q4_K_M` plus its mmproj is 4.7 GB
and the mmproj is not optional, against 1.53 GB for SenseVoice `q8` plus a 2B
text model, of which the recognition half is loaded only while transcribing.

**Measured** here for comparison: Qwen2.5-Omni took 36 s on the 48-second clip
that SenseVoice handled in 1.5 s. Twenty-four times — **but that pair is not
like for like**, and reading it as "recognition is 24× slower" is the mistake it
invites. The 36 s is a whole exchange, encoder and prefill and generation; the
1.5 s is transcription with no language model in it at all.

**The encoders are close. It is what they emit that differs.** Both measured on
the Kirin:

| | encoder RTF |
| --- | --- |
| SenseVoice, 4 threads | **0.173** |
| Qwen2.5-Omni's audio encoder in MNN | ~0.31 |

Under two times apart. And the omni thinker's 9.3 s splits **audio 1.57 s,
prefill 5.68 s, decode 2.01 s** — the encoder is 17% of it and the prefill is
61%.

That is the whole argument, arriving as a measurement rather than a derivation:
an audio encoder hands the language model ~25 tokens per second of clip, and
every one is prefilled. A transcript of the same speech is nearer 4 tokens per
second. **Recognition first is not a faster encoder, it is a sixfold shorter
prompt** — and on this device that came to 5.68 s against about 1.5 s, of which
2.6× is the token count and 1.5× is 3B against 2B.

(The ~0.31 is inferred: that run logged its token count but not its clip length,
which is the bug the logging fix in `src/voice/omni/qwen25_omni.cpp` was for. The next run
prints the length directly.)

**The derived ~26 tok/s was close.** Measured on the phone through MNN, with the
prefill separated out: **182 tokens in 5.68 s, 32.0 tok/s**. So a minute of
speech — ~1500 audio tokens plus the template — is about **49 seconds** before
the answer starts, against the 58 s this paragraph derived. The derivation was
pessimistic by a fifth and right in every way that mattered.

**That extrapolation assumes prefill is linear in the token count, and it was
checked.** Two phone runs looked like it might not be: 182 tokens took 5.64 s
and 150 took 5.50 s, which fits a large fixed cost far better than a rate. A
large fixed cost would make long clips much cheaper than the line predicts, so
it mattered. Two desktop runs on the same clip with deliberately different
prompt lengths:

| prompt tokens | prefill | average |
| --- | --- | --- |
| 264 | 3.09 s | 85.4 tok/s |
| 465 | 5.75 s | 80.9 tok/s |

13.2 ms per token with an intercept of **−0.4 s** — no fixed cost, and the
average creeping up with length is attention's quadratic term showing through.
So the line holds and the phone pair is not structural. Most likely thermal:
those two runs were three minutes apart and the second followed a 38-second
Talker run on eight threads.

### Whether it hears tone at all — and three conclusions withdrawn

The original test was a two-way control. **Control 1** held the voice fixed and
changed the words: two clips from one Windows TTS voice, same flat delivery.
The verdict flipped from "a sense of exhaustion" to "not tired… calm and
collected". **Control 2** held the words fixed and changed the sound, by
re-rendering one real recording at three tape speeds. A 1.9× tempo ratio and a
large pitch shift left no trace.

That was reproduced here independently, and extended to seven clips across seven
speakers, where the model's verdict matched the emotional lean of the *words*
every time.

**And then it failed to hold, on better material.** Cutting one real recording
into four segments — same speaker, same take, so the only variable is what is
being said — the model called the *cheerful* opening "slightly tired", crossing
the content's lean. Asked the same question about the transcript with the audio
replaced by silence, it answered "happy" for that segment. **Same model, same
prompt, audio is the only difference, and the answer changes.** Qwen2.5-Omni
does use the audio.

Three things written here earlier are therefore withdrawn:

- **"The model reads the words, not the voice."** Overstated. The seven-clip
  table that supported it drew on seven different speakers, so acoustic and
  semantic differences were confounded; that table cannot separate the two
  explanations, and it was read as though it could.
- **"SenseVoice's emotion head is systematically broken."** It has no label for
  the low-arousal band at all. Missing vocabulary, not missing perception.
- **"Prosody features cannot find it, so that path is closed."** They were being
  asked to find *sadness* — a valence judgment — and at ten-second granularity.
  Asked for arousal at utterance granularity, they locate the emotional turn
  precisely, as the table above shows.

The pattern across all three: a measurement that could not distinguish two
explanations, read as though it had.

**None of this revives the omni path on-device.** The throughput wall is
independent of whether the model hears.

### The larger model was run, and it hears clearly

Two things written above are out of date. **llama.cpp does run Qwen3-Omni** —
`ggml-org/Qwen3-Omni-30B-A3B-Instruct-GGUF` ships `Q4_K_M` plus an mmproj, and
it loads and answers through `llama-mtmd-cli`. And the **base Instruct model was
enough**; the Captioner variant was not needed to get a clear result.

**Measured**, `Q4_K_M` plus a bf16 mmproj on an RTX 3090, 19.3 GB of 24, one
6.5-second clip of real Mandarin put through control 2 — the same words at three
tape speeds:

| | sound | what the model said |
|---|---|---|
| slow, 9.0 s | pitch down | "**mid-to-low pitched**… **moderate, conversational pace**… **His** delivery" |
| original, 6.5 s | — | "**fast-paced**… pitch consistently **high**… **Her** pitch" |
| fast, 4.8 s | pitch up | "**fast-paced**… **high-pitched**, animated" |

Pitch and pace both track the manipulation, and **the perceived speaker's sex
flips with the pitch shift** — which is what a pitch shift does to a listener,
and is not available from the words, because the words are identical in all
three. The semantic read stayed put across all three ("exasperated",
"animated"), which is the correct behaviour: the content-driven part should not
move when only the sound does.

The 3B, given the same treatment, said "moderate pace, low pitch, calm" for all
three.

So the question the two controls were built to answer has an answer. **It is a
capacity problem, not an impossibility.** A model of this class hears tone; the
one small enough to put on a phone does not.

This does not change the on-device plan — 19.3 GB is a server — but it changes
what the on-device plan *is*. Signal processing is not a fallback chosen because
the model idea failed. It is the only thing that works at the size that fits,
and it is the part that is validated.

Caveats: one speaker, one clip, one sample per condition. A tape-speed change is
not real emotional prosody; this shows the model is sensitive to acoustics, not
that it judges tiredness correctly. That needs one person recording one sentence
in several real states.

**Half the mmproj is the vision tower, and it is dead weight here.** "mmproj" is
the *multimodal* projector: for an omni model it carries an audio encoder and a
vision encoder side by side, and `clip_init` loads **every** tower the file
declares. There is no runtime switch for audio-only — the single skip in that
function is a hardcoded Gemma-3N special case — so a process that will never be
handed an image pays for the vision weights anyway.

[strip-mmproj-vision.py](strip-mmproj-vision.py) drops them: the `v.*` tensors
and the `mm.0.*` / `mm.2.*` projector MLP go, the `a.*` encoder and `mm.a.*` head
stay, every `clip.vision.*` key goes and `clip.has_vision_encoder` is set false —
that last flag being the one `clip.cpp` actually reads. It is a metadata rewrite,
not a requantization: each kept tensor is copied byte for byte at its original
type, for the reason the `q8`/`f16` section gives at length.

**Measured**, both models, and verified by loading rather than by parsing:

| | before | after | dropped |
|---|---|---|---|
| Qwen2.5-Omni-3B `f16` | 2.44 GiB | 1.20 GiB | 1.25 GiB |
| Qwen3-Omni-30B `bf16` | 2.06 GiB | 1.22 GiB | 0.84 GiB |

On the same clip with greedy decoding, stripped and unstripped give the **same
answer word for word** — on both models. (Qwen3-Omni names its projectors per
modality, `clip.audio.projector_type = qwen3a` beside
`clip.vision.projector_type = qwen3vl_merger`, where 2.5 had one combined
`qwen2.5o`. The script needs no change for either.)

**It does not make generation faster, and cannot.** The mmproj runs once at
prefill; every token after that reads the language model's weights. Measured on
the 3B, alternating runs: 27/31 s unstripped against 30/30 s stripped — inside
the run-to-run noise. What it buys is load time and resident memory, which turns
into tok/s only where VRAM was the binding constraint and layers were being left
on the CPU.

**Practical note for any batch use**: a run is about 40 s, of which roughly 38 s
is loading 17 GB from disk — audio encoding is **198 ms** and inference is
seconds. Anything comparing more than a handful of clips wants `llama-server`
with the model resident, not `llama-mtmd-cli` per clip.

There is also a use for Qwen2.5-Omni that does not need it to be fast: an
**offline check on the signal processing**. It runs on one clip occasionally in
the background; a long-run divergence between the two says the feature weights
have drifted. **Measured**: 17 s for a ten-second clip asked for a single word,
of which 5.6 s is audio encoding and most of the rest is model load, which a
resident model removes.

Its cost advantage over the 30B is smaller than the sizes suggest, and the
smaller model is the slower one where it counts. **Measured** on one CPU
machine, same 8.8 s clip, so the two numbers are comparable in a way the figures
above are not: audio encoding takes **4465 ms on Qwen2.5-Omni-3B** and **477 ms
on Qwen3-Omni-30B**. The 3B is cheaper to keep resident, which is the whole
argument for it here; it is not cheaper per clip.

## The engine question, and why it stopped being one

The first version of this problem was posed as a choice between two engines:
**modify llama.cpp to support omni speech output, or move to Alibaba's MNN**.
MNN is a reasonable thing to have asked about — it is Alibaba's own on-device
runtime, so the Qwen family is first-party there in a way it is not upstream,
and the gap it would have closed is real. The section below documents that gap
precisely: there is no Talker checkpoint in GGUF, and llama.cpp's audio
generation is a TTS stack driven by its own backbone rather than by a chat
model's hidden states. On end-to-end omni speech out, llama.cpp cannot do it.

**The choice was never made, because the requirement went away.** Once
recognition and synthesis moved outside the language model — platform TTS for
output, SenseVoice on ggml for input — nothing was left that llama.cpp lacks.
There was no capability to switch engines *for*. That is a better outcome than
picking the right engine, and it is the reason this file spends its length on
where the axes get their numbers rather than on runtimes.

**If it does come back, this is the shape it takes.** Worked out rather than
left to be re-derived:

- **Prosody runs on every recording, on every lane.** It is what goes in the
  store, and the store is longitudinal. A field computed one way on Tuesday and
  another way on Wednesday is worse than one not computed at all, so the
  measurement path cannot depend on which engine answered.
- **SenseVoice runs too**, even with an omni model present. The text axes read a
  transcript, and a transcript from an omni model is generated rather than
  decoded: no frame alignment, where CTC hands over 60 ms timestamps that the C
  ABI already uses to attach words to utterances. And it costs a full audio
  prefill against one encoder pass.
- **So the omni lane's genuine addition is exactly one thing: the voice on the
  way out.** Not a parallel pipeline — an optional output stage.

That makes the engineering small and the product rule obvious: **its absence is
the normal case, so the interface must never promise it.** It is tied to
whichever model the engine supports, and it disappears the day the user moves to
a better thinker.

**What would put it back on the table**, so this is not re-argued from memory:

- Wanting the model's own voice out — a real omni Talker, the speaker's
  characteristics carried through into the reply — rather than the platform
  reading text aloud. That is the original motivation, and it is still
  unavailable upstream.
- Finding that ggml is a poor fit for some encoder worth having. So far the
  opposite: SenseVoice took 400 lines against the ggml this repo already ships.

### MNN was built and measured

**The capability is real, and it works.** MNN carries what llama.cpp does not: a
`Talker` with `setWavformCallback` streaming float PCM, behind a
`predit → dit → bigvgan` chain. `taobao-mnn/Qwen2.5-Omni-3B-MNN` ships all of it
— thinker, audio encoder, talker, DiT, vocoder, speaker dictionary — 3.8 GB.
Built with `MNN_BUILD_LLM_OMNI=ON`, clean.

**Measured**: an 8.8 s clip in, a one-sentence answer about how the speaker
sounds, and **4.0 s of genuine speech out**, in one call. That is the thing this
whole engine question was ever about.

**Install the waveform callback AFTER `load()`, or it produces silence.** One
line, and the whole shape of the call hangs off it:

```cpp
llm->load();                                 // this is what CREATES the Talker
llm->setWavformCallback(sink);               // so this has to come after it
llm->response(prompt, &os, "", 512);         // then one ordinary call
llm->generateWavform();
```

`Omni::setWavformCallback` forwards to the Talker and guards with
`if (mTalker)`. Called first, it is a **silent no-op**: no error, no warning,
and the Talker's own `doGenerate()` is `mWavformCallback != nullptr`, so it then
skips collecting hidden states and skips generating. The result is a clean run
that produces no audio.

The `""` is `end_with`, a terminator MNN appends to the output stream when
generation stops. It is not a token the model emits. Passing `"<eop>"` puts that
text at the end of every reply; passing `NULL` gets a newline, because MNN reads
NULL as "use the default".

**An earlier version of this section prescribed something else**, and it was
wrong:

```cpp
llm->response(prompt, &os, "<eop>", 0);   // prefill ONLY
while (...) { llm->generate(1); }         // step the decode by hand
```

That was reverse-engineered from MNN's Android app while the callback bug was
still in place, on the theory that a completed `response()` marks the session
finished before the Talker can read it. It does not — the Talker collects an
embed on every forward pass of an ordinary call. The hand-stepping only put MNN
into a state its own code never produces, and on the device that crashed: the
Talker's prefill concatenates `mTalkerEmbeds[0]` and `[1]` with no bounds check,
so one embed short is an out-of-bounds read on a vector of one. MNN's own
interleaved path guards the same spot with `hasEmbeds()`.

**So: the thinker must produce at least two tokens before the Talker is asked to
speak**, and the graph must publish `talker_embeds` at all —
`getOutputIndex("talker_embeds")` says so at load, which is much better than
finding out after a recording.

**One nice thing** found on the way: where the GGUF mmproj needed a tensor-level
rewrite to drop the vision tower, MNN reads `is_visual`, `is_audio` and
`has_talker` as plain booleans out of `llm_config.json`. Skipping the 353 MB
vision encoder is one edited word.

#### The real defects, and one that is not

**`use_mmap` breaks the thinker on ARM.** **Measured on the device**: with
`use_mmap: true` the thinker's prefill dies in shape computation, before a token
exists —

```
MNNJNI: Compute Shape Error for /Unsqueeze_output_0
MNNJNI: onForward returned no outputs. seqLen=182, inDecode=0
```

— and the same build, the same model and the same call sequence answer
correctly with the flag off. The desktop cannot see it: MNN's mmap path fails
outright on Windows (`MNNMmapFile failed ... size=1073741824`, repeatedly) and
falls back, so x86 runs the non-mmap path either way.

Worse than the failure is its **stickiness**: MNN sets `INTERNAL_ERROR` and
every entry point checks the status first, so every later call returns in
milliseconds having done nothing. Without a status check of its own the caller
sees an empty answer rather than a broken engine, forever, until the model is
loaded again.

**The control** was a desktop program driving the engine exactly as
`src/voice/omni/qwen25_omni.cpp` does — an in-memory waveform through `MultimodalPrompt`,
one `response()` call — rather than MNN's own demo. It answered correctly,
**263 prompt tokens to 35 generated**, which is what narrowed the difference
between the two machines down to the overrides this file had added.

**The mechanism is not understood**, and the pairing is odd enough to say so:
shape computation runs before a kernel is chosen and should not care how weights
were loaded. What is established is the correlation, from a controlled
comparison with one variable.

**What the flag was buying, measured**: load took 2.3–3.1 s with it and
7.9–8.3 s without — so roughly five seconds, once per session. Resident memory
was not measured, which is the half that mattered for adding it: with mmap on
the process held 3.5 GB, and nothing says what it holds now.

**Not a defect, and worth having before the next knob is turned: the two config
knobs are measured.** `precision` and `memory`, all five combinations that load,
on the desktop at 209 prompt tokens:

| precision | memory | prefill | decode |
| --- | --- | --- | --- |
| low | low | 80.7 | **19.8 tok/s** ← the model's `config.json` |
| normal | low | 81.7 | 20.2 |
| high | low | 80.8 | 20.6 |
| low | normal | 58.0 | 5.6 |
| high | normal | 56.0 | 5.0 ← `config_high.json` |

**`memory` is the whole cost and `precision` is free.** Low memory keeps the
weights 4-bit and uses the quantized GEMM; normal dequantizes and loses it, for
a 4× decode penalty and nothing in return. The precision column measures nothing
on x86, which reports `fp16:0` — all three compute in fp32 there. On the phone
it would be a real choice, and `precision: high` was tried on the device against
the shape error above and changed nothing.

**Intermittent segfault in text-in/speech-out.** One crash in two runs on the
desktop. The audio is written first and the crash comes during teardown, so the
product is delivered — but a crash is a crash. Not re-tested since the callback
ordering was fixed.

**Audio over ~30 s segfaults — and "over ~30 s" was wrong.** **Measured** at
the time: 8.8 s and 28 s fine, 31 s, 32 s, 37 s and 38 s all segfault, within
ten seconds of starting, during the audio front end and before a token is
generated. That reads as a threshold, and `Omni::max_seconds()` was set to 28
on the strength of it.

**Re-measured on a PC, it is not a threshold. It is not deterministic.** Ten
runs of the same two clips through MNN's own `llm_demo`:

| clip | outcome |
| --- | --- |
| 10 s, 14 s | ok |
| **18 s** | crash, ok, crash, crash |
| 22 s | crash |
| **26 s** | ok, crash, crash, crash |
| 28 s, 30 s | crash |

**The same file, the same command, both outcomes.** The bisect gave that away
before the repeats did — 26 s passed where 18 s and 22 s had failed, and a
length limit cannot do that.

So there is no limit to find, on this machine or any other, and the question
of whether the limit differs per device has no threshold to compare. What can
differ is the PROBABILITY. `max_seconds() = 28` does not prevent this crash; it
shortens the input, which does less work and gives it fewer chances to fire.
That is worth something and it is not what the constant says it is.

Reading the code says why there is no clamp to find. Qwen2.5-Omni's encoder
takes two inputs, so `Omni::audioProcess` takes the branch that builds a
`seqlen × seqlen` attention mask and forwards — and the only length clamp in
that function, `dim[2] > 3000` with the comment "Qwen2-Audio just support audio
time <= 30s", is in the OTHER branch, the single-input one, which omni never
reaches.

**Segfaulting instead of saying so was always the defect. It is a worse defect
than it looked**, because a caller cannot avoid it by staying under a number.

**Still true at the current pin.** MNN moved from `f90018e` to `04ab6f2` on
2026-09-21, and two of those ten commits are Omni bugfixes — attention-mask
routing by mode, and a PLE refresh between successive text requests. Neither
touches `audioProcess`, which is byte-identical across the update, so the
paragraph above is not stale, only unfixed.

**Not a defect: the cost.** **Measured**: about 43 s of compute for 14.4 s of
speech — roughly **RTF 3** on a desktop CPU.

**That desktop ratio has not been re-derived with a wall clock**, and it is
written as "seconds of compute" — which is what `clock()` returns, summed across
threads. The phone's first figure was inflated five times for exactly that
reason. If this one carries the same defect it reads **high**, the desktop is
faster than RTF 3, and the phone is not "barely worse" but several times worse.
Every comparison below that leans on 3.0 against 3.7 rests on this.

#### On the phone: RTF 3.7

This is the number the file had been missing, and the one the decision below was
explicitly waiting on. **Measured** on the target device, Qwen2.5-Omni-3B in
MNN, four threads, the model's own `config.json`:

| | |
| --- | --- |
| Load, from app storage | 7.9 s |
| Thinker, 194 prompt tokens → 16 generated | 17.2 s |
| Talker, 10.1 s of speech | 37.5 s — **RTF 3.70** |
| Whole exchange, for 4.8 s of recorded audio | 46.2 s |

**The ratio covers the Talker alone**, which is the half that was ever in
question — `generateWavform()`, from codec tokens through DiT to the vocoder.
The thinker is the rest of the exchange and is billed separately above.

A third run, with the thinker broken down and nine blocks of audio instead of
two — MNN's own counters, read off the public `LlmContext`:

| | |
| --- | --- |
| Thinker, 182 prompt tokens → 32 generated | 9.3 s |
| — audio encoder | 1.57 s |
| — prefill, 182 tokens | 5.68 s (**32.0 tok/s**) |
| — decode, 32 tokens | 2.01 s (**15.9 tok/s**) |
| Talker, 10.34 s of speech | 38.5 s — **RTF 3.7** |
| First block of PCM, from the start | 14.7 s |
| Blocks | 9 |
| Whole exchange | 47.7 s |

**Prefill is 61% of the thinker**, which is the structural claim two sections up
arriving as a measurement: audio reaches the model as ~25 tokens per second of
clip, and every one is prefilled. Recognition first is cheaper because it hands
over a tenth as many tokens, not because the model is smaller.

Thinker time varies more than the token counts explain — 194 tokens took 17.2 s
against 182 in 9.3 s. The slow one was the first call after a load, so some of
that gap is cold cache rather than the model.

**The Talker ratio is stable across every run and the block count is not.** Four
exchanges gave RTF 3.5 to 3.7 with no attempt to control for anything, which is
what makes it worth deciding on. Blocks were logged on two of them: 2 and 9, for
replies of 2.26 s and 10.34 s — so MNN's chunking tracks the reply's length
rather than being a fixed cadence, and the short run's two blocks made streaming
look more useless than it is. It is the nine-block run that says what streaming
actually does.

**Against the desktop**, on the same engine and model: `llm_demo` there gets
80.7 tok/s prefill and 19.8 decode, against 32.0 and 15.9 on the phone. Prefill
is 2.5× slower on the device, decode only 1.2×. The split is the expected shape
— prefill is compute-bound and decode is memory-bound — and it says which half
to attack.

**One number here did not fit**, and chasing it found two wrong sentences
elsewhere. The thinker decodes at 15.9 tok/s against a 1.74 GB weight file,
which is **27.6 GB/s** of reads — half again the 19.5 GB/s the section above
calls a wall that a single core already saturates. The number turned out to be
real; both the "single core" claim and the first attempt to explain the number
away did not.

#### The thread sweep, and what it disproves

**Measured** on the device through the `/asr` bench, Qwen3.5-2B `Q4_K_M`
(1.28 GB) in llama.cpp, two runs per setting, the model resident throughout —
`yoai_local_set_threads` changes the count without dropping the weights,
so no run pays for a cold page cache:

| threads | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| decode, tok/s | 5.8 | 9.0 | **12.6** | 12.7 | 12.9 |
| implied reads, GB/s | 7.4 | 11.5 | **16.1** | 16.2 | 16.5 |
| prefill, tok/s | 22.7 | 29.0 | **48.4** | 46.3 | 50.1 |

**"A single core already saturates it" is false.** One core reaches 7.4 GB/s,
45% of what the chip gives up at saturation and 38% of the 19.5. It takes four.

**Both phases stop at four**, which is this SoC's performance-core count — one
big, three mid, four little — so the little cores contribute nothing to either.
The project's default of six threads was one cluster too many and is now four,
for the same throughput and two more cores left to the UI.

What survives is narrower than it first appeared. llama.cpp's own ceiling of
15.2 tok/s for a 1.28 GB model holds — 12.9 measured is 85% of it — so within
this engine, threads are not the lever.

**But the plateau is not the bus.** It arrives at the performance-core count,
which is as consistent with running out of useful cores as with filling the
memory controller, and a sweep of one engine cannot separate those. Measured
directly two sections down, the chip reads at 24.4 GB/s and llama.cpp's decode
uses 73–75% of it. The threads stop helping before the memory does.

#### The 27.6 GB/s survives the check, and the wall is llama.cpp's

**The denominator was checked and it is right.** A previous version of this
section explained the 27.6 away as a divisor error — 1.74 GB read as the whole
bundle, the thinker's own weights guessed at 1.05 GB, which lands on 16.7 GB/s
and agrees with the sweep. It does not survive the numbers it asked for:

- **`llm.mnn.weight` is the thinker alone**, 1,737,291,202 bytes (1.618 GiB).
  The bundle is 3.77 GiB across 24 files, and the audio encoder, Talker, DiT and
  vocoder are separate files beside it. The per-file listing was already in the
  device log that produced the 27.6.
- **The graph accounts for every byte of it.** `llm.mnn.json` has 327 ops
  carrying external blobs, and they sum to exactly 1,737,291,202 —
  1655.7 MiB in `Convolution`, which is how MNN stores a Linear, plus 1.2 MiB of
  `LayerNorm`. The model is dense: no routing, no conditional layers, so decode
  reads all of it for every token.

**And the inference undercut itself.** "llama.cpp cannot exceed ~16.5 GB/s on
this chip however it is threaded" treats the sweep's plateau as a property of
the bus — but the sweep explains that plateau two paragraphs earlier by the
performance-core count, *the little cores contribute nothing*. That is a compute
explanation. A curve that flattens at the number of usable cores cannot by
itself tell you the memory bus is full; it is equally consistent with having run
out of cores worth using.

#### Measured without a model: the chip does at least 24.4 GB/s

`yoai_memory_bandwidth` reads a 256 MiB buffer with no engine involved —
threads claiming 4 MiB chunks from a shared counter, four accumulators to keep
loads in flight, the buffer written before the clock starts. Swept the same way
as the decode bench, on the same device:

| threads | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| GB/s | 6.5 | 10.6 | 20.9 | 21.9 | **24.4** |

**19.5 GB/s is not a hardware wall.** A few dozen lines of ordinary C beat it by
a quarter, and llama.cpp's decode implies 17.9–18.4 GB/s — 73–75% of what the
chip gives up to a read loop. So the constant this file has been dividing by was
one engine's throughput all along, and "no other compute unit moves that wall"
was only ever tested against engines with llama.cpp's access pattern.

**24.4 is a lower bound, not a peak.** Each fix to the benchmark raised it, which
is the sign of a measurement not yet limited by the thing it is measuring:

| | 1 | 4 | 6 | 8 |
|---|---|---|---|---|
| equal static slices | 6.0 | 18.2 | 16.0 | 21.4 |
| chunks claimed from a counter | 5.5 | 18.2 | 22.7 | 23.0 |
| plus a 2 MiB-aligned buffer | 6.5 | 20.9 | 21.9 | **24.4** |

The first row contains its own refutation: **six threads slower than four**, five
runs each. Equal slices on one big, three mid and four little cores means the
timer waits for a little core carrying a sixth of the buffer while the fast ones
idle. That is not the machine's bandwidth, it is the bandwidth of an unlucky
split — and it inverted the very ordering the table exists to show.

**Huge pages were tried and are not available.** 256 MiB of 4 KiB pages is 65,536
translations against a TLB holding a few thousand, so a page walk every 512 loads
was a plausible reason the loop might understate. `madvise(MADV_HUGEPAGE)` was
refused by this kernel. It is worth recording as closed rather than untried,
because it also means MNN does not get huge pages for its weights either: page
size cannot explain any gap between the two.

**What is left unexplained is 13%**, and it is left there. MNN's thinker implies
27.6 GB/s against a measured 24.4, which is inside the range that "the read loop
uses scalar 8-byte loads and has not been tuned" covers — every previous fix
moved it further than that. Chasing it further would not change anything below:
the wall is higher than 19.5 either way, and by enough that the arithmetic in
this file should be re-derived rather than patched.

**MNN's side was cross-checked and holds.** The 15.9 tok/s comes from MNN's own
counter, so it was verified against wall-clock differences between runs instead:
184 prompt tokens to 8 generated took 7.5 s, and to 25 generated took 8.6 s —
17 tokens in 1.1 s, **15.5 tok/s**, arrived at without touching MNN's timers.

**The desktop extrapolation was right in direction**, and how far is not
settled. "On a phone it can only be worse" is how the paragraph above ends, and
3.0 became 3.7 — but the 3.0 may be a `clock()` figure, in which case the gap
is wider than it looks. What the thinker measurements do say cleanly, both taken
with a wall clock: prefill is 2.5× slower on the device and decode only 1.2×.

If the phone really is close to a desktop CPU, the reason is that it is not
running the same arithmetic — the device reports `fp16:1` and `i8mm:1`, ARM
kernels x86 has no equivalent of, and the model's `config.json` asks for the
precision that uses them.

Either way the decision below is unaffected, because it never turned on the
desktop comparison. **Ten seconds of spoken reply costs thirty-seven seconds of
the phone's whole attention**, measured on the phone, and the blocks say the
person waits through nearly all of it — see the streaming paragraph in the
decision.

**A caution about how this number is obtained.** The first phone run reported
**RTF 17.0**, and that figure was wrong by a factor of five. The timings used
`clock()`, which on POSIX is processor time summed across threads; the engine
runs four. The error was caught by comparing the printed durations against the
log's own timestamps — 87.9 s printed, 18.2 s elapsed. Anything multithreaded
measured with `clock()` reports several seconds per second, and a ratio built on
it decides the wrong way. The same mistake was in the recognition timings on the
`/asr` path.

#### What this correction replaced, and why it is kept

An earlier version of this section listed five defects and concluded "not
adopted". **Four of the five were not defects.** They were one API misuse — a
callback installed one line too early — plus a config file this project had
broken itself, plus a standard window limit read as a capability gap. The
conclusion built on them had no support.

**The correction was itself half wrong, and that is part of the lesson.** It
named the misuse as driving generation the wrong way, and prescribed the
hand-stepped decode above. Reading the shipping app did find *a* working call
shape; it did not find *why* ours was silent, and the difference went unnoticed
because the recipe appeared to work. The real cause surfaced only later, from
MNN's source: `setWavformCallback` guards on a Talker that `load()` has not
created yet.

The mistake is worth more than the measurements. Faced with output that did not
work, the response was to turn configuration knobs — precision, the `mllm`
block, sync against async — four rounds of it, and then to promote "I could not
make it work" into "the engine is defective", in a written decision.

**The first question was never asked: has anyone already got this working?**
They had. MNN ships an Android app that does audio in and speech out, and ten
lines of its session code hold the answer. Turning knobs feels like work in a
way that reading a working caller does not.

This is the same shape this file records three times elsewhere — a measurement
that cannot distinguish two explanations, read as though it had. "Silence" was
equally consistent with a broken engine and with a wrong call, no experiment
separated them, and the reading chosen was the one that allowed the
investigation to stop.

### Whether to adopt it

**The case for, and the half this file never priced**: it is the only way to get
the model's own voice out, and that works today.

**Its voice is reported to sound more natural than the platform's.** That is an
observation from listening to both, not a measurement, and it is recorded
because everything else here prices only the cost side. A decision that weighs
RTF 3.7 against "one capability" has quietly assumed the capability is
interchangeable with the free one — and if it is not, the trade is a product
judgement rather than arithmetic.

**The obvious objection was checked: the platform is not simply misconfigured.**
An app cannot ask for a nicer voice than the one it is getting.

- `@hms.ai.textToSpeech` lists **three** voices on this handset, and only person
  13 (聆小珊) is INSTALLED. Every one of them has style
  `interaction-broadcast`, and **style cannot be requested** — it appears in
  those typings only as a field on `VoiceInfo`, never in `VoiceQuery` or
  `CreateEngineParams`. `person` is the whole of the choice.
- **The online engine is not a door.** `online: 0` is refused with
  `401 online type error`. An earlier note here said offline was the only engine
  HarmonyOS offers and was demoted to "an assumption"; it was right, and now it
  is checked.
- `@hms.ai.textReader` is a second surface with a richer model on paper —
  `Person { tone, style }`, style as an *input*, an online mode — and driven
  with its defaults it speaks **the same voice**. The panel says 小艺朗读 and
  what comes out is the broadcast timbre.

So the platform's ceiling for a third-party app is that timbre. The voices in
the assistant's own settings menu belong to the assistant and are not reachable
from either API. **The comparison against omni is therefore a fair one**, which
is the opposite of what checking it was expected to show.

One thread is unpulled: `textReader`'s `person` was never passed, and
`interaction-broadcast` is obviously one name in a scheme. The typings enumerate
nothing, so that is guessing string constants, and a wrong guess speaks in the
default and looks like a success.

**The obvious middle path is closed for this model.** Driving the Talker from
text — the cheap chain writes, omni's voice speaks — would skip the thinker's
9 s entirely. MNN has the entry point, `generateTTS`, but it refuses anything
whose `talker_type` is not `qwen3_tts`, and Qwen2.5-Omni's Talker consumes the
thinker's hidden states rather than text. It is not a wiring problem.

**A standalone TTS in the same family would be the route**, and it changes the
size argument though not the speed one. The parts that make sound are talker
391 MB, DiT 348 MB, vocoder 116 MB, predit 17 MB and the talker embeddings
34 MB — about **0.9 GB**, against 3.8 for the bundle. The 3B thinker is the
freight, and it is exactly the part a text-driven TTS does not need. But the
RTF 3.7 was measured on `generateWavform()` alone, so removing the thinker does
not move it.

**What the ratio actually rules out is conversation, not playback.** Everything
above assumes somebody is waiting to be answered. An app reading back something
it prepared — a day's recap — spends RTF 3.7 in the background and nothing in
front of the person. That is a different product, and this file has never
costed it.

Recognition through it is also plausibly better than SenseVoice's
— Qwen2.5-Omni-3B fixed four transcription errors including a polarity flip, in
the table above — though that was measured through llama.cpp and has not been
reproduced in MNN's own quantization, because the reference clip is longer than
the 30 s window.

**The case against, and it is about arithmetic, not defects.** Recognition is
already served by SenseVoice, which is cheap and uniquely gives the 60 ms
alignment the C ABI already uses. Text is already served by llama.cpp,
integrated, carrying the GBNF grammar the text axes decode against. Prosody is
signal processing and needs no engine at all. So a second engine buys exactly
one thing — the voice on the way out — and charges a second model format, a
second quantization story, a second tuning surface, and 3.8 GB.

**The 3.8 GB charges something the size figure hides**: it has to reach the
phone. Twenty-four files whose layout MNN resolves relative to `config.json`,
which nobody is going to copy by hand, so this repo now carries a bundle format
and an extractor to make it one file. That is real code, and it exists only
because of this engine.

**A second engine also means a second set of headers in one binary.** MNN
vendors rapidjson 1.1.0; this repo already vendored a much newer one. Both are
header-only templates emitting the same weak symbols into one `.so`, the linker
keeps one definition, and the newer `MemoryPoolAllocator` holds a `SharedData*`
where 1.1.0 holds `chunkHead_`. The result was a segfault at 0x10 on MNN's first
JSON parse. Not a defect in either library — a cost of carrying two.

**And the one thing it buys runs at RTF 3.7 on the phone.** Measured, in
the table above: ten seconds of spoken reply for thirty-seven seconds of the
device's whole attention. That is the number to argue with, and it is not an
implementation detail that improves with familiarity.

**So: not yet — and now on a measurement rather than an extrapolation.** The
engine works on the device, end to end, audio in and the model's own voice out.
What the phone figure says is that a spoken reply cannot be waited for. Below
about RTF 1 it would be a feature; at 3.7 it is a demo.

**What would change the answer**: Talker throughput near RTF 1, and nothing
else. The capability is proven and the remaining crashes are ours or fixable, so
the ratio is the whole question — and the obvious way around it does not work.

**Streaming is not the rescue, and that is arithmetic rather than an
implementation detail.** The Talker hands PCM over in blocks, so a reply could
in principle start playing before it is finished. **Measured**, on the run with
nine blocks: 10.34 s of speech, 38.5 s of Talker, first block at **14.7 s** from
the start of the exchange against 47.7 s for the whole exchange.

Bringing the first word forward by thirty-three seconds sounds like the answer,
and it is not, because of what arrives after it. Nine blocks over 38.5 s is a
block every **4.3 s** carrying **1.15 s** of audio. Playback consumes a second
per second, so it drains each block in 1.15 s and then waits 3.2 s for the next:
**27% speech, 73% silence**. That is not a reply being spoken, it is a reply
being spelled out.

The duty cycle *is* the ratio — 1 ÷ 3.7 — and that is the general form. **Above
RTF 1 the listener always outruns the generator**, so streaming can only ever
move the first word earlier; it can never close the gaps. Below 1 it would close
them for free.

So chunking, `n_window`, and MNN's interleaved mode are all the same idea and
all bounded by the same number. They are worth building **after** the ratio goes
below 1, not instead of getting it there.

#### The diffusion steps were the one free lever, and they are worth 0.9 RTF

`dit_steps` is a plain config value MNN reads at generation time, so
`yoai_omni_set_dit_steps` changes it without dropping the weights. MNN runs
`steps - 1` DiT forwards per chunk. **Measured on the phone:**

| `dit_steps` | DiT forwards | RTF | by ear |
| --- | --- | --- | --- |
| 5 (the model's own config) | 4 | 3.7 | — |
| 3 | 2 | **3.0** | acceptable |
| 2 | 1 | 2.8 | audible artefacts |

**About 0.3 RTF per forward, so the diffusion is a third of the bill and the
talker plus vocoder are the other 2.5.** Extrapolated to free diffusion the
ratio still sits near 2.5, and there is no knob on the rest. The lever is real
and it is spent.

It was predicted at RTF 1.5, on the reasoning that the talker's autoregression
costs about 1 — 50 codec tokens a second against 0.392 GB is 19.6 GB/s, inside
what this device reads — and that the remainder was diffusion. The first half
held; the second did not. `predit` and BigVGAN are ~1.4 RTF between them and have
no equivalent setting.

What is left that could move it: the NPU backend, a smaller Talker, or accepting
that something else reads text aloud.

#### On a desktop the split was measured, and it kills the diffusion argument

The "~1.4 RTF for predit and BigVGAN" above is a subtraction, not a reading. It
can now be read, because MNN keeps the numbers and simply does not expose them:
the Talker is a separate `Llm` with a separate `LlmContext`, so none of its work
appears in the engine's own counters. `third/patches/mnn-talker-context.patch`
adds one virtual, `getTalkerContext()`, and `yoai_omni_ask` reports the
three fields it carries. **Desktop, `dit_steps` 5, 2.30 s of speech:**

| | seconds | as a ratio to the speech |
| --- | --- | --- |
| Talker prefill | 0.17 | — |
| Talker decode, 119 codec tokens | **3.57** | **1.55x** |
| token2wav (DiT + BigVGAN) | 3.07 | 1.33x |
| whole exchange | 6.5 | 2.8x |

**The autoregressive half alone is above real time**, at 30 ms per codec token
against 0.392 GB of 8-bit weights — about 13 GB/s, which is what this desktop
manages. `dit_steps` cannot touch that line. With the diffusion free the ratio
still sits at 1.6, and with the floor at 3 steps — the phone table above says 2
has audible artefacts — the realistic best is about 2.3.

So the earlier prediction had the right shape and the wrong half: the talker's
autoregression was assumed to cost about 1 and costs 1.55, and it is now the
larger of the two rather than the cheaper.

**The wait before the first word is a fixed cost and it is fully accounted
for.** Desktop, measured repeatedly: the thinker finishes at 1.2–1.6 s and the
first block of audio arrives at 4.4–5.7 s, regardless of how long the reply is —
an 8-token answer waits as long as a 40-token one. The first DiT chunk needs
60 codec tokens plus 12 of lookahead, so 72 × 30 ms of decode, then one
token2wav pass of ~1.5 s: 3.7 s, which is the gap.

**MNN's quantization is not uniform, and the expensive half is the coarse
half.** From the export's own defaults, confirmed against the shipped weights:

| | bits | source |
| --- | --- | --- |
| thinker | 4 | `--quant_bit`, default 4 |
| audio encoder | 4 | `utils/audio.py` |
| **Talker** | **8** | `utils/talker.py`, forced for `hidden_size <= 2048` |
| **token2wav** | **8** | `utils/token2wav.py` |

Upstream chose 8 for the 3B deliberately — the 7B gets 4 — and for the vocoder
it says why: INT4 on those conv-heavy modules destroyed the output, correlation
0.03 against PyTorch where fp16 held 0.9999. The Talker is an ordinary
autoregressive transformer and is the one worth re-exporting at 4, which halves
the reads that the 1.55x is made of. `--talker_quant_bit` did not exist;
`third/patches/mnn-export-talker-quant-bit.patch` adds it. **Not yet measured**
— it needs the original 11 GB checkpoint — and upstream's default is a reason to
expect the answer may be no.

#### Vulkan for the vocoder: two real bugs found, and it still does not work

The idea was narrow and the reasoning held up. `config.json` has two runtimes —
the main one carries the thinker and the Talker, the `mllm` block carries the
audio encoder, the DiT and BigVGAN — and MNN disables its async token2wav worker
only when the MAIN backend is a GPU. So `mllm.backend_type: vulkan` alone should
put the two halves on different processors and make the overlap real. It did:
**Talker decode dropped from 3.57 s to 1.91 s**, which is the CPU no longer
sharing four cores with the vocoder.

Everything else went wrong, in three layers, and the first two are worth
recording because they are not about this GPU.

1. **A static libMNN does not register its Vulkan backend at all.** Every part of
   it registers from a file-scope initializer — the runtime creator in one
   object, one `addCreator` per operator in its own — and an initializer only
   runs if the linker pulls that object out of the archive. Nothing else in MNN
   references them. Symptom: `Can't Find type=7 backend, use 0 instead`, then a
   silent CPU run. Referencing one symbol fixes the runtime creator
   (`mnn-vulkan-static-register.patch`); the operators need the archive linked
   whole, which is one `/WHOLEARCHIVE` and, with `/OPT:REF` still discarding what
   is unreachable, cost 0.4 MB of binary.
2. **Two device limits were being ignored**
   (`mnn-vulkan-device-limits.patch`). A tensor's offset in its pool goes
   straight to `vkUpdateDescriptorSets`, so it must be a multiple of
   `minStorageBufferOffsetAlignment`; the pools were built with
   `nonCoherentAtomSize`, which is the flush granularity of mapped memory. Most
   desktop drivers report 64 for both and nothing shows; this Intel Arc reports
   1 and 64. Separately, `checkFP16` enables `storageBuffer16BitAccess` but not
   `uniformAndStorageBuffer16BitAccess`, while the shaders put 16-bit members in
   uniform blocks too. Both are undefined behaviour and both surfaced only as
   `VK_ERROR_DEVICE_LOST` — 83 of them per exchange, with no other diagnostic.
   The validation layer named both in one run.
3. **With those fixed the device survives and the output is still noise.**
   Identically at fp16 and fp32 — peak clipped to 1.000 against 0.573 on CPU,
   zero-crossing rate 0.257 against 0.140, 96.8% of frames below 5% of peak — so
   it is not precision, it is at least one wrong operator in the buffer path.

**Stopped there**, and the reason is the table above rather than fatigue: the
whole prize is token2wav's 3.07 s, and the Talker's 3.57 s — untouched by any of
this — is the larger half.

It left one thing worth keeping: `MNN_LLM_BUILD_DEMO=ON` builds `llm_demo`,
which runs an omni exchange from a config path and a prompt file and writes
`output.wav`. Every measurement in this section after the first was taken with
it, without starting the desktop client.

#### Above RTF 1, a player must not stream at all

The streaming arithmetic above says the gaps cannot be closed. The desktop
client now acts on it rather than restating it: the player measures what the
producer is actually delivering — seconds of audio per second of clock, with the
first block excluded because it arrives at no cost in clock — and **only streams
when that is above 1.1**. Below it, everything is held and played in one piece
when generation ends.

Speech output measures 1.8 and streams as before. Omni measures 0.28–0.33 and is
held: later, but whole. Streaming it produced 15 underruns of 1.8–4.5 s in one
reply, which is the 1 ÷ RTF duty cycle heard rather than calculated. The
threshold is per exchange, so a lane that gets faster starts streaming on its
own.

**The words do not have to wait for the voice.** The thinker finishes at 1.2–1.6
s and the exchange ends at 6.5 s — on a longer reply, 3.8 s against 70.3 s — and
until this session the reply text appeared only when the whole call returned, so
the page showed the answer after the speech reading it had finished playing.
`yoai_omni_ask` now takes a text handler and fires it the moment the
thinker stops.

**What was NOT established**, so it is not quietly assumed later: whether MNN's
quantization transcribes as well as the GGUF build does. The reference clip is
the one that crashes. The finding that Qwen2.5-Omni-3B beats SenseVoice on four
errors including a polarity flip stands — but it was measured through
llama.cpp, and it does not transfer to another runtime for free.

## A text-driven TTS on the engine already here

`ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF` is the first candidate that is better
than omni on every axis except the one that decides.

**llama.cpp supports it first-party.** `tools/mtmd/models/qwen3tts-gen.cpp` and
`qwen3tts-spkenc.cpp`, driven by `tools/tts/tts.cpp`, against `libmtmd` —
`mtmd_init_from_file` for the codec decoder, `mtmd_helper::gen_audio` to
generate. `mtmd` is its own `add_library` needing only ggml, llama, Threads and
four vendored header libraries, so it can be built without turning the whole
tools tree on — which is what `-DLLAMA_BUILD_TOOLS=OFF` in this repo's cross
build currently skips.

| | omni, `dit_steps` 3 | Qwen3-TTS-12Hz |
| --- | --- | --- |
| Size | 3.8 GB | **1.47 GB** (Q4_K_M + Q8_0 mmproj) |
| Engine | a second one, MNN | **the one already built** |
| Driven by | thinker hidden states | **text**, and a reference wav for the voice |
| Measured RTF | 3.0 on the phone | desktop: **1.65** short / **1.41** long, all-CPU with a Q8_0 mmproj; **0.24** on a 3090 with a bf16 one |

**Quality: acceptable, judged by ear on both a short and a long Chinese
passage.** That is the bar every cheap TTS had failed — VITS-class models
(Piper, MeloTTS, what sherpa-onnx ships) sound like the broadcast voice, which
is the thing being replaced.

### The CPU figures stand, and `-ngl 0` is not a CPU run

A re-measurement claimed this section was three times pessimistic and that
Qwen3-TTS runs at RTF 0.5 on a desktop CPU. It does not. `-ngl 0` moves the
language model and nothing else: **`--mmproj-offload` is a separate flag and
defaults to enabled**, so the projector was on a 3090 the whole time.

**Measured**, same utterance, bf16 mmproj, eight threads, b11037:

| | RTF |
| --- | --- |
| `-ngl 0`, projector left on the GPU | 0.50 |
| `-ngl 0 --no-mmproj-offload`, genuinely all CPU | **1.82** |
| `-ngl 99` | 0.24 |

1.82 against the 1.65 below, with the mmproj this section measures as ~30%
slower. The original numbers were right.

**The useful part is what the middle row costs**, because it is the section
above arriving from another direction: moving *only the projector* to the GPU
takes 1.82 to 0.50, a factor of 3.6, and moving the backbone as well buys
another 2.1. The projector is worth more than the model it serves — which is
what "the mmproj runs inside the autoregressive loop" predicts, and is now
measured rather than inferred.

**The error is worth more than the numbers, and it is this file's usual one.**
"I passed `-ngl 0`, therefore this is the CPU" is an assumption wearing a
measurement's clothes. It was checkable in one run — the one that produced the
1.82 — and the check was skipped because the result agreed with what was hoped
for. A number three times better than the file's own is the moment to look for
the mistake, not to write the correction.

### Its cost is a constant plus a rate, and the mmproj is in the inner loop

**Measured** on the desktop, eight threads, Q8_0 mmproj, all on the CPU:

| | audio | generation | final stage | total | RTF |
| --- | --- | --- | --- | --- | --- |
| short | 3.20 s | 2.65 s | 2.52 s | 5.29 s | **1.65** |
| long | 20.24 s | 25.26 s | 2.81 s | 28.45 s | **1.41** |

**The final stage takes the same 2.5–2.8 s either way.** It is a per-utterance
constant, not a rate — which inverts the lesson taken from omni one section up,
where the decoder half was the majority. That lesson does not transfer: it was a
property of a diffusion sampler run per chunk, not of codec decoders in general.

Generation goes the other way, at 105 ms per frame over 37 frames and 142 over
269 — attention's quadratic term. The long passage is still cheaper per second
of audio, because the constant amortises faster than the rate degrades.

**The mmproj quantisation is a real lever, and not where it looks.** Q8_0 (426
MB) against bf16 (638 MB), two runs each:

| mmproj | generation | final stage | RTF |
| --- | --- | --- | --- |
| Q8_0 | 2.65 / 2.92 s | 2.52 / 2.56 s | **1.65 / 1.63** |
| bf16 | 4.06 / 4.03 s | 2.62 / 2.60 s | 2.13 / 2.17 |

The final stage moves 3%; **generation moves 35%**. So the mmproj is not "the
vocoder" — calling it that was the mistake. `tools/tts/tts.cpp` samples a
semantic code from the backbone and feeds the hidden state to
`mtmd_helper::gen_audio` **every frame**, so the mmproj runs inside the
autoregressive loop and its size is paid per frame. Q8_0 is 33% smaller and 24%
faster, for 1.47 GB total.

**Which end this lands on is decided by the product, and the chain's replies are
short by design.** The measured chain answers in 12 to 18 characters, about
three seconds of speech — the end where 2.65 s of fixed cost is most of the
bill.

### What llama.cpp's clone path loses, and what it does not

**`-Base` is the cloning checkpoint, so running it with no reference is using it
wrong.** The GGUF carries no chat template and no speaker list, and free-running
it draws a speaker and a recording style per utterance — the same seed gives a
male voice on one text and a female one on another. That was recorded here as
"the model cannot hold a voice". It was a statement about a model being driven
without the input it exists to take, and it is withdrawn.

What survives from those runs is about sampling, not identity: `top_k` 1
collapses outright (160 s of audio for a 20 s passage, running to the frame cap
— the same greedy collapse MNN's talker config warns about), 5 is too tight, 20
beats 5, 40 with a lucky seed beat both. And the good draws had a character
worth naming — *very clear, close-miked, slight breath, strong prosody* — which
is a recording style the model imagines, not a setting.

**Given a reference, identity holds and the prosody flattens.** Sentence pauses
come out identical and mechanical, across three samples, two mmproj
quantisations and two sampling configs, including cloning a take that had been
judged good.

**That one is not a misconfiguration either, and the reason is in the graph.**
`qwen3tts-spkenc.cpp` ends in attentive statistics pooling, `[1536, T] → [3072,
1]`: the time axis is pooled away and the reference becomes a fixed-length
vector. Prosody is the time-varying part, so there is nothing left of it to
transfer. `mtmd_helper_gen_audio_inp` also has no field for a reference
transcript, so the in-context path the same weights support elsewhere cannot be
expressed through this API at all.

**"Holds identity" has a number now, and one sample that does not fit.** Pitch
of the generated takes, all the same sentence, measured with this repo's own
`yoai-prosody-test` rather than by ear — which is the point, since the
tool that judges the app's recordings can judge its output too:

| | baseline F0 |
| --- | --- |
| the reference wav | 245.7 Hz |
| free-running, two draws | 141.1, 198.4 Hz |
| cloned, five draws | 223.0, 233.9, 235.1, 235.6, **278.1** Hz |

Free-running moves 57 Hz between draws, which is the gender change the bullets
above describe. Cloned sits inside 12 Hz across four of five and each take's own
spread collapses from 16.6 to about 1.0 — flat pitch, which is the flattened
prosody stated another way, arrived at from a different direction.

**The 278 is unexplained and left standing.** Same text, same speaker file, 43 Hz
above its neighbours. Either the clone is not as pinned as three samples
suggested, or pitch moved without identity moving. F0 cannot tell those apart
and an ear can; the takes are in `D:\Models\wav` for whoever does that.

So llama.cpp gives a stable voice with flattened prosody, and that is the
ceiling of its interface rather than of the model.

### The variant that solves it is on a third runtime

The family has more than the Base checkpoint, and llama.cpp supports only Base.

| variant | identity | prosody | RTF, long | runtime |
| --- | --- | --- | --- | --- |
| Base, free-running | **changes, including gender** | good | 1.41 | llama.cpp |
| Base, cloned through a speaker encoder | stable | **flattened** | 1.41 | llama.cpp |
| Base, cloned in context | **stable** | **good** | 1.97 | qwentts.cpp |
| **VoiceDesign**, text attributes | **stable** | **good** | **1.16** | qwentts.cpp |
| CustomVoice, 9 named speakers + optional style | stable by construction | **worse than VoiceDesign** | 1.89 at 0.6B on the phone | qwentts.cpp |

**The same checkpoint is in the top three rows.** What separates them is the
interface: `mtmd_helper_gen_audio_inp` carries `prompt`, `speaker_ref`, `lang`,
`top_k`, `top_p`, `seed`, `out_type` and nothing else — no speaker id, no
reference transcript — while `qwen-tts` takes `--ref-wav` **and** `--ref-text`
and admits the reference as 280 frames of context. Cloning did not stop
flattening prosody because the model changed; it stopped because the reference
stopped being pooled.

**Both qwentts.cpp modes were built and measured, and both clear every bar
llama.cpp's interface could not.** It is a C++17 ggml port with a C ABI and a
termux build script — much narrower than MNN, which was a whole inference
framework.

- **Identity holds across texts.** The same instruction, two different passages,
  judged the same voice. This is the requirement Base failed.
- **The instruction is a real control surface, not a preset list.** Written
  `gentle, speaks slowly`, the result was audibly listless — the words landed.
  Rewritten `breathy, unhurried but not slow`, it was the one chosen. Pace,
  energy, mic distance and breath are all writable, and Base had no control over
  any of them.
- **Prosody survives**, because nothing goes through a speaker encoder.

**Measured**, same long passage as the table above, and it reports its own
split:

| | |
| --- | --- |
| Prefill | 0.79 s |
| Talker decode, 275 frames | 8.81 s (32.0 ms/frame) |
| Code predictor | 7.65 s (27.8 ms/frame) |
| Codec decode | 8.25 s |
| **Total, 22.00 s of audio** | 25.52 s — **RTF 1.16** |

**That figure does not reproduce, and what it teaches is about the machine.**
Re-run later on the same laptop, same passage, same Q8 checkpoint, three times:
**1.404, 1.399, 1.401**. Tight, and 21% slower than recorded. All three stages
moved together — talker 32.0 → 37.8 ms a frame, code predictor 27.8 → 32.5,
codec 30.0 → 39.2 — which is a clock difference rather than a configuration
one. The host here is a **Core Ultra 9 185H**, a laptop part, and it throttles
like the phone does; a single run of it is a reading of that day's power state,
not a constant.

**Q4_K_M on the same host: 1.250 cold, then 1.137 and 1.131.** So the
quantization is worth 19% here against 22% on the phone, and per frame the two
machines are in uniform proportion — talker 26.7 against the phone's 49.2, code
predictor 22.8 against 43.5, codec 38.6 against 73.5, **each within 2% of a
factor of 1.9**. Three stages agreeing that closely is the strongest evidence
yet that nothing about the phone's arithmetic is unusual; it is the same work at
half the clock, plus a throttle the laptop only hints at.

Numbers here are Q4 unless said otherwise from this point.

The fastest of them all, for 2.33 GB and a third runtime. **No stage is the
`dit_steps` of this engine**: the three cost within 15% of each other, so there
is nothing to switch off. The 0.6B talker exists only as base and customvoice —
VoiceDesign is 1.7B or nothing.

An earlier version of this paragraph went on to say there was no cheap lever
left at all. There was one: quantizing the talker to Q4_K_M took 22% off it on
the phone, and it was not tried here because these runs were all at Q8. The
lever it is NOT is one that touches the codec, which is what the phone section
below turns out to need.

**In-context cloning also works, and costs 70% more.** Same runtime, the Base
checkpoint, `--ref-wav` plus `--ref-text`:

| | short, 11.0 s | long, 19.5 s |
| --- | --- | --- |
| Prefill | 2.54 s | 2.35 s |
| Codec decode | 9.32 s | 17.48 s |
| AR per frame | 69.6 ms | 68.0 ms |
| **RTF** | **2.16** | **1.97** |

Prefill is the fixed part and amortises; the codec decode does not — it tracks
the audio. The reference sits in context for the whole generation, so every
frame is dearer than VoiceDesign's 59.9 ms as well.

**So the choice inside qwentts.cpp is how a voice gets specified**, not whether
it holds. VoiceDesign describes one in English and runs at 1.16; cloning names
an exact recording and runs at 1.97. The first is the one to port, because a
phone will be slower and 1.16 has room where 1.97 does not — and because a
described voice needs no reference file shipped or recorded. Cloning is what a
later "use my own voice" feature would be built on, and it should wait until the
ratio is known on the device.

**All of it is still above 1, and the phone will be worse.** So this belongs to
prepared playback, not to conversation. Conversation already has its answer, and
that answer is 1.37 s of silence with the platform voice.

### The answer: 1.7B VoiceDesign at Q4_K_M, for prepared playback only

Four checkpoints were run on the device and the first one tried is the one that
stays. It is not a default that went unexamined — each of the others lost on
something specific:

| | why not |
| --- | --- |
| 1.7B VoiceDesign Q8_0 | same voice, 22% slower per frame, 0.8 GB larger |
| 0.6B CustomVoice | 14% faster a frame, and **audibly worse** — as is 0.6B base, so it is the size that loses on voice and not one checkpoint |
| 0.6B base, in-context clone | ~24 s of setup per utterance, first audio at 29.8 s, and it **mispronounces** |
| 1.7B base, in-context clone | reads correctly, and costs **RTF 11.76** with first audio at 43.8 s |
| 1.7B CustomVoice | untried; CustomVoice already lost on voice at 0.6B |
| a shorter reference for cloning | untried here; on CosyVoice3 it took 74% off the fixed charge but only 22% off the rate, and the rate is what a long passage pays |

**What it costs is the one thing nothing fixed.** RTF 2.14 for the first half
minute and 3.52 after. Six levers were tried on that number — streaming against
buffered, the thread count, the talker's quantization, the codec's, the talker's
size, and the utterance's length — and two moved it: quantizing the talker, by
22% a frame, and shrinking it, by another 14% at a cost in voice that was not
worth paying. So the lane is for **prepared playback**: generate while nobody is
waiting, play later. Conversation keeps the platform voice and its 1.37 s of
silence.

**One lever is left and it is not a small one.** The gap between the two speeds
is 40%, larger than anything else found here, and the fast regime is bought with
idle time. Whether a passage split into sentences with pauses stays in it is
unmeasured, and it is the only remaining thing that could change the number
rather than the model.

**The GPU is not a lever, and the reason is a policy rather than a limit** —
see "The GPU runs, and the platform kills it" below.

### On the phone: RTF 2.1 for half a minute, then 3.5 — half a laptop's clock

qwentts.cpp cross-compiles for HarmonyOS and the lane is wired end to end —
[harmony/build.cmd](../harmony/build.cmd) `qtts` assembling
`harmony/prebuilt/qtts-sdk/<abi>/`, `src/voice/qtts.*` behind a
`yoai_tts_*` C ABI, NAPI bindings, and `/tts` to drive it.

**The device has two speeds and every subsection but the last measured only the
first.** It holds one rate for the first twenty to thirty seconds of continuous
generation and then steps down to about 60% of it. Everything short enough to
finish inside that window — which is every comparison below, because they were
all run on a 3.8-second utterance — is a sprint figure, and comparisons between
sprint figures are still valid. A single ratio quoted out of them is not what an
utterance costs. "Two speeds" at the end of this section is the one that ran
long enough to see both.

Sprint figures, then. Per frame, four threads, seed pinned so every run of a
configuration is the same utterance; a frame is 80 ms of audio at 12.5 Hz:

| stage | laptop, Q4 | phone, Q8 talker | phone, Q4 talker | laptop → phone |
| --- | --- | --- | --- | --- |
| Talker decode | 26.7 ms | 63.2 ms | **49.2 ms** | 1.84× |
| Code predictor | 22.8 ms | 51.6 ms | **43.5 ms** | 1.91× |
| **Codec decode** | 38.6 ms | **73.5 ms** | **73.5 ms** | **1.90×** |
| Prefill | 0.65 s | 0.80–0.96 s, fixed | 0.83–0.92 s | amortises |

**The laptop column wanders**, so read that last column as 1.9 to 2.1 rather
than as three digits: the same three stages measured 23.9, 20.0 and 34.9 on
another day against the 26.7, 22.8 and 38.6 here, which is the same ±15% every
CPU figure on that machine carries. What survives is that all three move
together.

An earlier version of this table compared the phone's Q4 against the laptop's
**Q8** and read the codec as an outlier at 2.45× where the other two were about
1.9. It was comparing two quantizations: the codec file is the same in both
columns, so it was the only row where the mismatch showed. Like for like, all
three are 1.9 and **there is no outlier at all** — the codec is simply the
largest of three stages that scale together.

**The codec is the floor, and it is the one number here that nothing has
moved.** Five separate attempts left it at 73.5 ms per frame: streaming against
buffered, a talker at half the weights, a talker a third the size, its own
quantization, and the GPU. 73.5 ms buys 80 ms of audio, so **the codec alone is
RTF 0.92** and everything else is on top of it. That is a statement about
its SIZE, not about it scaling oddly — against the laptop it is 1.90×, exactly
like the other two.

The two autoregressive stages do respond. Per frame they are now 45% of the
bill together, against the codec's 45% and prefill's 10% on a 3.8-second
utterance; the arithmetic closes — 49.2 + 43.5 + 73.5 = 166 ms per 80 ms frame
is RTF 2.08, plus prefill amortised over that length is 0.22, and 2.30 is what
the page shows. All of that is the fast regime.

**Nothing reaches 1 from here**, and after three levers it is worth saying why
rather than hoping. The codec is RTF 0.92 on its own, it is a separate model from
the talker, and it does not care what the talker is. So this lane stays where
the desktop section already put it: prepared playback, not conversation.

#### Streaming is free, and the reason it looked expensive was noise

The desktop decoded 275 frames in one codec pass; the phone streamed, in chunks
that ramp from one frame to eight, and the streaming path re-decodes the codec's
left context per chunk. That is a real mechanism, the codec was the outlier, and
two phone runs seemed to show it: 8 chunks gave 76.1 ms/frame against 13 chunks'
79.4, and a per-frame rate should not rise with length.

`yoai_tts_set_streaming` was added to settle it, with a pinned seed so both
modes generate the identical 41 frames. Seven runs, four threads, alternating,
over two minutes:

| stage | spread across all seven | streamed | buffered |
| --- | --- | --- | --- |
| Talker decode | 2532–2664 ms | — | — |
| Code predictor | 2081–2199 ms | — | — |
| Prefill | 803–964 ms | — | — |
| **Codec decode** | **3013–3997 ms** | 3013, 3013, 3440, 3280 | 3073, 3997, 3547 |

**Buffered is not faster.** Its best run is 3073 ms against streaming's 3013,
and its worst is the worst of the seven. The re-decode costs nothing measurable.

**What the table actually shows is that the codec stage wanders**: 3013 to 3997
on identical input, ±14%, where the two autoregressive stages hold ±2.5%. The
76.1-against-79.4 that motivated this was 4% — under a third of that, read off
two runs, as a trend.

**It wanders between periods, not within one.** Five back-to-back runs of a
fixed configuration later held the codec inside 3530–3598 ms, under ±1%, so the
±14% is drift across two minutes rather than scatter. That does not rescue the
76.1-against-79.4: those two runs were minutes apart, which is exactly the
interval the drift lives on. It does mean a comparison run back to back is
worth much more than one run of each, and it is why every later figure here
comes from three consecutive runs of a configuration.

So streaming is not why the codec looked like an outlier, and — measured later,
like for like against the laptop at the same quantization — **neither was
anything else: it is not an outlier.** The whole investigation below chased a
2.45× that was two quantizations being compared. Nor is
quantization, and nor is the talker — see the next section for the other two
attempts on it.

**Streaming stays on.** It moves the first audio from the end of the run to
about 1.0 s — 930 to 1109 ms across four runs, against 8.5 to 9.6 s buffered —
and now costs nothing. That does not make playback smooth: above RTF 1 the
listener drains each chunk faster than the next arrives, 43% speech and 57%
silence at 2.3, the same arithmetic that settled the question for omni. It makes
the first word early, which is worth having in a flow that generates ahead.

#### Q4 moves the talker by a fifth and the codec not at all

Two predictions, made before the files were downloaded, and one of each:

- **The talker at Q4_K_M would help**, because 1.9 GB of Q8 weights read 12.5
  times a second is most of what this chip's 24.4 GB/s can give.
- **The codec at Q4_K_M would not**, because it reads at most 0.27 GB per frame
  in 73.5 ms — under 4 GB/s, nowhere near the ceiling — so it is not waiting on
  memory. A convolutional decoder with 21 conv contexts and a 128-deep KV ring
  does a great deal of arithmetic per byte of weight.

Measured, four threads, seed pinned, three consecutive runs of each:

| | Talker | Code predictor | Codec | RTF |
| --- | --- | --- | --- | --- |
| Q8 talker, Q8 codec | 63.2 ms | 51.6 ms | 73.5 ms | 2.60 |
| **Q4 talker**, Q8 codec | **49.2** | **43.5** | 73.5 | **2.29** |
| Q4 talker, **Q4 codec** | 49.2 | 43.5 | 73.5 | 2.31 |

**The talker gained 22% and the code predictor 16%; the codec gained nothing**
(3587 ms against 3545 over 48 frames, a 1% difference inside two overlapping
ranges). The second prediction holds. So does the first, but only halfway:
Q4_K_M is 58% of Q8's bytes and bought 22%, not 42%, so that stage is part
bandwidth and part arithmetic too.

**The codec reading 73.5 ms/frame in all three rows is the useful part.** It is
the same number the streaming comparison produced, from a different talker, at a
different quantization, on a different day. Three independent attempts on one
stage and it has not moved once.

**Q4_K_M is what this lane runs at, and that choice is now in question.** The
pair goes from 2.2 GB to 1.4 and RTF from 2.60 to 2.29, and the voice was judged
to hold — but "Q4 sounds fine" was said about one text with no `Q8` beside it,
and when the two were later played back to back on the PC, **`Q8` was the more
natural of the two**.

The PC's answer flipped on that (see the deployment matrix), and the phone's does
not automatically follow. There, neither quantization is anywhere near 1, so the
22% is not buying latency either — it buys a generation that finishes sooner and
a phone that heats less, which on a lane whose whole use is generate-ahead is a
battery argument rather than an experience one. Against 0.8 GB more storage and
an audible difference in the one thing this engine was carried for.

**The measurement that would settle it has not been made**: `Q8` against `Q4` on
the PHONE, same text, back to back. Everything above compares them on speed
only.

#### The thread count is not a lever, and the phone getting warm is

Swept on the device at 2 / 4 / 6 / 8 and back to the engine's own choice, same
text each time, per-frame so the differing lengths do not matter:

| threads | Talker | Code predictor | Codec | AR frame | RTF |
| --- | --- | --- | --- | --- | --- |
| 2 | 72.5 ms | 60.6 ms | 126.1 ms | 133.2 ms | 3.66 |
| 4 | **62.1** | **51.8** | **73.2** | **114.0** | **2.56** |
| 6 | 64.4 | 54.9 | 78.9 | 119.4 | 2.66 |
| 8 | 65.2 | 56.1 | 73.3 | 121.3 | 2.63 |
| 6, again | 66.1 | 56.3 | 83.5 | 122.4 | 2.79 |
| 6, again | 67.6 | 57.2 | 88.2 | 124.8 | 2.87 |

**Four, six and eight are within about 2% of each other. Two is 12% worse.**

The last two rows are the reason the middle three cannot be separated. They are
the same count as row three, run five and ten minutes later, and they get
monotonically slower: 119.4 → 122.4 → 124.8 ms per frame, **about 9% across the
sweep**. Every number in this table carries however much of that its position
earned, and 2% is less than one run's worth. Four ran second and six ran third,
so four's lead is roughly the drift between them.

This was added expecting the opposite. The argument was clean — the engine picks
`hardware_concurrency() / 2` reasoning about hyperthreads, this phone has none,
and the llama lane on the same device measured prefill and decode flat from four
threads up — and every clause of it is true. None of them implied a penalty, and
there is not one. `QWEN_N_THREADS` stays because the next device is not this
one.

**The drift is the finding worth keeping.** A phone speaking for a few minutes
loses about a tenth of its speed, which is larger than anything the thread count
was going to buy, and it is not visible in a single measurement. Prefill also
scales with threads and then stops — 1305 ms at two, 814 at four, 855 at six,
798 at eight — the same shape the llama lane has.

That 9% turned out to be the shallow end of something much bigger. Each run here
was about ten seconds of compute with gaps between, so the sweep never left the
fast regime and only saw the chip warming inside it. Run one long passage
instead and the same mechanism takes 40% — see "Two speeds" below. Every
±2%-versus-drift argument in this subsection survives, because every point in it
was measured under the same conditions; what does not survive is reading any of
these RTFs as what the lane costs.

#### Two speeds, and the one that matters is the slow one

The 3.8-second utterance was measured to death and none of it says what a real
one costs. A 30-second passage — the same text the desktop's 22-second figure
came from, run at Q4, four threads, seed pinned — came out at **RTF 2.95 and
3.22**, against 2.29 short. **The ratio got worse with length**, and the
prefill-amortisation arithmetic that predicted 2.12 had the sign right on the
wrong term.

The engine logs a line every eight frames, so the run can be read as a rate
rather than a total. The first pass, frames 8 to 368:

| frames | seconds per 8 | ms/frame | RTF |
| --- | --- | --- | --- |
| 8 → 176 | 1.29–1.43, mean 1.37 | **171** | 2.14 |
| 176 → 184 | 2.05 | — | the step |
| 184 → 368 | 2.22–2.27, mean 2.26 | **282** | 3.52 |

**Flat, a cliff, flat.** Twenty-one blocks at 171 ms a frame, one transition
block, then twenty-three blocks at 282 — varying by under 2% on either side of
a 65% jump. That is a clock step, not an algorithm: attention over a growing KV
cache gets dearer gradually and never flattens, and the codec, which has no
growing state at all, slowed by the same proportion.

**It is heat, not a fixed boost window**, and the two runs discriminate. The
first began after fourteen idle minutes and held the fast rate for 29.9 s. The
second began 44 s after the first ended and held it for 16.4 s, then stepped
down in stages — 169, 228, 260, 282 — rather than in one. A timer would have
given both runs the same window; a warm chip gives the second one less.

So the honest pair of numbers for this lane, for each talker:

| | ms/frame | RTF | what it is |
| --- | --- | --- | --- |
| 1.7B fast | 171 | **2.14** | the first ~30 s from cold, and every other measurement in this section |
| 1.7B sustained | 282 | **3.52** | anything longer |
| 0.6B fast | 155 | **1.94** | |
| 0.6B sustained | 264 | **3.30** | |

**This is the wrong way round for what the lane is for.** Prepared playback
means long text, and long text is exactly what runs in the slow regime: a
30-second passage takes 88 to 97 seconds, most of it throttled.

**The window is wall clock, not work.** That was a prediction before the 0.6B
was run — the chip is pinned at four threads either way, so heat should track
time under load rather than frames produced — and the small model agrees with
it. Every long run to date, by when the clock first steps:

| talker | idle before | fast until |
| --- | --- | --- |
| 1.7B | 14 minutes | t+29.9 s |
| 0.6B | ~90 s of short runs | t+28.3 s |
| 1.7B | 44 s, after 88 s of full load | t+16.4 s |
| 0.6B | 40 s, after 59 s of full load | t+17.4 s |

**Model size does not move the step; thermal history does.** A talker 46%
cheaper per frame gets the same half-minute, and spends it on more audio. That
is the whole of what a smaller model buys on a long passage — the fast regime is
a fixed amount of TIME, so the only way to get more out of it is to produce more
audio per second, and 0.6B produces about a tenth more.

The ladder is not always one step. The 0.6B's second run went 157 → 201 → 264 ms
a frame, pausing at an intermediate clock for eleven seconds before dropping the
rest of the way; the 1.7B's first run went straight from 171 to 282.

**One untested mitigation, and it is cheap to try.** If the fast regime is bought
with idle time, a long passage split into sentences with pauses between them
might stay in it. It would pay prefill per sentence — 0.39 s at 0.6B, 0.87 at
1.7B, against one longer prefill — and lose whatever prosody crosses a sentence
boundary. Whether the pauses need to be seconds or minutes is not known, and
that is the measurement to make before designing anything around it.

#### The 0.6B was worth trying after all, and it buys about a tenth — but loses on voice

This section used to argue from the floor that the small talker should not be
run: the codec is RTF 0.92 by itself, so even a free talker leaves more than 1.
The arithmetic was right and the conclusion was too strong — it answered
"can this reach real time" when the question worth asking was "how much does it
move", and those have different answers. It was run. Measured, `0.6b-customvoice`
at Q4 against `1.7b-voicedesign` at Q4, four threads, seed pinned:

| stage | 1.7B | 0.6B | |
| --- | --- | --- | --- |
| Talker decode | 49.2 ms | **26.8 ms** | −46% |
| Code predictor | 43.5 ms | 42.5 ms | **−2%** |
| Codec decode | 73.5 ms | 73.7 ms | **unchanged** |
| Prefill, short prompt | 870 ms | 386 ms | −56% |
| per frame | 166 ms | **143 ms** | −14% |
| RTF, short utterance | 2.31 | **1.89** | |

**The talker is the only stage that shrank.** The code predictor did not, which
is the surprise: it is the MTP head, sized by the sixteen codebooks rather than
by the LM's depth, so a third of the parameters buys nothing there. And the
codec is a separate model — the fourth attempt on it, and it has still never
moved.

So **the floor is higher than the codec alone**. Two of the three stages are
indifferent to talker size, and together they are 116 ms a frame: **RTF 1.45
with a talker costing nothing**, and about 2.47 once the clock steps down.

In the long-passage runs the difference is smaller than the short-utterance
table suggests — 155 ms a frame against the 1.7B's 171 in the fast regime, 264
against 282 in the slow one. **About a tenth in each**, because the stage that
shrank is the smaller share of the bill.

**What it does not cost is the written voice.** CustomVoice takes a speaker AND
an optional style instruction, so identity comes from the table and delivery is
still written. Removing the instruction entirely from one run changed prefill by
226 ms and the per-frame rate not at all — **the description is nearly free
here**, which is the opposite of the trade this file assumed when it wrote
CustomVoice off.

**But it sounds worse than VoiceDesign**, judged by ear on the same passage.
That settles the trade against it: a tenth of a frame and half the disk are not
worth a voice that is audibly second best, when the whole reason this engine was
chosen over three others was how it sounds. The speaker table is read from the
file rather than taken from the model card — `qt_n_speakers` and
`qt_speaker_name`, logged at load — so the nine are real; they are just not as
good as a described one.

So the lane stays on **1.7B VoiceDesign at Q4_K_M**, and the 0.6B's value turned
out to be a measurement rather than a candidate: it is what showed that two of
the three stages do not care about talker size, which is the floor argument that
makes the whole family's ceiling structural.

#### In-context cloning costs a fixed half-minute, whatever it says

The one configuration of this family that has never run on the phone, and the
only one left that could sound BETTER than VoiceDesign rather than merely
cheaper: on the desktop it was the other row scoring stable identity and good
prosody, and the recording it would clone is the one judged best of everything
generated here.

`yoai_tts_set_reference` now takes a WAV and its transcript, and
`src/voice/tts/qwen3_tts.cpp` carries a RIFF reader strict enough to refuse anything that
is not mono 16-bit at exactly 24 kHz — the same no-resampler rule the
recognition lane states at 16 kHz, for the same reason. The page shows the
reference controls only for a `base` checkpoint and nothing else for it, because
that checkpoint refuses a description and a named speaker both.

**Measured: RTF 6.70, and almost all of it is a fixed charge.** 0.6B base at
Q4, cloning the 22.4-second `long_zh.wav`, six seconds of output:

| | | |
| --- | --- | --- |
| Prefill | 3.53 s | the reference's text and audio |
| Codec, 280 reference frames | ~20.6 s | encoding the recording |
| Talker decode | 39.3 ms/frame | 26.8 without a reference — **+47%**, the attention over 280 extra frames |
| Code predictor | 44.4 ms/frame | unchanged |
| Codec decode | ~70 ms/frame | unchanged |
| First audio | **29.8 s** | nothing can be emitted until the reference is encoded |

The engine bills the reference encode inside `CodecDecode`, which is why that
line reads 26.1 s for 79 frames; subtract the 280 reference frames at the rate
every other measurement here found and the remainder is 69.5 ms a frame, the
usual number again. **The codec has now survived a fifth attempt.**

So in-context cloning is **about 24 seconds of setup plus roughly the
per-frame cost of a described voice** — 154 ms against VoiceDesign's 143 at the
same size, the difference being the talker's larger context. On six seconds of
output that setup is the whole bill. On a long passage it amortises, and the
70% the desktop measured was a figure for one passage length rather than a
property.

**The lever is the reference, not the model.** Both halves of the fixed cost
scale with how long the recording is, and nobody has tried a short one at either
end here. A five-second reference would cost a fifth of the setup and a fifth of
the added attention per frame — whether it still carries the voice is the
question.

**Half of that prediction has since been tested, on CosyVoice3, and half of it
was wrong.** Cutting a reference from 17.3 s to 6 s — 65% — took 74% off the
fixed charge, which is the proportional scaling this paragraph assumed. It took
only **22%** off the per-second rate, where proportional would have been 65%. So
the setup half is a length and the attention half is not, or not only: a shorter
reference buys back the wait before the first sample much more than it buys back
the rate afterwards. On a long passage the rate is what dominates. That is a
different engine and does not transfer, but it says which of the two halves to
measure first, and which to stop expecting much from.

**At 1.7B and Q8 the same clone costs RTF 11.76 and 43.8 s to first audio** —
prefill alone is 14.7 s against the 0.6B's 3.5, because the reference's 280
frames go through a talker three times the size. Both cloning runs are far
outside anything usable, and the second one confirms the first was not an
artefact of the small model.

#### A polyphone, and the one cell of a 2×2 that fails

Given `先把灯调暗一点吧`, `调暗` needs `tiáo`. Six configurations read it, and
one says `diào`:

| checkpoint | Q4_K_M | Q8_0 |
| --- | --- | --- |
| 0.6B base | **wrong** (twice) | correct |
| 1.7B base | correct | correct |
| 0.6B CustomVoice | correct | — |
| 1.7B VoiceDesign | correct | — |

**Neither variable alone explains it.** Q4 does not, since three Q4 rows are
fine. The 0.6B does not, since the same checkpoint at Q8 is fine. The base kind
does not, and neither does the cloning context — both base rows carry the same
22.4-second reference. It takes **small AND Q4 together**, on the base
checkpoint, and the reading is the ordinary one: a 0.6B has less redundancy to
spend, so a quantization the 1.7B absorbs costs it a capability.

An earlier version of this section called completing that square **"a test with
no payoff"**, because the failing configuration was already out on cost. That
was wrong about where the payoff was. The square does not exonerate the
0.6B — it clears **this lane's own quantization**, which is Q4, on a model
large enough to carry it, and `1.7B base Q4_K_M` reading correctly is the direct
evidence for that. A negative result about a discarded configuration was worth
nothing; the same experiment's evidence about the shipping one was worth having.
**Asking what a test rules OUT is not the same as asking what it rules in.**

Still one sentence, so it bounds nothing about Chinese generally. What it does
establish is that **grapheme-to-phoneme is per-checkpoint and quantization can
take it away**, so it does not carry across a model swap or a requantization and
has to be re-checked after either. **And it cannot be corrected from here**:
`qt_tts_params` carries text, a language name and sampling knobs, and nothing in
the runtime accepts pinyin or phonemes. For a lane that speaks whatever an LLM
wrote, rewording is not a remedy either.

#### CustomVoice was never a speed question, and it lost on the other one

The paragraph above used to fold the nine named speakers into the case against
the 0.6B, as though CustomVoice were something only the small model offered.
**It is not** — `qwen-talker-1.7b-customvoice` is published at every
quantization the VoiceDesign checkpoint is, and it has never been run here.

Folding the two together hid that they are different questions. At 1.7B the
speed is already known: same parameter count, same codec, so the same RTF 2.29,
give or take a shorter prompt. **Nothing about CustomVoice is a speed
proposal.** What is open is the voice, and there the interface is not the one
this file assumed:

> `instruct` is the style instruction string; required for voice_design,
> optional for custom_voice, rejected for base. `speaker` is the named speaker
> for custom_voice models, rejected for the other two modes.

So CustomVoice is **a speaker table plus an optional style instruction**, not a
preset list instead of one. Identity comes from an enumerated speaker rather
than from a description holding steady across passages, and the writable part of
VoiceDesign — pace, energy, mic distance, breath — appears to survive. That is
a strictly stronger guarantee on the axis Base failed, with the same control on
top, and it was passed over on a misreading rather than a measurement.

**Since written, the speaker parameter exists and the 0.6B CustomVoice has been
run**; its cost is in "The 0.6B was worth trying after all" below, and the
reading above held — the style instruction does apply, and costs 226 ms of
prefill. How the nine speakers sound is still open.

**And the 1.7B CustomVoice is what the desktop client ships.** Measured through
it, 1.7B CustomVoice `Q4_K_M` with the 12 Hz tokenizer at `Q8_0`: **RTF 0.56,
first audio at the speaker 0.13 s**, a reply arriving in 20–29 chunks. It is the
lane the playback rule above calls fast enough to stream, and by a wide margin.

Three things the client had to learn about the parameters, each of which looked
like a bug first:

- **`seed` -1 draws afresh every call**, and the same sentence then varies by
  55% in spoken duration. Heard as "the pace changes between replies". The
  client pins it.
- **The instruct participates in generation on custom_voice**, 20 tokens of it,
  rather than being ignored in favour of the speaker id. Writing `slow` into it
  is worth 44%.
- **Speech rate is a property of the text, not a setting**: 5.92 against 3.29
  characters a second on two passages with everything else identical. So a
  written pace is a nudge, not a multiplier, and a reply that reads slowly was
  never the configuration's doing.

#### The GPU runs, and the platform kills it

The phone has a Maleoon 920 and the codec stage is compute-bound — it reads at
most 0.27 GB a frame in 73.5 ms, under 4 GB/s against a chip measured at 24.4 —
which is the profile a GPU helps. So ggml's Vulkan backend was cross-compiled
for OHOS and linked in.

**It builds and it runs.** `harmony/build.cmd <abi> vulkan` assembles it, the
driver initialises (`<HM_GPU> # Device init`), ggml selects it, and the engine
reports `[Load] Talker backend: Vulkan0`. Four build obstacles, all in the
toolchain and none in the model:

| | |
| --- | --- |
| `SPIRV-Headers` not found | the OHOS toolchain re-roots package searches into the sysroot, so ggml's own `list(APPEND CMAKE_PREFIX_PATH $ENV{VULKAN_SDK})` is ignored |
| "unable to find a build program corresponding to Ninja" | the nested host build — 151 shaders compiled by a `vulkan-shaders-gen` built for the host — inherits the generator but not `CMAKE_MAKE_PROGRAM` |
| `vulkan/vulkan.hpp` not found | the sysroot ships the C headers only; the C++ bindings come from the host SDK, and compiling against those while linking the phone's loader is fine |
| `libggml-vulkan.a` not linked | the app finds ggml archives by name, and it has to go ahead of them because it references the rest |

**And then HarmonyOS kills the process**, with `GPU_LEAK_KILL`, seconds after the
first compute submission and before any audio. It is a platform policy about one
number, not a crash and not ordinary OOM — the app is billed for GPU buffers and
there is a ceiling.

The number did not move:

| setting | GPU buffers at the kill |
| --- | --- |
| defaults | 8.57 GB |
| `GGML_VK_PREFER_HOST_MEMORY=1` | 8.03 GB |
| plus `GGML_VK_SUBALLOCATION_BLOCK_SIZE=64 MB` | 8.31 GB |

For a model of **1.1 GB**. The second setting matches the hardware — a phone has
no separate video memory to prefer — and the third matched the number, because
ggml batches allocations into 1 GB blocks by default and 8 GB is the shape of a
few buffers each rounded up to one. **That hypothesis was written down with its
own refutation attached**: if the total stayed near 8 GB, the memory was being
used rather than reserved. It stayed at 8.31.

So three allocation strategies produce the same total, which is what "no
allocation knob will help" looks like. qwentts.cpp has no `n_gpu_layers`, so
there is no partial offload to fall back to either.

**What stays**: the build path, because it is one command and the next driver or
the next ggml may behave differently, and because the finding is about a policy
rather than about arithmetic. **What does not**: the SDK ships CPU-only, which
is what `harmony/build.cmd <abi> qtts` assembles.

**What was not reached**: whether the GPU would be faster at all. The codec is a
convolutional decoder with 21 conv contexts and it is 45% of the bill; if
ggml-vulkan does not cover those ops they fall back to the CPU and the question
answers itself. That is now unmeasurable here, and it was always the second
question.

#### Two things the port settled, and neither stayed settled

**Two ggml, and only one of them can be in the library.** The llama SDK and the
qwentts SDK each ship `libggml{,-cpu,-base}.a`, built from two trees that move
independently. Linking both is the rapidjson mistake in a bigger form: the linker
resolves each symbol from whichever archive it scans first, so one build's
compiled code would run against the other's implementation. Three measurements
decided it rather than a preference:

| at llama `c390d0a`, qwentts ggml `0572d60` | needs | the other's ggml is missing |
| --- | --- | --- |
| `libqwen-core.a` | 134 ggml/gguf symbols | 1 — `ggml_prec_set_acc` |
| `libllama.a` | 287 | **0** |

The public headers differed in two files and not one struct: `ggml.h` gained a
prec enum and its two setters, `ggml-backend.h` lost a function neither library
calls. So the newer superset the older, and the qwentts copy was what went in.
It is built from the same `-march` flags by the same script, so nothing was
given up for it. **This fails loudly if it stops being true** — C symbols are
strong, so a real collision is a duplicate-definition error at link, not a
silent wrong-layout read like rapidjson's.

**It stopped being true, and it did fail loudly.** Both checkouts were moved to
their tips on 2026-09-21 — llama to `335b21f`, the qwentts ggml fork to
`e2568d7` — and the desktop addon stopped linking with one undefined symbol.
Re-measured on the new pair, from the built archives:

| at llama `335b21f`, qwentts ggml `e2568d7` | needs | the other's ggml is missing |
| --- | --- | --- |
| `qwen-core.lib` | 134 ggml/gguf symbols | **0** |
| `llama.lib` | 288 | 1 — `ggml_dsv4_hc_pre_gated` |

**The direction inverted.** Whichever copy is kept, the same count is unmet;
only the side that goes short has swapped. So "keep the newer one" was never a
rule — it was one observation of a race between two trees, and which one was
ahead changed the first time both were updated.

**And the fork is not simply the older ggml.** It carries two symbols llama's
does not, and one of them is functional: `ggml_compute_forward_snake_fused`, a
graph-level pattern match in `ggml-cpu.c` that collapses `mul → sin → sqr →
mul → add` into one kernel. That chain is the DAC decoder's snake activation,
which is to say **the codec — 45% of the synthesis bill**. Dropping it does not
break anything: the match fails and the five ops run unfused, same output.
(The other, `ggml_backend_vk_reg`, is not a fork difference — it is there
because only the qtts SDK is configured with `GGML_VULKAN=ON`.)

**The decoder is built around that kernel, which is the part worth knowing
before calling the loss small.** `dac-decoder-v2.h` says so in its header: the
whole pipeline is laid out T-first *because* "the fused SNAKE op requires
ne[0]=T and ne[1]=C", `alpha`/`beta` are folded into `a` and `inv_b` at load
time so "the runtime kernel stays a single fused op", and the `[1, C]` weight
layout matches its broadcast convention. The shape, type and contiguity tests
in the dispatcher are exactly what that header undertakes to satisfy — so the
match is designed to fire, not hoped to.

It fires **29 times per codec forward**: four blocks of one SnakeBeta plus
three residual units of two, plus `snake_post`. Unfused that is 145 ops instead
of 29, and the count is the smaller half — each activation goes from one pass
over the tensor to five, with four extra full-size intermediates written and
read back, in the stage this file has already established is bandwidth-bound
and is the floor. **Still unmeasured, and no longer plausibly negligible.**

**Where it can matter is the CPU only.** The fusion lives in `ggml-cpu.c`, so
the Vulkan path runs the five ops as five kernels either way: the GPU server
configuration is untouched by this. What it touches is the phone, which has no
usable GPU here, and the PC's CPU lane.

**So the way out is one ggml, not a better choice between two.** qwentts.cpp
added `GGML_SOURCE_DIR` on 2026-09-21, which points its build at any ggml tree,
llama.cpp's included. The measurement above says that direction works — 134 of
134, nothing missing — which makes it the fix rather than a tidy-up.

**Done on the desktop lane, and it links.** `desktop/build.cmd qtts` now
compiles llama.cpp's ggml inside the qwentts build, with `GGML_VULKAN=ON`, and
the resulting archive set carries `ggml_dsv4_hc_pre_gated` for libllama and
`ggml_backend_vk_reg` for the GPU path, satisfying both libraries from one
commit. Linking llama's own SDK copy instead would not have worked here: only
the qtts configure asks for Vulkan, so llama's SDK has no `ggml-vulkan.lib`,
and the GPU is the entire reason this lane exists.

**What it cost is confirmed absent**: `ggml_compute_forward_snake_fused` is not
in the new archives. **Its worth has not been measured**, and that is the number
that decides whether the fork's kernel comes back as a third patch or the loss
is noise — five unfused ops in the stage that is 45% of the bill and was
already the floor.

**The phone lane had the same breakage, and now has the same fix.**
`harmony/build.cmd qtts` passes `GGML_SOURCE_DIR` too, cross-compiles clean,
and the HAP builds and signs — so libllama.a and libqwen-core.a link against
one ggml on the device as well. `android/build.sh` is changed to match. Two
things came with it: llama.cpp's affinity patch is now *applied by the script*
rather than by hand, which is how it came to be recorded at all, and `qtts`
needs a llama.cpp checkout whether or not `llama` is being built.

**Two lines for the whole HarmonyOS build.** `ggml-cpu.c` guards its
thread-affinity code with `__gnu_linux__` in five places and `__linux__` in one.
OHOS is Linux on musl: it defines the second and not the first, so it took that
one branch alone, into two errors. `cpu_set_t` and `CPU_ZERO` are undeclared —
musl has them behind `_GNU_SOURCE`, which the toolchain's `-D_XOPEN_SOURCE=600`
hides. Then `pthread_setaffinity_np`, which musl does not have under any guard;
`libc.so` exports only `sched_setaffinity`. Both are fixed in the file: define
the macro, and widen the `__ANDROID__` branch that already calls
`sched_setaffinity` for exactly this gap. The patch lives in
[third/patches/](../third/patches/) because the checkout is upstream's.

**The first fix was wrong, and its write-up was wrong about why.** Narrowing
`__linux__` to `__gnu_linux__` builds — it drops OHOS into the file's own
"unsupported platforms" stub, where affinity is a no-op — and the note here
called `-D_GNU_SOURCE` a symptom-level fix, on the grounds that the next error
after it was the missing function. That reasoning reads a two-part problem as
one part plus noise. `_GNU_SOURCE` is half the real fix; the second error
needed its own half, not a different diagnosis. The tell was available and
unread: **llama.cpp's checkout already carried this patch**, in this shape, by
hand, applied for the same sysroot on the same machine — found only when `third/`
went to record what those checkouts contain. What affinity is worth on this
device is still unmeasured, so the argument for the new shape is that it does
not silently discard upstream behavior, not that it is faster.

### Nineteen misses, and the four shapes they came in

Every claim these sections made from arithmetic or from a failed attempt was
wrong, in four recurring shapes, so the pattern is worth more than the numbers:

| claimed | measured | what was left out |
| --- | --- | --- |
| omni at 1 DiT forward: **RTF 1.5** | 2.8 | the DiT was only a third of the cost |
| Qwen3-TTS: **RTF 0.7** | 2.15 | the codec stage entirely, and the AR half was assumed to reach peak bandwidth |
| Qwen3-TTS on the phone: **RTF 3.6** | 2.14 sprinting, 3.52 sustained | — |
| "Qwen3-TTS cannot hold a voice" | it holds one whenever it is given a reference | `-Base` is the name of the cloning checkpoint, so running it without one was using it wrong |
| "cloning flattens prosody" | true of a speaker encoder, false of in-context cloning | the same weights clone well through a different interface |
| "four threads take ~85% of the bus" | the chip gives up 24.4 GB/s; decode uses 73–75% | that a plateau at four cores is a compute limit |
| "six threads is the wrong count for a phone" | 4, 6 and 8 within 2% | that true premises about a heuristic do not make its output wrong |
| "streaming re-decode is why the codec is the outlier" | buffered is not faster | that the codec's own run-to-run spread is ±14%, three times the difference being read |
| "a longer utterance amortises prefill, so **RTF 2.12**" | 2.95 and 3.22 | that thirty seconds of continuous compute costs 65% of the clock, which dwarfs the 0.2 being chased |
| "the 0.6B is not worth running" | it is 14% a frame, and keeps the written voice | that "cannot reach real time" and "does not help" are different claims, and only the first had been shown |
| "`调` is wrong because it is the base checkpoint" | 1.7B base reads it correctly | that two configurations differing in three things cannot name which one did it |
| "completing the 2×2 is a test with no payoff" | it cleared Q4 on the model this lane ships | that a test's value is what it rules IN elsewhere, not only what it settles about the thing being tested |
| "the codec is the stage that did not scale, at **2.45×**" | 1.90×, same as the other two | that the phone column was Q4 and the laptop column Q8, and the codec is the one row a talker quantization does not move |
| "the long Vulkan take is the seed, not the backend" | three kernel switches each shorten it, and one fixes the delivery | that a sweep over seeds shows a path is not ALWAYS wrong, not that it is right |
| "closest to the CPU's length is the best numeric path" | the furthest of the three sounds best | that length was a proxy nobody had checked against the ear |
| "the 0.6B runaway on Vulkan is a correctness failure, unexplained" | it is MMVQ, and it goes away | that the lossy kernel had already been found, one section up |
| "the 0.6B is slower than the 1.7B on CPU, so it has no place" | 67.9 ms a frame against 78.8 | that the codec splits at 24 s, so two runs either side of that boundary are not comparable |
| "omni lost to qtts on speed" | omni's coarsest setting beats qtts sustained | that RTF 2.8 was the Talker alone past the clock step, and 2.14 the whole synthesis before it |
| "`-D_GNU_SOURCE` treated the symptom of a wrong branch" | it is half the fix; the branch is right and the other half is `sched_setaffinity` | that the llama.cpp checkout on this machine already carried the correct patch |

Row three is worth a second look before the shapes. **RTF 3.6 was the prediction
and 3.52 is the sustained answer**, so the number was very nearly right — and it
was arrived at by leaving the codec stage out of the arithmetic entirely and then
assuming the rest hit peak bandwidth. Two errors of opposite sign landed on a
right answer, and it stayed wrong for the two hours it took to also discover the
throttle: measured against the sprint figure it was out by 60%. **A method that
produces a correct number by cancellation is not confirmed by that number**,
which is the whole reason these rows record what was left out rather than only
how far off the total was.

**Shape one: count the autoregressive half, assume it saturates the bus.** It
does not — 12 frames a second against 1.04 GB is 12.5 GB/s and the measurement
implies about 9.9 — and both times the half that was not counted mattered more
than the half that was.

**Shape two, and it is the one this file already records three times: reading a
limit off one's own failure to configure something.** "The seed does not pin the
speaker, so the model cannot hold a voice" was stated before the model card had
been read. `-Base` is the name of the cloning checkpoint. CustomVoice and
VoiceDesign were one lookup away, and VoiceDesign is both faster and stable.
The correction came from being asked whether a TTS with no way to fix its voice
was plausible — which is the question that should have been asked first, exactly
as with MNN's silence.

The last row is the same shape with a compiler in place of a model. `_GNU_SOURCE`
did not silence the second error, and that was read as *the branch is wrong for
this platform* rather than *this fix is not finished*. It is the cheapest
version of the shape to avoid, because the evidence was not one lookup away but
already on disk: the other checkout on this machine had the finished fix applied
by hand. Nothing recorded that it was there, which is why `third/` now does.

**Shape three: a question with the answer missing from it.**
`yoai_local_set_threads` was added to settle whether decode is
bandwidth-bound, and framed as two outcomes — a flat curve meaning the wall is
per-chip, a rising one meaning it is per-core. The curve rose to four threads
and then flattened, and that was read as the first outcome.

It was neither. Four is this SoC's performance-core count, so the plateau says
the cores ran out and nothing at all about the bus. That reading was not on the
ballot, so the measurement could not return it, and the wrong one was picked by
elimination. No thread sweep of a language model can separate the two, because
every point on it is one engine's kernels as much as the hardware — which is why
`yoai_memory_bandwidth` had to exist.

The 0.6B row is the same shape with the ballot narrowed by one question instead
of one option. "Can the small talker reach real time" was answered correctly —
no, the codec alone forbids it — and then used to answer "is it worth running",
which nobody had asked. It is worth running: 14% a frame, half the prefill, half
the disk, and the written voice kept. **A proof about the ceiling says nothing
about the slope.**

**Shape four: predicting a difference, and never asking how big a difference
the measurement can see.** The thread row and the streaming row are both this,
and both were built out of true statements.

The thread one: qwentts.cpp picks `hardware_concurrency() / 2` reasoning about
hyperthreads, this phone has none, and the llama lane on the same device had
measured prefill and decode flat from four threads up. Every clause true, none
of them implying a penalty, and there is not one — 4, 6 and 8 land within 2%,
which is less than the 9% five consecutive runs drift as the phone warms.

The streaming one is worse, because it had a number. The codec looked like an
outlier at 2.45×, the streaming path really does re-decode its left context per
chunk,
and two runs put 8 chunks at 76.1 ms/frame against 13 chunks' 79.4 — a rate
that should not rise with length. Seven runs on a pinned seed then showed the
codec's spread on *identical input* is 3013 to 3997 ms, ±14%, while the two
autoregressive stages hold ±2.5%. The trend was a quarter of the noise.

**The shared error is the missing denominator.** In both cases the effect was
argued for and then looked for, and in neither was the instrument's own
variance measured first. A pinned seed and a repeat of the same condition cost
one extra run each and are what turned both of these from a story into a number.
Shape three says an option missing from the question cannot be returned by the
answer; this one says a difference smaller than the scatter cannot be either.

The first two shapes are errors in arithmetic and in reading. This one is an
error in the experiment: **an option left out of the question cannot be chosen
by the answer.**

## CosyVoice3 was built and measured, and the reference length is the result

Looked at for one reason: **streaming text input**. Every engine here takes a
complete string and streams only its audio out, so a reply cannot start being
spoken until it has finished being written. CosyVoice2's paper describes an
interleaved layout — N text tokens then M speech tokens, with a filling token
for "the next slot is text and none has arrived" — which is the thing that would
remove that wait. CosyVoice3 inherits it.

**The port does not implement that layout.** `cosyvoice.cpp`'s prompt is
assembled in `src/cosyvoice-llm-job.cpp` as `sos` · reference transcript ·
**the whole utterance text** · `task` · reference speech tokens · accepted
tokens, and only then does decoding start. Text is a prefix, exactly as it is in
Qwen3-TTS, and `llm_prefill` only appends to the tail of the KV cache — which by
then is where the speech tokens are. The low-level API does expose the loop in
pieces (`cosyvoice_tokenize`, `llm_prefill`, `llm_decode`, `sample_token`,
`accept_token`, `token2wav`), so the interleaved layout could be built on it,
but nothing here ships it. The high-level entry points all take `const char*`.

Measured anyway, because the weights were already down and it is the same ggml
family this repo already builds.

**What produced these numbers.** `cosyvoice.cpp` at `f46496b`, whose CMake
shallow-clones ggml master rather than pinning it — here that was **0.24.0 /
`456172e`**, and the Vulkan result below is a reason that matters. Model
`CosyVoice3-2512_Q8_0.gguf` (944 MB) with `speech_tokenizer_v3.onnx` and
`campplus.onnx` for the frontend. **PC only — nothing was run on the phone.**
Cross-lingual mode, pinned seed, 11 threads, which is this file's PC CPU
protocol. Reference: qwentts.cpp's own `freeman.wav`, 17.26 s, and a 6.0 s head
cut of it. Not in [third/manifest.txt](../third/manifest.txt): no lane builds
it, and the checkout was a scratch one.

| reference | audio | generate | RTF |
| --- | --- | --- | --- |
| 17.3 s | 1.32 s | 41.02 s | 31.07 |
| 17.3 s | 10.08 s | 77.37 s | 7.68 |
| 6.0 s | 1.32 s | 13.64 s | 10.34 |
| **6.0 s** | **10.08 s** | **41.94 s** | **4.16** |

**Two lengths per reference is what makes those four numbers a cost rather than
a score.** Solving each pair for a fixed charge plus a rate:

| reference | fixed, per utterance | per second of audio |
| --- | --- | --- |
| 17.3 s | 35.5 s | 4.15 s |
| 6.0 s | **9.4 s** | **3.23 s** |

**Cutting the reference by 65% took 74% off the fixed charge and 22% off the
rate**, and the utterance itself held: the two reference lengths produced the
same 1.32 s and 10.08 s durations to the sample, and level envelopes that track
each other within about 4 dB across every half-second bin. Same content, same
pauses in the same places — not a bit-for-bit match, which a different prompt
could not give. The fixed half is the reference being encoded
and prefilled; the rate half is it sitting in context for every frame
afterwards, which is the same mechanism this file already measured on Qwen3-TTS
base as +47% a frame for 280 extra frames.

**That is the transferable result, and it is not about CosyVoice3.** The lever
was named as untried in four places — the checkpoint summary, the levers table
and the cloning section, all above, and `yoai.h`'s
`yoai_tts_set_reference`. It has now been pulled once, on a different
engine, and the two halves did not move together: the fixed charge scaled with
the reference's length, the per-frame rate did not. The cloning section above
predicted both would. Qwen3-TTS base pays 24 s at 0.6B and 40 s at 1.7B per
utterance for a 22.4-second reference; nothing here says that scales the same
way, and everything here says it is the first thing to measure.

### Deterministic, and correct on CPU

Four threads and eleven threads produced **byte-identical** output — same MD5,
not merely the same duration. This file's Qwen3-TTS figures carry ±15% on this
machine and its 0.6B lost grapheme-to-phoneme to a requantization, so a lane
that does not move under a thread count is worth recording as a difference.

The polyphone probe passes: `调暗` is read `tiáo` at Q8. Q4_K_M was not
downloaded, and on a **0.5B** checkpoint — smaller than the 0.6B that failed
this probe at Q4 — that is the row most likely to fail it.

Output is 24 kHz mono float, which is already what
[audio.ts](../desktop/src/renderer/src/audio.ts) asks its `AudioContext` for, so
nothing about the rate would have to change.

`prompt_speech` extracts once and is reused: 279 KB, 3–4 s, and no ONNX pass per
utterance. Qwen3-TTS has no equivalent — its reference is re-encoded every
`say()`.

### Vulkan does not work on this device, and it fails quietly first

The backend name is `vulkan0`; plain `vulkan` is refused. It then selects the
right device — the same **Intel Arc Pro Graphics** every GPU row in this file was
measured on.

**The short probe ran to completion and produced garbage.** Same text, same
seed, against CPU's 1.32 s of speech at −29 dBFS, Vulkan returned **7.28 s
between −52 and −80 dBFS** — noise floor, and 5.5× the token count. The
corruption is in the LLM stage, not the vocoder: it is not a quieter or rougher
version of the right utterance, it is a different and much longer one. Anything
past about nine characters aborts outright, in
`ggml-vulkan.cpp:2668`, `GGML_ASSERT(!dst || get_misalign_bytes(ctx, dst) == 0)`,
with text splitting on or off alike.

The port's own backend table says Vulkan is verified on an Intel **Raptor Lake**
iGPU. This is Meteor Lake, and the ggml under it is an unpinned master. Either
could be it.

**An exit code of 0 and a WAV of the right shape is not a result.** A Vulkan RTF
of 2.17 was computed from that 7.28 s file and reported before anyone played it.
It measured the cost of generating noise, and it was the most favourable number
this engine produced all day — which is the direction a silent failure tends to
push, because the work it skips is work.

### The arithmetic that closes it

RTF 4.16 is CPU, and CPU has never been the question — Qwen3-TTS reads 1.2 to
1.4 there too and only crosses 1 on the iGPU. So the honest comparison is what
the GPU would have to be worth, and this file already knows: on this Arc,
Qwen3-TTS gains **1.38× at Q8** (98.3 → 71.0 ms/frame) and **1.71× at Q4**
(78.8 → 46.1).

CosyVoice3 needs **4.16×** to reach real time. Q4 is worth 19% on this host and
a 3-second reference might take the rate from 3.23 toward 2.9; together they
land near 2, not near 1. Fixing the Vulkan crash would not change that
conclusion, so it was not attempted.

**And the feature it was looked at for is gated on the same number.**
[audio.ts](../desktop/src/renderer/src/audio.ts)'s `REALTIME_MARGIN` is a
threshold, not a preference: a producer slower than playback is never streamed
at all, it is gathered and played in one piece. A 10-second reply at RTF 4.16
needs 31.6 s of pre-buffer. Streaming text *in* saves a fraction of a second at
the front of a wait that long.

### What this does not settle

Nothing was run on the phone, at Q4, on SYCL, or with a reference shorter than
6 s. Voice similarity across the two reference lengths was judged by envelope
and by one listen, not by anything that would catch a slow drift in timbre. And
one favourable reading survives: CosyVoice3's per-utterance cost falls away with
a shorter reference in a way Qwen3-TTS's does not, because `prompt_speech` is
extracted once. That matters only if the rate term comes down too, and it has
not.

## What llama.cpp can do now

An earlier note here said llama.cpp implements only the Thinker and audio output
is an open question. **Out of date.** `tools/mtmd/` carries a three-stage
generation pipeline — backbone to hidden state, hidden state to audio codes,
codes to PCM — exposed as `mtmd_gen_audio_process()`, with
`MTMD_GEN_AUDIO_TYPE_QWEN3TTS` and `MTMD_GEN_AUDIO_TYPE_POCKETTTS` registered.

**It is still not the omni Talker.** For Qwen3-TTS the stage-1 backbone is
`talker.model`, the TTS model's own — hidden states flow inside the TTS stack,
not from a chat model that just understood something. Excellent text-to-speech,
not end-to-end speech. There is also **no Talker checkpoint in GGUF**: published
conversions are thinker plus mmproj only, so implementing it would mean writing
the conversion as well as the inference.

Audio **input** for Qwen2.5-Omni is fully supported (`PROJECTOR_TYPE_QWEN25O`),
and the registered audio encoders now go well beyond it — Qwen2A, Qwen3A,
Parakeet, Granite Speech, Whisper, Conformer.

## The port exists; multimodal is not in it

This repo already cross-compiles llama.cpp for HarmonyOS —
[harmony/build.cmd](../harmony/build.cmd) `llama`, assembling
`harmony/prebuilt/llama-sdk/<abi>/`. It defaults to
`-march=armv8.6-a+i8mm+bf16+dotprod+fp16`, which is not a guess: a cross build
with no `-march` lands on baseline `armv8-a`, because CMake's feature probes
compile **and run** a test binary and cannot run one for the phone. That costs
2.9× prefill and 2.2× decode, measured.

**Audio is not in that build.** It sets `-DLLAMA_BUILD_TOOLS=OFF`, and `libmtmd`
lives under `tools/mtmd/`. The assembled SDK has `libllama.a` and the three ggml
libraries and nothing else.

**The decision at the top means none of that has to change.** Recognition links
ggml directly; the signal processing needs nothing at all.

## What this must not become

**Never show a person a flat claim about their own emotional state.** The omni
result is why. The model did not say it could not tell — it produced a
confident, specific, plausible reading, and in an app about health that failure
is invisible: it looks right every time.

Everything here surfaces as **deviation from a baseline**, never as a label.
"You spoke thirty percent slower than earlier in this recording" is something
the speaker can check. "You sound tired" is something they can only accept or
resent. The same rule is why the falsifiable dimensions — hoarseness, effort,
fluency — are worth more here than the emotional ones, even though they sound
less impressive.

Use the signal where being wrong is survivable: within one recording, or
aggregated across many, or as an input to how the model phrases its own replies.
Never as a displayed fact.
