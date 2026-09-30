---
marp: true
theme: default
size: 16:9
paginate: true
title: "YoAI: Voice"
---

<style>
section { font-size: 26px; }
section.lead h1 { font-size: 72px; }
section.lead p { color: #57606a; }
section.cover h1 { font-size: 60px; }
section.cover p { color: #57606a; }
table { font-size: 20px; }
code { font-size: 0.9em; }
svg.fig { display: block; width: 100%; height: auto; margin-top: 12px; }
.note { font-size: 20px; color: #57606a; }
</style>

<!-- _class: cover -->
<!-- _paginate: false -->

<!-- Render with HTML enabled (marp --html, or the VS Code setting markdown.marp.html set to all); the diagrams are inline SVG. -->

# YoAI: Voice

Recognition and synthesis sit outside the language model, so **voice works with every provider**, local or remote.

<svg class="fig" viewBox="0 0 1250 330" role="img" aria-label="Microphone to asr to a chat model to the tts Reader to the speaker, with prosody read from the audio">
<defs><marker id="arrow-chain" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker></defs>
<g text-anchor="middle">
<rect x="20" y="40" width="170" height="100" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="105" y="84" font-size="22" font-weight="700" fill="#24292f">Microphone</text>
<text x="105" y="114" font-size="17" fill="#57606a">16 kHz mono</text>
<rect x="280" y="40" width="170" height="100" rx="10" fill="#ddf4ff" stroke="#0969da" stroke-width="2"/>
<text x="365" y="84" font-size="22" font-weight="700" fill="#24292f">asr/</text>
<text x="365" y="114" font-size="17" fill="#57606a">SenseVoice-Small</text>
<rect x="540" y="40" width="170" height="100" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="625" y="84" font-size="22" font-weight="700" fill="#24292f">Chat model</text>
<text x="625" y="114" font-size="17" fill="#57606a">local or remote</text>
<rect x="800" y="40" width="170" height="100" rx="10" fill="#dafbe1" stroke="#1a7f37" stroke-width="2"/>
<text x="885" y="84" font-size="22" font-weight="700" fill="#24292f">tts/ Reader</text>
<text x="885" y="114" font-size="17" fill="#57606a">Qwen3-TTS</text>
<rect x="1060" y="40" width="170" height="100" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="1145" y="84" font-size="22" font-weight="700" fill="#24292f">Speaker</text>
<text x="1145" y="114" font-size="17" fill="#57606a">24 kHz mono</text>
<rect x="280" y="215" width="430" height="90" rx="10" fill="#fff1e5" stroke="#bc4c00" stroke-width="2"/>
<text x="495" y="252" font-size="22" font-weight="700" fill="#24292f">prosody/</text>
<text x="495" y="283" font-size="17" fill="#57606a">pitch, loudness, continuity → arousal, no model</text>
<g font-size="15" fill="#57606a">
<text x="235" y="80">audio</text>
<text x="495" y="80">text</text>
<text x="755" y="80">text deltas</text>
<text x="1015" y="80">PCM</text>
</g>
</g>
<g stroke="#57606a" stroke-width="2" fill="none" marker-end="url(#arrow-chain)">
<line x1="192" y1="90" x2="276" y2="90"/>
<line x1="452" y1="90" x2="536" y2="90"/>
<line x1="712" y1="90" x2="796" y2="90"/>
<line x1="972" y1="90" x2="1056" y2="90"/>
<path d="M105,142 V260 H276"/>
</g>
</svg>

---

## Speech out: what is said, and who says it

<svg class="fig" viewBox="0 0 1250 425" role="img" aria-label="Reply text from a remote or local LLM goes through SpeakableStream, then either whole into a text buffer or cut into pieces by SentenceStream, into the Qwen3-TTS talker, then the emitter and speech. The voice comes from above for Base, a voice file extracted once from a recording or a VoiceDesign sample, or from below for CustomVoice, a speaker name and an optional style. A pacer cuts pieces early when the listener is about to run out.">
<defs>
<marker id="arrow-out" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker>
<marker id="arrow-pacer" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#bc4c00"/></marker>
</defs>
<g text-anchor="middle">
<rect x="200" y="10" width="180" height="50" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="290" y="32" font-size="19" font-weight="700" fill="#24292f">a recording</text>
<text x="290" y="51" font-size="14" fill="#57606a">e.g. recorded in settings</text>
<rect x="200" y="75" width="180" height="50" rx="10" fill="#fff8f2" stroke="#bc4c00" stroke-width="2" stroke-dasharray="6 4"/>
<text x="290" y="97" font-size="19" font-weight="700" fill="#24292f">1.7B VoiceDesign</text>
<text x="290" y="116" font-size="14" fill="#57606a">voice in words → a sample</text>
<text x="100" y="88" font-size="15" fill="#bc4c00">a tool only: the words</text>
<text x="100" y="106" font-size="15" fill="#bc4c00">are the voice; add 开心</text>
<text x="100" y="124" font-size="15" fill="#bc4c00">and the speaker changes</text>
<rect x="430" y="38" width="170" height="60" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="515" y="63" font-size="19" font-weight="700" fill="#24292f">extract the voice</text>
<text x="515" y="85" font-size="14" fill="#57606a">once, about 4.5s</text>
<rect x="650" y="38" width="230" height="60" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="765" y="63" font-size="19" font-weight="700" fill="#24292f">voice file (.spk)</text>
<text x="765" y="85" font-size="14" fill="#57606a">1024 or 2048 numbers</text>
<rect x="20" y="180" width="140" height="50" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="90" y="212" font-size="19" font-weight="700" fill="#24292f">remote LLM</text>
<rect x="20" y="250" width="140" height="50" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="90" y="282" font-size="19" font-weight="700" fill="#24292f">local LLM</text>
<rect x="200" y="200" width="180" height="80" rx="10" fill="#ddf4ff" stroke="#0969da" stroke-width="2"/>
<text x="290" y="233" font-size="17" font-family="monospace" font-weight="700" fill="#24292f">SpeakableStream</text>
<text x="290" y="258" font-size="14" fill="#57606a">markdown → speech text</text>
<rect x="430" y="160" width="170" height="56" rx="10" fill="#ddf4ff" stroke="#0969da" stroke-width="2"/>
<text x="515" y="184" font-size="19" font-weight="700" fill="#24292f">text buffer</text>
<text x="515" y="205" font-size="14" fill="#57606a">cleaned, not cut</text>
<rect x="430" y="264" width="170" height="56" rx="10" fill="#ddf4ff" stroke="#0969da" stroke-width="2"/>
<text x="515" y="288" font-size="17" font-family="monospace" font-weight="700" fill="#24292f">SentenceStream</text>
<text x="515" y="309" font-size="14" fill="#57606a">cut into pieces</text>
<rect x="650" y="190" width="230" height="100" rx="10" fill="#dafbe1" stroke="#1a7f37" stroke-width="2"/>
<text x="765" y="222" font-size="20" font-weight="700" fill="#24292f">Qwen3-TTS talker</text>
<text x="765" y="247" font-size="15" fill="#57606a">Base or CustomVoice</text>
<text x="765" y="270" font-size="15" fill="#57606a">0.6B or 1.7B</text>
<rect x="650" y="335" width="230" height="56" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="765" y="359" font-size="19" font-weight="700" fill="#24292f">speaker name + style</text>
<text x="765" y="380" font-size="14" fill="#57606a">serena · 语气平静 (optional)</text>
<rect x="920" y="200" width="150" height="80" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="995" y="233" font-size="19" font-weight="700" fill="#24292f">emitter</text>
<text x="995" y="258" font-size="14" fill="#57606a">in reading order</text>
<rect x="1105" y="205" width="125" height="70" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="1167" y="235" font-size="19" font-weight="700" fill="#24292f">speech</text>
<text x="1167" y="258" font-size="14" fill="#57606a">24 kHz</text>
</g>
<g font-size="15" fill="#57606a">
<text x="430" y="152">whole reply</text>
<text x="430" y="256">pieces</text>
<text x="777" y="322" font-size="14">CustomVoice: built-in speakers</text>
</g>
<text x="777" y="140" font-size="15" fill="#bc4c00">Base: no style, so emotion and pace cannot be set</text>
<g stroke="#57606a" stroke-width="2" fill="none" marker-end="url(#arrow-out)">
<path d="M382,35 H405 V68 H426"/>
<path d="M382,100 H405 V68 H426"/>
<line x1="602" y1="68" x2="646" y2="68"/>
<line x1="765" y1="100" x2="765" y2="186"/>
<path d="M162,205 H180 V240 H196"/>
<path d="M162,275 H180 V240 H196"/>
<path d="M382,240 H405 V188 H426"/>
<path d="M382,240 H405 V292 H426"/>
<path d="M602,188 H625 V240 H646"/>
<path d="M602,292 H625 V240 H646"/>
<line x1="765" y1="333" x2="765" y2="294"/>
<line x1="882" y1="240" x2="916" y2="240"/>
<line x1="1072" y1="240" x2="1101" y2="240"/>
</g>
<path d="M995,282 V415 H515 V324" stroke="#bc4c00" stroke-width="2" stroke-dasharray="6 4" fill="none" marker-end="url(#arrow-pacer)"/>
<text x="780" y="408" font-size="15" fill="#bc4c00" text-anchor="middle">pacer: under 1s of audio left → cut now</text>
</svg>

<ul class="note">
<li>The voice file is one slot in the prompt, so Base costs no more per frame. A real recording holds pieces tighter than a VoiceDesign sample: 1.31 against 1.68 semitones of spread.</li>
</ul>

---

## Real-time factor (RTF)

Below 1 the speech keeps up with playback; above 1 a stream needs duration×(RTF−1) of buffer.

| `Q4_K_M` | Kirin 9020 (ARM) | Core Ultra 9 185H (x86) | Arc iGPU (Vulkan) | RTX 3090 |
| --- | --- | --- | --- | --- |
| 0.6B Base | 2.61 | 0.91–0.95 | 0.44 | not measured |
| 1.7B Base | 3.15 | 1.14 | **0.55** | not measured |
| 0.6B CustomVoice | 2.87 | 0.96–0.97 | 0.44–0.45¹ | not measured |
| 1.7B CustomVoice | 3.64 | 1.12–1.13 | 0.55–0.57 | not measured |

<ul class="note">
<li>Kirin 9020: 6 threads, clock steps down after about 30s, GPU unusable. Core Ultra 9: 11 threads, ±15% across days.</li>
<li>Per 80ms frame at 1.7B, CPU → iGPU: codec 34.9 → 6.3ms, talker 23.9 → 17.3ms, code predictor 20.0 → 20.5ms. The code predictor is sized by the 16 codebooks, so neither a GPU nor a smaller talker speeds it up.</li>
<li>The chat model stays on the CPU: sharing the iGPU with it pushed speech to RTF 1.64.</li>
<li>Reproduce: <code>python tests/tts_rtf.py --models &lt;dir&gt;</code> on a PC; on the phone, /tts → Benchmark.</li>
<li>¹ Sounds worse on Vulkan for now; the cause may still be in our code.</li>
</ul>

---

<style scoped>
section { font-size: 18px; padding-top: 36px; padding-bottom: 36px; }
h2 { font-size: 32.5px; margin-bottom: 0.2em; }   /* the other slides' size: 1.25em of 26px */
p { margin: 0.3em 0; }
svg.fig { width: 64%; margin: 6px auto 0; }
.cols { display: grid; grid-template-columns: 1fr 1fr; gap: 36px; font-size: 16px; line-height: 1.4; }
.cols h3 { font-size: 19px; margin: 0.3em 0 0.2em; }
.cols ul { margin: 0; padding-left: 1.1em; }
.cols li { margin: 0.1em 0; }
.cols p { margin: 0.5em 0 0.3em; }
table { font-size: 14px; }
th, td { padding: 2px 9px !important; }
</style>

## The reading problem, and two modes

A reply takes **seconds to write**, and a single `say()` waits for the last token; faster synthesis cannot recover that. `Reader` speaks while it is written: `begin()`, `feed(delta)` per token from any provider, `finish()`.

<svg class="fig" viewBox="0 0 1200 255" role="img" aria-label="Timelines: whole reply is one continuous generation; pieces are cut from the text and played one after another with seams">
<defs><marker id="arrow-time" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker></defs>
<g font-size="22" fill="#57606a">
<line x1="230" y1="22" x2="270" y2="22" stroke="#0969da" stroke-width="14" stroke-dasharray="8 5"/>
<text x="280" y="30">text from the model</text>
<rect x="530" y="12" width="40" height="18" rx="3" fill="#1a7f37"/>
<text x="580" y="30">audio</text>
<line x1="710" y1="8" x2="710" y2="36" stroke="#bc4c00" stroke-width="3"/>
<text x="722" y="30">seam</text>
<line x1="840" y1="8" x2="840" y2="36" stroke="#24292f" stroke-width="2"/>
<text x="852" y="30">cut</text>
</g>
<text x="20" y="112" font-size="26" font-weight="700" fill="#24292f">Whole reply</text>
<line x1="230" y1="85" x2="780" y2="85" stroke="#0969da" stroke-width="16" stroke-dasharray="8 5"/>
<rect x="275" y="102" width="860" height="26" rx="4" fill="#1a7f37"/>
<text x="705" y="121" font-size="19" fill="#ffffff" text-anchor="middle">one generation</text>
<text x="20" y="197" font-size="26" font-weight="700" fill="#24292f">Pieces</text>
<line x1="230" y1="170" x2="780" y2="170" stroke="#0969da" stroke-width="16" stroke-dasharray="8 5"/>
<g stroke="#24292f" stroke-width="2">
<line x1="290" y1="156" x2="290" y2="184"/>
<line x1="470" y1="156" x2="470" y2="184"/>
<line x1="780" y1="156" x2="780" y2="184"/>
</g>
<rect x="305" y="187" width="190" height="26" rx="4" fill="#1a7f37"/>
<rect x="499" y="187" width="300" height="26" rx="4" fill="#1a7f37"/>
<rect x="803" y="187" width="332" height="26" rx="4" fill="#1a7f37"/>
<g font-size="19" fill="#ffffff" text-anchor="middle">
<text x="400" y="206">1</text>
<text x="649" y="206">2</text>
<text x="969" y="206">3</text>
</g>
<g stroke="#bc4c00" stroke-width="3">
<line x1="497" y1="184" x2="497" y2="219"/>
<line x1="801" y1="184" x2="801" y2="219"/>
</g>
<line x1="230" y1="240" x2="1125" y2="240" stroke="#57606a" stroke-width="2" marker-end="url(#arrow-time)"/>
<text x="1135" y="247" font-size="20" fill="#57606a">time</text>
</svg>

<div class="cols">
<div>

### Whole reply · `set_whole_reply(true)`

- One generation per reply. The text is pulled into the talker as it is written: `Qtts::say_pulled`, which needs `qwentts-streaming-text.patch`.
- **No seams**, and the pace moves across the reply the way a whole take's does.
- The talker reads **one text token per frame**, 12.5 a second, and waits when the text runs out. So the writer has to stay above **about 17–18 Chinese characters a second**.

With a simulated writer, a player that waits for 0.5s of audio:

| writer, characters/s | ≈ tokens/s | playback starts, s | starved, s |
| --- | --- | --- | --- |
| 29 | 21 | 0.78–1.11 | 0 |
| 21 | 15 | 0.98–1.52 | 0 |
| 14 | 10 | 1.36–2.26 | **1.16–1.24** |

</div>
<div>

### Pieces · `set_whole_reply(false)`

- `SentenceStream` cuts the text as it arrives. Each piece is its own `say()`.
- **Piece 1 is the whole of the wait**: 12–40 characters, and a full stop ends it from 6 on.
- **Later pieces** wait for 30–80 characters (30 sounded better than 12), and are cut early when the listener has under 1s of audio left.
- **Cost**: every piece starts from a sentence's opening state, so the pace is even and resets at every seam; N pieces are N draws. The voice file holds the register: 1.31 semitones of spread across pieces, against 3.09 without it.
- **Tolerant of a slow or bursty writer**: each piece is complete text before it is spoken.

</div>
</div>

---

## Rules that came from listening

| rule | what it fixes |
| --- | --- |
| **Pin the seed** (desktop: 101) | A free seed is redrawn per `say()`. Split into pieces, the pitch spread between utterances went from 10.1 Hz to 17.5 |
| **Piece 1 ends at a full stop from 6 characters** | "你好。" and "你觉得呢？" spoken alone come out clipped, so they join what follows |
| **The last piece merges back to 40 characters** | A short last piece stops while the vowel still sounds, at −34 dBFS. Folded into a longer one, it decays to −68. On a stream it can merge only while the piece before it is still queued |
| **Hold 2 frames past the first stop** (160ms) | With the streaming text layout, endings stopped mid-vowel. Four frames sounded no different |

<p class="note">Each was found by listening. None of them shows in RTF.</p>

---

## Measured in the app

Desktop, speech on the Arc iGPU, the chat model on the CPU. First audio is from the message being sent, including prefill.

| model | mode | decode while reading, tok/s | first audio, s | starved, s |
| --- | --- | --- | --- | --- |
| Qwen3.5-2B | pieces | 20.5–21.1 | 0.89–1.20 | 0.24–0.32 |
| Qwen3.5-2B | **whole reply** | 20.0–21.9 | **0.44–1.15** | 0–0.31 |
| Qwen3.5-4B | pieces | 4.0–8.1 | 1.17–2.73 | 0.20–0.33 |
| Qwen3.5-4B | whole reply | 5.7–6.5 | 1.10–2.05 | **0.38–2.11** |

- The 2B writes fast enough: whole reply starts earlier and has no seams.
- The 4B does not: whole reply runs dry for up to 2.1s, while pieces stay under 0.33s.

---

## Choosing a mode: proposed, not built

- Today `WHOLE_REPLY = true` is fixed in `desktop/src/main/reader.ts`. With the 4B it runs dry.
- **Decide per turn, before `begin()`**, from the writer's rate while speech runs, in **characters a second** rather than the provider's tokens. Fast enough → whole reply; otherwise pieces.
- **Never switch within a reply.** A whole reply cut into pieces midway gains a seam.
- **Remote providers deliver text in bursts**, and whole reply turns every gap into silence. Not measured: pieces until it is.

---

## Outside the desktop: `yoai-server`

The same `Reader` behind a WebSocket. Any client can feed it, for example a service relaying a remote model's deltas.

```text
→ {"type":"start"}
← {"type":"started","sample_rate":24000,"channels":1,"format":"s16"}
→ {"type":"text","text":"我查了一下，"}          (any number of these)
← binary PCM, mono 24 kHz, in reading order
→ {"type":"finish"}
← {"type":"done", "first_audio_seconds": ..., "starved_seconds": ...}
```

- **One passage at a time, server-wide**: a second `start` gets `busy`.
- The reading settings are the desktop's. Clients cannot change them.
- It reads "你好。" before opening the port: a GPU compiles its shaders on the first synthesis, and the first client waited 13.7s for audio against 1.9s afterwards.

---

## Open questions

- **Remote writers**: how bursty delivery interacts with whole reply. Unmeasured.
- **Whole reply's first audio** waits for the second pre-tokenizer chunk: 0.25–0.57s of `TextWait` in Chinese.
- **Pieces sound read aloud.** Two levers are unbuilt: give each piece the previous one's last seconds as a reference in the prompt, and a per-piece time-stretch from a pace plan.
- **Emotion and pace on Base.** Candidates, none measured: style carried in the voice file, and a reference recording in the prompt to steer by example.
- **The code predictor** is the largest stage on the GPU, and nothing tried so far reaches it.
