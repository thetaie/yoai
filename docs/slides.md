---
marp: true
theme: default
size: 16:9
paginate: true
title: "Voice streaming"
---

<style>
section { font-size: 26px; }
section.lead h1 { font-size: 72px; }
section.lead p { color: #57606a; }
section.cover { padding-top: 48px; padding-bottom: 40px; }
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

# Voice streaming

<svg class="fig" viewBox="0 -102 1250 574" role="img" aria-label="The microphone feeds an echo canceller, which takes what the speaker plays back out of the microphone signal, with the PCM played as its reference; its output feeds barge-in and three paths to the speaker. The main line, framed and labeled The Qwen-Omni experience, without Qwen-Omni: ASR hands a chat model, local or remote, whole sentences cut at pauses, and the chat model streams its reply to TTS token by token. Above ASR, barge-in takes the echo-cancelled microphone and ASR's words: a voice over the reply pauses the speaker, and words stop the chat model, TTS and speaker, the history keeping only what was played. Beside ASR, emotion in reads the user's arousal from the sound, cut and timed by the transcript, and passes it to the chat model; the chat model hands its reply's valence, arousal and dominance to emotion out, which steers the TTS voice file. Below it, two omni paths, where one model takes the audio and speaks the reply itself: Qwen3-Omni-30B-A3B on vLLM-Omni, a remote provider on a GPU server or desktop, kept, which handles barge-in itself by its server VAD, made of a MoE thinker (30B total, 3B active, 48 layers, a router per layer picking 8 of 128 experts), a MoE talker (3B total, 0.3B active, 20 layers, 6 of 128 experts plus one shared), and a dense codec; and Qwen2.5-Omni-3B on MNN, a local provider on the device at RTF 3.0 on a phone, replaced by the main line.">
<defs>
<marker id="arrow-chain" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker>
<marker id="arrow-ref" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#bc4c00"/></marker>
<marker id="bar-stop" viewBox="0 0 4 22" refX="2" refY="11" markerWidth="4" markerHeight="22" markerUnits="userSpaceOnUse" orient="auto"><rect width="4" height="22" fill="#cf222e"/></marker>
</defs>
<rect x="352" y="-62" width="706" height="280" rx="16" fill="none" stroke="#24292f" stroke-width="2" stroke-dasharray="8 5"/>
<text x="354" y="-76" font-size="24" font-weight="700" fill="#24292f">The Qwen-Omni experience, without Qwen-Omni</text>
<g text-anchor="middle">
<rect x="16" y="36" width="130" height="84" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="81" y="72" font-size="20" font-weight="700" fill="#24292f">Microphone</text>
<text x="81" y="100" font-size="17" fill="#57606a">16 kHz mono</text>
<rect x="185" y="36" width="130" height="84" rx="10" fill="#fff1e5" stroke="#bc4c00" stroke-width="2"/>
<text x="250" y="72" font-size="22" font-weight="700" fill="#24292f">AEC</text>
<text x="250" y="100" font-size="17" fill="#57606a">echo canceller</text>
<rect x="375" y="36" width="150" height="84" rx="10" fill="#ddf4ff" stroke="#0969da" stroke-width="2"/>
<text x="450" y="72" font-size="22" font-weight="700" fill="#24292f">ASR</text>
<text x="450" y="100" font-size="17" fill="#57606a">SenseVoice-Small</text>
<rect x="655" y="36" width="140" height="84" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="725" y="72" font-size="22" font-weight="700" fill="#24292f">Chat model</text>
<text x="725" y="100" font-size="17" fill="#57606a">local or remote</text>
<rect x="925" y="36" width="120" height="84" rx="10" fill="#dafbe1" stroke="#1a7f37" stroke-width="2"/>
<text x="985" y="72" font-size="22" font-weight="700" fill="#24292f">TTS</text>
<text x="985" y="100" font-size="17" fill="#57606a">Qwen3-TTS</text>
<rect x="1104" y="36" width="130" height="84" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="1169" y="72" font-size="22" font-weight="700" fill="#24292f">Speaker</text>
<text x="1169" y="100" font-size="17" fill="#57606a">24 kHz mono</text>
<rect x="375" y="-50" width="250" height="56" rx="10" fill="#ffebe9" stroke="#cf222e" stroke-width="2"/>
<text x="500" y="-27" font-size="18" font-weight="700" fill="#24292f">Barge-in</text>
<text x="500" y="-6" font-size="15" fill="#57606a">pause on voice, stop on words</text>
<rect x="375" y="150" width="150" height="56" rx="10" fill="#fff1e5" stroke="#bc4c00" stroke-width="2"/>
<text x="450" y="173" font-size="18" font-weight="700" fill="#24292f">Emotion in</text>
<text x="450" y="194" font-size="15" fill="#57606a">arousal from sound</text>
<rect x="885" y="150" width="160" height="56" rx="10" fill="#fff1e5" stroke="#bc4c00" stroke-width="2"/>
<text x="965" y="173" font-size="18" font-weight="700" fill="#24292f">Emotion out</text>
<text x="965" y="194" font-size="15" fill="#57606a">V, A, D → voice file</text>
<rect x="375" y="238" width="680" height="132" rx="10" fill="#fbefff" stroke="#8250df" stroke-width="2"/>
<text x="715" y="262" font-size="20" font-weight="700" fill="#24292f">Qwen3-Omni-30B-A3B</text>
<text x="715" y="284" font-size="16" fill="#57606a">vLLM-Omni · remote provider, on a GPU server or desktop · <tspan fill="#cf222e">barge-in by its server VAD</tspan></text>
<rect x="389" y="296" width="240" height="62" rx="8" fill="#ffffff" stroke="#8250df" stroke-width="1.5"/>
<text x="509" y="316" font-size="16" font-weight="700" fill="#24292f">Thinker · MoE</text>
<text x="509" y="334" font-size="14" fill="#57606a">30B total, 3B active · 48 layers</text>
<text x="509" y="350" font-size="14" fill="#57606a">router per layer: 8 of 128</text>
<rect x="653" y="296" width="240" height="62" rx="8" fill="#ffffff" stroke="#8250df" stroke-width="1.5"/>
<text x="773" y="316" font-size="16" font-weight="700" fill="#24292f">Talker · MoE</text>
<text x="773" y="334" font-size="14" fill="#57606a">3B total, 0.3B active · 20 layers</text>
<text x="773" y="350" font-size="14" fill="#57606a">router per layer: 6 of 128 + shared</text>
<rect x="917" y="296" width="124" height="62" rx="8" fill="#ffffff" stroke="#8250df" stroke-width="1.5"/>
<text x="979" y="316" font-size="16" font-weight="700" fill="#24292f">Codec · dense</text>
<text x="979" y="334" font-size="14" fill="#57606a">code predictor</text>
<text x="979" y="350" font-size="14" fill="#57606a">+ Code2Wav</text>
<rect x="375" y="390" width="680" height="56" rx="10" fill="#fbefff" stroke="#8250df" stroke-width="2"/>
<text x="715" y="412" font-size="20" font-weight="700" fill="#24292f">Qwen2.5-Omni-3B</text>
<text x="715" y="434" font-size="16" fill="#57606a">MNN · local provider, on the device · RTF 3.0 on a phone</text>
<rect x="955" y="244" width="86" height="24" rx="12" fill="#8250df" stroke="#8250df" stroke-width="1.5"/>
<text x="998" y="261" font-size="14" fill="#ffffff">kept</text>
<rect x="955" y="406" width="86" height="24" rx="12" fill="#ffffff" stroke="#8250df" stroke-width="1.5"/>
<text x="998" y="423" font-size="14" fill="#8250df">replaced</text>
<g font-size="15" fill="#57606a">
<text x="589" y="64">whole sentences</text>
<text x="589" y="108">cut at pauses</text>
<text x="859" y="64">streamed</text>
<text x="859" y="108">token by token</text>
<text x="1082" y="64">PCM</text>
<text x="613" y="170">user's arousal</text>
<text x="458" y="140" text-anchor="start">text + timing</text>
<text x="458" y="28" text-anchor="start">words</text>
<text x="817" y="170">reply's V, A, D</text>
</g>
</g>
<g font-size="15" fill="#bc4c00" text-anchor="end">
<text x="240" y="300">reference:</text>
<text x="240" y="319">PCM played</text>
</g>
<g stroke="#57606a" stroke-width="2" fill="none" marker-end="url(#arrow-chain)">
<line x1="148" y1="78" x2="181" y2="78"/>
<line x1="340" y1="78" x2="371" y2="78"/>
<line x1="340" y1="178" x2="371" y2="178"/>
<line x1="340" y1="327" x2="371" y2="327"/>
<line x1="340" y1="418" x2="371" y2="418"/>
<line x1="527" y1="78" x2="651" y2="78"/>
<line x1="797" y1="78" x2="921" y2="78"/>
<line x1="1047" y1="78" x2="1100" y2="78"/>
<path d="M340,78 V-22 H371"/>
<line x1="450" y1="34" x2="450" y2="10"/>
<path d="M527,178 H700 V124"/>
<path d="M750,122 V178 H881"/>
<line x1="985" y1="148" x2="985" y2="124"/>
<line x1="450" y1="122" x2="450" y2="146"/>
<line x1="631" y1="327" x2="649" y2="327"/>
<line x1="895" y1="327" x2="913" y2="327"/>
</g>
<g stroke="#57606a" stroke-width="2" fill="none">
<path d="M317,78 H340 V418"/>
<path d="M1057,327 H1076 V78"/>
<path d="M1057,418 H1076 V327"/>
</g>
<g fill="#57606a">
<circle cx="340" cy="78" r="4"/>
<circle cx="340" cy="178" r="4"/>
<circle cx="340" cy="327" r="4"/>
<circle cx="1076" cy="78" r="4"/>
</g>
<path d="M1169,122 V462 H250 V124" stroke="#bc4c00" stroke-width="2" fill="none" marker-end="url(#arrow-ref)"/>
<g stroke="#cf222e" stroke-width="2" fill="none" marker-end="url(#bar-stop)">
<path d="M627,-22 H1169 V32"/>
<line x1="725" y1="-22" x2="725" y2="32"/>
<line x1="985" y1="-22" x2="985" y2="32"/>
</g>
<g fill="#cf222e">
<circle cx="725" cy="-22" r="4"/>
<circle cx="985" cy="-22" r="4"/>
</g>
<text x="733" y="-30" font-size="15" fill="#cf222e">history keeps what was played</text>
<g fill="#0969da">
<rect x="540" y="72" width="40" height="12" rx="3"/>
<rect x="590" y="72" width="40" height="12" rx="3"/>
</g>
<line x1="801" y1="78" x2="904" y2="78" stroke="#0969da" stroke-width="12" stroke-dasharray="6 4"/>
</svg>

---

<style scoped>
section { font-size: 18px; padding: 36px 56px; }
h2 { font-size: 32.5px; margin-bottom: 0.2em; }   /* the other slides' size: 1.25em of 26px */
p { margin: 0.3em 0; }
svg.fig { margin-top: 6px; }
.cols { display: grid; grid-template-columns: 1fr 1fr; gap: 32px; font-size: 15px; line-height: 1.35; margin-top: 4px; }
.cols h3 { font-size: 19px; margin: 0.3em 0 0.2em; }
.cols ul { margin: 0; padding-left: 1.1em; }
.cols li { margin: 0.2em 0; }
.cols p { margin: 0.3em 0; }
table { font-size: 14px; margin: 4px 0; }
th, td { padding: 3px 9px !important; }
.note { font-size: 14px; margin-top: 12px; padding-top: 6px; border-top: 1px solid #d0d7de; }
</style>

## AEC: take the reply out of the microphone

The microphone hears the reply as well as the user. The AEC learns how the reply reaches the microphone and subtracts it, leaving the user.

<svg class="fig" viewBox="0 0 1250 286" role="img" aria-label="Left to right. What was played, the reference x, goes two ways. Along the top, the speaker plays it and the echo path h, buffering, loudspeaker and room, carries it to the microphone, which also hears the user talking. Along the bottom, the adaptive filter, its model of h, turns x into an echo estimate. The microphone minus the estimate is the output, roughly the user's voice; it is also the error that goes back to the filter to learn from.">
<defs>
<marker id="arrow-aec" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker>
<marker id="arrow-aec-echo" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#bc4c00"/></marker>
</defs>
<g stroke-width="2">
<rect x="20" y="170" width="180" height="60" rx="10" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="370" y="50" width="160" height="60" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
<rect x="760" y="50" width="180" height="60" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
<rect x="320" y="170" width="260" height="60" rx="10" fill="#fff1e5" stroke="#bc4c00"/>
<rect x="1050" y="170" width="180" height="60" rx="10" fill="#ddf4ff" stroke="#0969da"/>
<circle cx="850" cy="200" r="16" fill="#ffffff" stroke="#57606a"/>
</g>
<g text-anchor="middle" fill="#24292f">
<text x="110" y="196" font-size="20" font-weight="700">Played</text>
<text x="110" y="218" font-size="14" fill="#57606a">the reply: reference x</text>
<text x="450" y="76" font-size="20" font-weight="700">Speaker</text>
<text x="450" y="98" font-size="14" fill="#57606a">plays x</text>
<text x="850" y="76" font-size="20" font-weight="700">Microphone</text>
<text x="850" y="98" font-size="14" fill="#57606a">echo + user</text>
<text x="450" y="196" font-size="20" font-weight="700">Adaptive filter</text>
<text x="450" y="218" font-size="14" fill="#57606a">ĥ: its model of h</text>
<text x="1140" y="196" font-size="20" font-weight="700">Output</text>
<text x="1140" y="218" font-size="14" fill="#57606a">e ≈ the user</text>
<text x="850" y="206" font-size="18">Σ</text>
</g>
<g font-size="15" fill="#57606a" text-anchor="middle">
<text x="850" y="18">the user talks</text>
<text x="706" y="190">ŷ: the echo estimate</text>
<text x="650" y="260">e: the error, which it learns from</text>
<text x="819" y="190">−</text>
<text x="864" y="160">+</text>
<text x="956" y="190">e</text>
</g>
<g font-size="15" fill="#bc4c00" text-anchor="middle">
<text x="644" y="70">echo path h</text>
<text x="644" y="101">buffering, loudspeaker, room</text>
</g>
<g stroke="#57606a" stroke-width="2" fill="none" marker-end="url(#arrow-aec)">
<line x1="202" y1="200" x2="316" y2="200"/>
<path d="M250,200 V80 H366"/>
<line x1="850" y1="26" x2="850" y2="46"/>
<line x1="850" y1="112" x2="850" y2="180"/>
<line x1="582" y1="200" x2="830" y2="200"/>
<line x1="866" y1="200" x2="1046" y2="200"/>
<path d="M850,216 V270 H450 V234"/>
</g>
<g fill="#57606a">
<circle cx="250" cy="200" r="4"/>
</g>
<line x1="532" y1="80" x2="756" y2="80" stroke="#bc4c00" stroke-width="2" stroke-dasharray="6 4" marker-end="url(#arrow-aec-echo)"/>
</svg>

<div class="cols">
<div>

### How it learns

- **A partitioned frequency-domain filter (MDF).** 16 partitions of 16ms model 256ms of echo path; the output is at most 16ms late.
- **Step size per bin (Valin, 2007):** the residual echo's share of the error. When the user talks, the error grows but the echo estimate does not, so the step falls and the filter holds. No double-talk detector.
- **Safeguards.** The first second learns on a fixed step. A filter that leaves the output louder than its input for 0.8s is reset.

</div>
<div>

### Measured on synthetic echo

| case | ERLE |
| --- | --- |
| white noise, 145ms path, at 8–10s | 59.5 dB |
| a voice at the echo's level: before, during, after | 33.1, 29.8, 48.0 dB |
| the path moves 5ms, into another room | −2 dB, then 32.3 dB 6–8s later |
| speech, the echo 6 dB over the user: while talking, at 18–22s | 17–20, 27–29 dB |

ERLE: how far the echo is pushed down. RTF 0.011 on the PC's CPU.

</div>
</div>

<p class="note">Linear only. What it cannot model stays, such as loudspeaker distortion, and recognition transcribes echo even 20 dB down. An echo path longer than 256ms, buffering included, is not cancelled at all.</p>

---

<style scoped>
section { font-size: 18px; padding: 36px 56px; }
h2 { font-size: 32.5px; margin-bottom: 0.2em; }   /* the other slides' size: 1.25em of 26px */
p { margin: 0.3em 0; }
.cols { display: grid; grid-template-columns: repeat(3, 1fr); gap: 28px; font-size: 15px; line-height: 1.35; }
.cols h3 { font-size: 19px; margin: 0.4em 0 0.2em; }
.cols svg { display: block; width: 100%; height: auto; margin: 6px 0 2px; }
.cols ul { margin: 0.4em 0 0; padding-left: 1.1em; }
.cols li { margin: 0.3em 0; }
.cols li span { display: block; font-size: 13px; color: #57606a; }
.more { font-size: 13px; color: #57606a; }
.note { font-size: 14px; }
</style>

## Autoregressive TTS: three shapes

All generate frame by frame in an LM. They differ in what a step emits, and what runs after it.

<div class="cols">
<div>

### Discrete AR

<svg viewBox="0 0 380 46" role="img" aria-label="Per frame, a language-model step and a small depth model; then the codec">
<defs><marker id="arrow-dar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker></defs>
<g stroke-width="2">
<rect x="1" y="6" width="46" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="50" y="6" width="54" height="34" rx="6" fill="#ddf4ff" stroke="#0969da"/>
<rect x="113" y="6" width="46" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="162" y="6" width="54" height="34" rx="6" fill="#ddf4ff" stroke="#0969da"/>
<rect x="282" y="6" width="96" height="34" rx="6" fill="#f6f8fa" stroke="#d0d7de"/>
</g>
<g text-anchor="middle" font-size="14" fill="#24292f">
<text x="24" y="28">LM</text>
<text x="77" y="28">depth</text>
<text x="136" y="28">LM</text>
<text x="189" y="28">depth</text>
<text x="229" y="28">…</text>
<text x="330" y="28">codec</text>
</g>
<line x1="242" y1="23" x2="278" y2="23" stroke="#57606a" stroke-width="2" marker-end="url(#arrow-dar)"/>
</svg>

A step emits codec tokens; a small depth model fills the other codebooks.

- **Qwen3-TTS** 0.6B / 1.7B · Apache 2.0 <span>Alibaba, 2026-01 · in use here</span>
- **Breeze TTS 2** 3B · non-commercial <span>BreezeBlue, 2026-08 · Qwen3-TTS's codec</span>
- **Fish Audio S2-Pro** 4B + 0.4B · research <span>Fish Audio, 2026-03 · slow AR over time, fast over codebooks</span>
- **Higgs Audio v3** 4B · non-commercial <span>Boson AI, 2026-06 · Qwen3-4B, ~100 languages</span>
- **MOSS-TTS-Nano** 0.1B · Apache 2.0 <span>OpenMOSS, 2026 · streams on 4 CPU cores; family up to 4B</span>
- **Magpie TTS** 0.36B · NVIDIA Open Model <span>NVIDIA, 2026-07 · 2 frames a step; no cloning</span>

<p class="more">Earlier: Orpheus, Llasa, Spark-TTS, Dia2, Kyutai TTS, OuteTTS, Sesame CSM. Closed: VALL-E (the first), MiMo-V2-TTS.</p>

</div>
<div>

### AR, then flow or diffusion

<svg viewBox="0 0 380 46" role="img" aria-label="Several language-model steps, then a flow network of N steps over the chunk, then a vocoder">
<defs><marker id="arrow-flow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker></defs>
<g stroke-width="2">
<rect x="1" y="6" width="35" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="40" y="6" width="35" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="79" y="6" width="35" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="118" y="6" width="35" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="186" y="6" width="104" height="34" rx="6" fill="#fff1e5" stroke="#bc4c00"/>
<rect x="308" y="6" width="70" height="34" rx="6" fill="#f6f8fa" stroke="#d0d7de"/>
</g>
<g text-anchor="middle" font-size="14" fill="#24292f">
<text x="18.5" y="28">LM</text>
<text x="57.5" y="28">LM</text>
<text x="96.5" y="28">LM</text>
<text x="135.5" y="28">LM</text>
<text x="238" y="28">flow ×N</text>
<text x="343" y="28">vocoder</text>
</g>
<g stroke="#57606a" stroke-width="2" marker-end="url(#arrow-flow)">
<line x1="157" y1="23" x2="182" y2="23"/>
<line x1="292" y1="23" x2="304" y2="23"/>
</g>
</svg>

The LM emits semantic tokens; a non-AR network turns each chunk into sound in N steps.

- **CosyVoice 2 / CosyVoice3** 0.5B · Apache 2.0 <span>Alibaba, 2024-12 / 2025-12 · measured here</span>
- **IndexTTS-2.5** 0.8B · Bilibili license <span>bilibili, 2026-08 · inline pinyin, duration, emotion vector</span>
- **GLM-TTS** · license unverified <span>Zhipu, 2025-12 · phonemes in the text</span>
- **Step-Audio-EditX** 3B · Apache 2.0 <span>StepFun, 2025-11 · edits emotion and style</span>
- **Chatterbox** 0.5B · MIT <span>Resemble AI, 2025 · Turbo: one-step flow</span>
- **Voxtral TTS** 4B · CC BY-NC <span>Mistral, 2026-03 · 16 flow steps inside every frame</span>

<p class="more">Closed: Seed-TTS, MiniMax-Speech. Also Kimi-Audio's speech.</p>

</div>
<div>

### Continuous AR

<svg viewBox="0 0 380 46" role="img" aria-label="Per frame, a language-model step and a diffusion head of N steps; then a decoder">
<defs><marker id="arrow-cont" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker></defs>
<g stroke-width="2">
<rect x="1" y="6" width="40" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="44" y="6" width="64" height="34" rx="6" fill="#fff1e5" stroke="#bc4c00"/>
<rect x="117" y="6" width="40" height="34" rx="6" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="160" y="6" width="64" height="34" rx="6" fill="#fff1e5" stroke="#bc4c00"/>
<rect x="282" y="6" width="96" height="34" rx="6" fill="#f6f8fa" stroke="#d0d7de"/>
</g>
<g text-anchor="middle" font-size="14" fill="#24292f">
<text x="21" y="28">LM</text>
<text x="76" y="28">diff ×N</text>
<text x="137" y="28">LM</text>
<text x="192" y="28">diff ×N</text>
<text x="237" y="28">…</text>
<text x="330" y="28">decoder</text>
</g>
<line x1="250" y1="23" x2="278" y2="23" stroke="#57606a" stroke-width="2" marker-end="url(#arrow-cont)"/>
</svg>

No tokenizer; every step runs its own diffusion loop, so size does not predict cost.

- **VoxCPM2** 2B · Apache 2.0 <span>OpenBMB, 2026-04 · 30 languages, 48 kHz; ggml port</span>
- **VibeVoice** 1.5B / 7B · MIT <span>Microsoft, 2025-08 · 90 minutes, 4 speakers; TTS pulled</span>
- **Pocket TTS** 0.1B · MIT <span>Kyutai, 2026-01 · one-step head; in llama.cpp</span>

</div>
</div>

<p class="note">As of September 2026, from model cards; only Qwen3-TTS and CosyVoice3 were run here. Not AR, so left out: F5-TTS, MaskGCT, MegaTTS 3, Kokoro.
<br>Architecture undisclosed: ElevenLabs v3, Gemini 3.8 Flash TTS, Cartesia Sonic 3.6.</p>

---

<style scoped>
section { font-size: 18px; padding: 36px 56px; }
h2 { font-size: 32.5px; margin-bottom: 0.2em; }   /* the other slides' size: 1.25em of 26px */
p { margin: 0.3em 0; }
.cols { display: grid; grid-template-columns: 1fr 1fr; gap: 28px; font-size: 15px; line-height: 1.35; margin: 10px 0 14px; }
.cols > div { border-left: 4px solid; padding: 2px 0 2px 14px; }
.cols > div.ar { border-color: #1a7f37; }
.cols > div.nar { border-color: #bc4c00; }
.cols h3 { font-size: 19px; margin: 0 0 0.25em; }
.cols ul { margin: 0; padding-left: 1.1em; }
.cols li { margin: 0.2em 0; }
table { font-size: 14px; line-height: 1.35; }
th, td { padding: 3px 9px !important; }
.note { font-size: 14px; }
</style>

## Runtimes: split by stage, not by shape

Each model does two kinds of work; each runtime is good at one.

<div class="cols">
<div class="ar">

### AR stage · llama.cpp

- Every step reads every weight: **bandwidth-bound**. Omni's Talker: 30ms a token, about 13 GB/s.
- Low bits cut the reads; KV cache and sampling are built in.
- ONNX leaves the KV cache and the loop to the host.

</div>
<div class="nar">

### Non-AR stage · ONNX

- Codec, flow, vocoder, diffusion head: convolutions over a chunk, **compute-bound**. Qwen3-TTS's codec, CPU → iGPU: 34.9 → 6.3ms a frame.
- Exported from PyTorch as is, onto GPU or NPU. ggml needs each written by hand.
- Low bits break it: INT4 left omni's vocoder at 0.03 correlation with PyTorch.

</div>
</div>

| shape | llama.cpp | ONNX Runtime | verdict |
| --- | --- | --- | --- |
| **Discrete AR** | Single-codebook: plain GGUF (Orpheus, Llasa, OuteTTS).<br>Multi-codebook: the depth loop by hand, dispatch-bound (20.0 → 20.5ms on the iGPU) | codec only | **llama.cpp** |
| **AR, then flow** | LM as is; flow and vocoder by hand.<br>cosyvoice.cpp: RTF 4.16 on CPU, noise on Vulkan | flow and vocoder export as is | **both**:<br>two memory pools, one GPU |
| **Continuous AR** | Embeddings in, hidden state out, the head a second graph; round trips every frame.<br>Ported: Pocket TTS, VoxCPM | LM step and unrolled head in one graph;<br>KV cache still outside | **neither fits** |

<p class="note">MNN runs both stages in one engine: omni ran on the phone at RTF 3.0, but its Vulkan output was noise.
<br>NPUs suit the fixed-shape stages, not bandwidth-bound decoding; untried here.</p>

---

<style scoped>
section { font-size: 18px; padding: 36px 56px; }
h2 { font-size: 32.5px; margin-bottom: 0.2em; }   /* the other slides' size: 1.25em of 26px */
p { margin: 0.3em 0; }
svg.fig { margin-top: 4px; }
.cols { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px 32px; font-size: 15px; line-height: 1.35; margin-top: 8px; }
.cols p { margin: 0; }
.variants { font-size: 15px; line-height: 1.35; margin-top: 12px; padding-top: 8px; border-top: 1px solid #d0d7de; }
</style>

## Qwen3-TTS: talker, code predictor, and codec, a frame at a time

Every 80ms frame, the talker emits one code, the code predictor 15 more, and the codec turns the 16 into 1920 samples.

<svg class="fig" viewBox="0 0 1250 350" role="img" aria-label="A voice file is made once from a recording: a 128-bin mel spectrogram, then the speaker encoder pools it over time into one vector. Text tokens and the voice file feed the input row, which is the sum of a text and a codec embedding. The talker emits code 0 and its hidden state; the code predictor emits codes 1 to 15; the codec decoder turns the 16 codes into 1920 samples of 24 kHz audio. The 16 code embeddings plus the next text token become the next input row.">
<defs>
<marker id="arrow-q3" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker>
<marker id="arrow-q3-loop" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#bc4c00"/></marker>
</defs>
<g fill="none" stroke="#57606a" stroke-width="1.5" stroke-dasharray="6 4">
<rect x="448" y="58" width="494" height="170" rx="14"/>
<rect x="1018" y="58" width="224" height="170" rx="14"/>
</g>
<g font-size="13" font-family="monospace" fill="#57606a">
<text x="462" y="80">qwen-talker-*.gguf</text>
<text x="1130" y="80" text-anchor="middle">qwen-tokenizer-12hz-*.gguf</text>
</g>
<g stroke-width="2">
<rect x="20" y="40" width="180" height="60" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
<rect x="20" y="112" width="180" height="86" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
<rect x="20" y="236" width="215" height="88" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
<rect x="255" y="110" width="150" height="90" rx="10" fill="#ddf4ff" stroke="#0969da"/>
<rect x="460" y="95" width="200" height="120" rx="10" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="730" y="95" width="200" height="120" rx="10" fill="#ddf4ff" stroke="#0969da"/>
<rect x="1030" y="95" width="200" height="120" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
</g>
<g stroke-width="1">
<rect x="965" y="80" width="28" height="8" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="965" y="90" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="100" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="110" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="120" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="130" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="140" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="150" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="160" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="170" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="180" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="190" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="200" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="210" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="220" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
<rect x="965" y="230" width="28" height="8" fill="#ddf4ff" stroke="#0969da"/>
</g>
<g text-anchor="middle" fill="#24292f">
<text x="110" y="66" font-size="20" font-weight="700">text</text>
<text x="110" y="88" font-size="14" fill="#57606a">BPE tokens, one a frame</text>
<text x="110" y="136" font-size="19" font-weight="700">voice file (.spk)</text>
<text x="110" y="156" font-size="13" fill="#57606a">1.7B · 2048 wide</text>
<text x="110" y="173" font-size="13" fill="#57606a">0.6B · 1024 wide</text>
<text x="110" y="190" font-size="13" fill="#57606a">one row in the prompt</text>
<text x="127" y="258" font-size="17" font-weight="700">speaker encoder</text>
<text x="127" y="277" font-size="13" fill="#57606a">a recording → 128-bin mel</text>
<text x="127" y="294" font-size="13" fill="#57606a">→ ECAPA-TDNN, pooled over time</text>
<text x="127" y="312" font-size="13" fill="#57606a">1.7B · 12M, 0.6B · 8.9M</text>
<text x="330" y="145" font-size="19" font-weight="700">input row</text>
<text x="330" y="168" font-size="14" fill="#57606a">text + codec</text>
<text x="330" y="186" font-size="14" fill="#57606a">embeddings, summed</text>
<text x="560" y="137" font-size="22" font-weight="700">Talker</text>
<text x="560" y="163" font-size="15" fill="#57606a">1.7B · 28 layers, 2048 wide</text>
<text x="560" y="187" font-size="15" fill="#57606a">0.6B · 28 layers, 1024 wide</text>
<text x="830" y="132" font-size="20" font-weight="700">Code predictor</text>
<text x="830" y="157" font-size="15" fill="#57606a">5 layers, 1024 wide</text>
<text x="830" y="178" font-size="15" fill="#57606a">1.7B · 175M</text>
<text x="830" y="199" font-size="15" fill="#57606a">0.6B · 142M</text>
<text x="979" y="70" font-size="14" fill="#57606a">16 codes</text>
<text x="1130" y="145" font-size="20" font-weight="700">Codec decoder</text>
<text x="1130" y="172" font-size="15" fill="#57606a">114M, causal</text>
<text x="1130" y="286" font-size="17" font-weight="700">24 kHz PCM</text>
<text x="695" y="146" font-size="14" fill="#57606a">hidden</text>
<g font-size="13" fill="#57606a" text-anchor="start">
<text x="122" y="226">once, about 4.5s</text>
<text x="20" y="344">A reference clip can also go in the prompt as codes, from a 48M encoder in the tokenizer file</text></g>
<text x="655" y="291" font-size="15" fill="#bc4c00">next row: the 16 code embeddings + the next text token</text>
</g>
<path d="M202,70 H228 V155" stroke="#57606a" stroke-width="2" fill="none"/>
<g stroke="#57606a" stroke-width="2" fill="none" marker-end="url(#arrow-q3)">
<line x1="110" y1="234" x2="110" y2="202"/>
<line x1="202" y1="155" x2="251" y2="155"/>
<line x1="407" y1="155" x2="456" y2="155"/>
<line x1="662" y1="155" x2="726" y2="155"/>
<line x1="932" y1="155" x2="961" y2="155"/>
<line x1="995" y1="155" x2="1026" y2="155"/>
<line x1="1130" y1="217" x2="1130" y2="262"/>
</g>
<path d="M979,240 V300 H330 V204" stroke="#bc4c00" stroke-width="2" stroke-dasharray="6 4" fill="none" marker-end="url(#arrow-q3-loop)"/>
<path d="M860,95 C860,60 800,60 800,91" stroke="#bc4c00" stroke-width="2" stroke-dasharray="6 4" fill="none" marker-end="url(#arrow-q3-loop)"/>
<text x="790" y="78" font-size="14" fill="#bc4c00" text-anchor="end">×15 a frame</text>
</svg>

<div class="cols">

**Talker**: a Qwen3 decoder with M-RoPE. Text can also go all in the prompt.

**Code predictor**: its cache resets every frame; serial, so dispatch-bound.

**Codec**: 16 codebooks of 2048; a transformer, 4× upsampling, a DAC decoder.

</div>

<p class="variants"><b>Same networks, different checkpoints.</b> Each has its own talker file; the tokenizer file is shared:
<br> - <i>Base</i> takes a voice file or a reference clip;
<br> - <i>CustomVoice</i>, built-in speakers;
<br> - <i>VoiceDesign</i>, a voice described in words, <b>with no 0.6B</b>.</p>

---

## Speech out: what is said, and who says it

<svg class="fig" viewBox="0 0 1250 425" role="img" aria-label="Reply text from a remote or local LLM goes through SpeakableStream, then either whole into a text buffer or cut into pieces by SentenceStream, into Qwen3-TTS, then the emitter and speech. The voice comes from above for Base, a voice file made once by the speaker encoder from a recording or a VoiceDesign sample, or from below for CustomVoice, a speaker name and an optional style. A pacer cuts pieces early when the listener is about to run out.">
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
<text x="515" y="63" font-size="19" font-weight="700" fill="#24292f">speaker encoder</text>
<text x="515" y="85" font-size="14" fill="#57606a">once, about 4.5s</text>
<rect x="650" y="38" width="230" height="60" rx="10" fill="#f6f8fa" stroke="#d0d7de" stroke-width="2"/>
<text x="765" y="63" font-size="19" font-weight="700" fill="#24292f">voice file (.spk)</text>
<text x="765" y="85" font-size="14" fill="#57606a">one row, 1024 or 2048 wide</text>
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
<rect x="650" y="185" width="230" height="110" rx="10" fill="#dafbe1" stroke="#1a7f37" stroke-width="2"/>
<text x="765" y="214" font-size="20" font-weight="700" fill="#24292f">Qwen3-TTS</text>
<text x="765" y="238" font-size="14" fill="#57606a">talker, code predictor, codec</text>
<text x="765" y="259" font-size="15" fill="#57606a">Base or CustomVoice</text>
<text x="765" y="280" font-size="15" fill="#57606a">0.6B or 1.7B</text>
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
<text x="430" y="152">one take</text>
<text x="430" y="256">pieces</text>
<text x="777" y="322" font-size="14">CustomVoice: built-in speakers</text>
</g>
<text x="777" y="140" font-size="15" fill="#bc4c00">Base: no style, so emotion and pace cannot be set</text>
<g stroke="#57606a" stroke-width="2" fill="none" marker-end="url(#arrow-out)">
<path d="M382,35 H405 V68 H426"/>
<path d="M382,100 H405 V68 H426"/>
<line x1="602" y1="68" x2="646" y2="68"/>
<line x1="765" y1="100" x2="765" y2="181"/>
<path d="M162,205 H180 V240 H196"/>
<path d="M162,275 H180 V240 H196"/>
<path d="M382,240 H405 V188 H426"/>
<path d="M382,240 H405 V292 H426"/>
<path d="M602,188 H625 V240 H646"/>
<path d="M602,292 H625 V240 H646"/>
<line x1="765" y1="333" x2="765" y2="299"/>
<line x1="882" y1="240" x2="916" y2="240"/>
<line x1="1072" y1="240" x2="1101" y2="240"/>
</g>
<path d="M995,282 V415 H515 V324" stroke="#bc4c00" stroke-width="2" stroke-dasharray="6 4" fill="none" marker-end="url(#arrow-pacer)"/>
<text x="780" y="408" font-size="15" fill="#bc4c00" text-anchor="middle">pacer: under 1s of audio left → cut now</text>
</svg>

<ul class="note">
<li>The voice file is one row in the prompt, so Base costs no more per frame. A real recording holds pieces tighter than a VoiceDesign sample: 1.31 against 1.68 semitones of spread.</li>
</ul>

---

<style scoped>
table { margin-left: auto; margin-right: auto; font-size: 18px; }
th, td { padding: 4px 14px !important; vertical-align: top; }
.sub { display: block; font-size: 13px; font-weight: 400; color: #57606a; }
tbody tr:nth-child(6) td, tbody tr:nth-child(10) td { border-top: 2px solid #8c959f; }
</style>

## Real-time factor (RTF)

Below 1 the speech keeps up with playback; above 1 a stream needs duration×(RTF−1) of buffer.

| `Q4_K_M` | Kirin 9020 (ARM)<span class="sub">6 threads; slows after ~30s; GPU unusable</span> | Core Ultra 9 185H (x86)<span class="sub">11 threads; ±15% day to day</span> | Arc iGPU (Vulkan)<span class="sub">RTF 1.64 if the chat model shares it</span> | RTX 3090 (CUDA)<span class="sub">a desktop card, for scale</span> |
| --- | --- | --- | --- | --- |
| **RTF** | | | | |
| 0.6B Base | 2.61 | 0.91–0.95 | 0.44 | 0.10 |
| 1.7B Base | 3.15 | 1.14 | **0.55** | 0.11 |
| 0.6B CustomVoice | 2.87 | 0.96–0.97 | 0.44–0.45<span class="sub">sounds worse; may be our code</span> | 0.10 |
| 1.7B CustomVoice | 3.64 | 1.12–1.13 | 0.55–0.57 | 0.11 |
| **Time per 80ms, 1.7B** | | | | |
| talker | — | 23.9ms | 17.3ms | 2.5ms |
| code predictor<span class="sub">set by the 16 codebooks:<br>no iGPU gain</span> | — | 20.0ms | 20.5ms | 5.3ms |
| codec | — | 34.9ms | 6.3ms | 0.5ms |
| reproduce | /tts → Benchmark | `tests/tts_rtf.py` | the same run | `--backend CUDA0` |

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
table { font-size: 15px; margin-left: auto; margin-right: auto; }
th, td { padding: 3px 10px !important; }
.rules { margin-top: 14px; padding-top: 8px; border-top: 1px solid #d0d7de; font-size: 15px; line-height: 1.35; }
.rules h3 { font-size: 19px; margin: 0 0 0.3em; }
.rules .grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 28px; }
.rules p { margin: 0; }
</style>

## The reading problem: speak it in pieces

A reply takes **seconds to write**, and a single `say()` waits for the last token; faster synthesis cannot recover that. `Reader` speaks while it is written: `begin()`, `feed(delta)` per token from any provider, `finish()`.

<svg class="fig" viewBox="0 0 1200 170" role="img" aria-label="Timeline: the text is cut into pieces as it arrives, and the pieces are spoken one after another with seams between them">
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
<text x="20" y="112" font-size="26" font-weight="700" fill="#24292f">Pieces</text>
<line x1="230" y1="85" x2="780" y2="85" stroke="#0969da" stroke-width="16" stroke-dasharray="8 5"/>
<g stroke="#24292f" stroke-width="2">
<line x1="290" y1="71" x2="290" y2="99"/>
<line x1="470" y1="71" x2="470" y2="99"/>
<line x1="780" y1="71" x2="780" y2="99"/>
</g>
<rect x="305" y="102" width="190" height="26" rx="4" fill="#1a7f37"/>
<rect x="499" y="102" width="300" height="26" rx="4" fill="#1a7f37"/>
<rect x="803" y="102" width="332" height="26" rx="4" fill="#1a7f37"/>
<g font-size="19" fill="#ffffff" text-anchor="middle">
<text x="400" y="121">1</text>
<text x="649" y="121">2</text>
<text x="969" y="121">3</text>
</g>
<g stroke="#bc4c00" stroke-width="3">
<line x1="497" y1="99" x2="497" y2="134"/>
<line x1="801" y1="99" x2="801" y2="134"/>
</g>
<line x1="230" y1="155" x2="1125" y2="155" stroke="#57606a" stroke-width="2" marker-end="url(#arrow-time)"/>
<text x="1135" y="162" font-size="20" fill="#57606a">time</text>
</svg>

<div class="cols">
<div>

### How it cuts

- `SentenceStream` cuts the text as it arrives; each piece is its own `say()`.
- **Piece 1 sets the wait**: 12–40 characters. A full stop ends it from 6 on, since <span style="white-space: nowrap">"你好。"</span> alone comes out clipped.
- **Later pieces**: 30–80 characters, cut early when under 1s of audio is left.
- **Cost**: the pace resets at every seam. The voice file holds the pitch: 1.31 semitones of spread, 3.09 without it.
- **Tolerant of a slow or bursty writer**: each piece is complete text before it is spoken.

</div>
<div>

### Measured in the app

The PC: speech on the Arc iGPU, the chat model on the CPU. First audio counts from sending, prefill included.

| chat model | decode, tok/s | first audio, s | starved, s |
| --- | --- | --- | --- |
| Qwen3.5-2B | 20.5–21.1 | 0.89–1.20 | 0.24–0.32 |
| Qwen3.5-4B | 4.0–8.1 | 1.17–2.73 | 0.20–0.33 |

A writer at 4–8 tok/s still leaves the speech starved under 0.33s.

</div>
</div>

<div class="rules">

### Rules found by listening, none visible in RTF

<div class="grid">

**Pin the seed** (default 101). A free seed is redrawn per `say()`: the pitch spread between pieces went from 10.1 Hz to 17.5.

**Merge a short last piece back**, up to 40 characters. Alone it stops while the vowel still sounds, at −34 dBFS; folded in, it decays to −68.

**Hold 2 frames past the first stop** (160ms). Without it, endings stopped mid-vowel; four frames sounded no different.

</div>
</div>

---

<style scoped>
section { font-size: 18px; padding: 36px 56px; }
h2 { font-size: 32.5px; margin-bottom: 0.2em; }   /* the other slides' size: 1.25em of 26px */
p { margin: 0.3em 0; }
svg.fig { margin-top: 6px; }
table { font-size: 15px; margin: 10px auto 14px; }
th, td { padding: 4px 10px !important; }
.note { font-size: 14px; }
.note li { margin: 0.15em 0; }
</style>

## Emotion: the same three axes, in and out

What is read from the user and what the reply is spoken with share one scale: −1, −0.5, 0, 0.5, 1 on each axis.

<svg class="fig" viewBox="0 0 1250 150" role="img" aria-label="The user's speech is read into valence, arousal and dominance: arousal from sound by prosody, valence and dominance from the words by the chat model. The chat model writes the reply and its own valence, arousal and dominance first. Qwen3-TTS speaks the reply's text with the user's voice file plus each axis times its direction.">
<defs>
<marker id="arrow-pad" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#57606a"/></marker>
</defs>
<g stroke-width="2">
<rect x="20" y="40" width="160" height="90" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
<rect x="240" y="40" width="250" height="90" rx="10" fill="#fff1e5" stroke="#bc4c00"/>
<rect x="550" y="40" width="200" height="90" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
<rect x="810" y="40" width="270" height="90" rx="10" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="1130" y="40" width="100" height="90" rx="10" fill="#f6f8fa" stroke="#d0d7de"/>
</g>
<g text-anchor="middle" fill="#24292f">
<text x="100" y="80" font-size="20" font-weight="700">user speech</text>
<text x="100" y="104" font-size="14" fill="#57606a">microphone</text>
<text x="365" y="72" font-size="20" font-weight="700">the user's V, A, D</text>
<text x="365" y="96" font-size="14" fill="#57606a">A: prosody/, from the sound</text>
<text x="365" y="116" font-size="14" fill="#57606a">V, D: chat model, from the words</text>
<text x="650" y="72" font-size="20" font-weight="700">chat model</text>
<text x="650" y="96" font-size="14" fill="#57606a">writes the reply,</text>
<text x="650" y="116" font-size="14" fill="#57606a">its own V, A, D first</text>
<text x="945" y="72" font-size="20" font-weight="700">Qwen3-TTS</text>
<text x="945" y="96" font-size="14" fill="#57606a">the user's voice file</text>
<text x="945" y="116" font-size="15" fill="#57606a">+ V·d<tspan font-size="11" dy="3">V</tspan><tspan dy="-3"> + A·d</tspan><tspan font-size="11" dy="3">A</tspan><tspan dy="-3"> + D·d</tspan><tspan font-size="11" dy="3">D</tspan></text>
<text x="1180" y="80" font-size="20" font-weight="700">speech</text>
<text x="1180" y="104" font-size="14" fill="#57606a">24 kHz</text>
</g>
<g font-size="13" fill="#57606a" text-anchor="middle">
<text x="520" y="76">V, A, D</text>
<text x="780" y="76">reply</text>
<text x="780" y="104">V, A, D</text>
</g>
<g stroke="#57606a" stroke-width="2" fill="none" marker-end="url(#arrow-pad)">
<line x1="182" y1="85" x2="236" y2="85"/>
<line x1="492" y1="85" x2="546" y2="85"/>
<line x1="752" y1="85" x2="806" y2="85"/>
<line x1="1082" y1="85" x2="1126" y2="85"/>
</g>
</svg>

| axis | from | to | the user: read from | the reply: made by |
| --- | --- | --- | --- | --- |
| **Valence** V | unpleasant | pleasant | the words: the chat model reads the transcript | direction d<sub>V</sub> |
| **Arousal** A | deactivated | activated | the sound: pitch, loudness, continuity; no model | direction d<sub>A</sub>, checked by prosody/ |
| **Dominance** D | acted upon | in control | the grammar: who is the subject of the verbs | direction d<sub>D</sub> |

<ul class="note">
<li>Heard: a style's voice file changes the reading, at no cost per frame; on the user's own voice, how far it can go is capped by the speaker (next slide). Not yet: fitting d<sub>V</sub>, d<sub>A</sub>, d<sub>D</sub> from eight labeled CustomVoice instructions.</li>
<li>Checked offline: A by prosody/; V and D by audeering's dimensional model, research only.</li>
<li>A named style is a point plus delivery: 郑重 = (0, 0, 1), slower. emotion2vec's labels are points too.</li>
<li>CustomVoice takes the nearest instruction instead.</li>
<li>To the user: only change from their own baseline, never a verdict.</li>
</ul>

---

<style scoped>
section { font-size: 18px; padding: 36px 56px; }
h2 { font-size: 32.5px; margin-bottom: 0.2em; }   /* the other slides' size: 1.25em of 26px */
p { margin: 0.3em 0; }
svg.fig { margin-top: 4px; }
table { font-size: 15px; margin: 8px auto 12px; }
th, td { padding: 4px 10px !important; }
.note { font-size: 14px; }
.note li { margin: 0.15em 0; }
</style>

## Style on the user's voice: the speaker caps the step

CustomVoice reads one passage calm and in each style; style − calm, in the speaker encoder's space, is the direction. Base reads with 01_female's voice file + α × direction.

<svg class="fig" viewBox="0 0 1250 160" role="img" aria-label="Distances in voice-file space on a line from 0 to 6. The cleaned mixed directions move the voice file 1.1 to 1.4 at alpha 1, and the speaker holds; 2.2 to 2.8 at alpha 2, and the speaker changes. Serena's directions move it 2.0 to 3.3 at alpha 2, borderline. The two closest built-in speakers are 2.85 apart, the median between two is 4.72, and serena is 5.00 from 01_female.">
<g stroke="#8c959f" stroke-width="1.5" stroke-dasharray="4 4">
<line x1="606" y1="24" x2="606" y2="112"/>
<line x1="965" y1="24" x2="965" y2="112"/>
<line x1="1018" y1="24" x2="1018" y2="112"/>
</g>
<g font-size="14" fill="#57606a">
<text x="600" y="18" text-anchor="end">two closest speakers, 2.85</text>
<text x="959" y="18" text-anchor="end">median between two speakers, 4.72</text>
<text x="1024" y="18">serena to 01_female, 5.00</text>
</g>
<g stroke-width="2">
<rect x="271" y="32" width="57" height="14" rx="3" fill="#dafbe1" stroke="#1a7f37"/>
<rect x="482" y="56" width="115" height="14" rx="3" fill="#fff1e5" stroke="#bc4c00"/>
<rect x="443" y="80" width="249" height="14" rx="3" fill="#fff8f2" stroke="#bc4c00" stroke-dasharray="5 3"/>
</g>
<g font-size="15" fill="#24292f">
<text x="338" y="44">α = 1, cleaned mix: speaker holds</text>
<text x="616" y="68">α = 2, cleaned mix: speaker changes</text>
<text x="702" y="92">α = 2, serena's: borderline</text>
</g>
<line x1="60" y1="112" x2="1210" y2="112" stroke="#57606a" stroke-width="2"/>
<g stroke="#57606a" stroke-width="2">
<line x1="60" y1="112" x2="60" y2="118"/>
<line x1="252" y1="112" x2="252" y2="118"/>
<line x1="443" y1="112" x2="443" y2="118"/>
<line x1="635" y1="112" x2="635" y2="118"/>
<line x1="827" y1="112" x2="827" y2="118"/>
<line x1="1018" y1="112" x2="1018" y2="118"/>
<line x1="1210" y1="112" x2="1210" y2="118"/>
</g>
<g font-size="14" fill="#57606a" text-anchor="middle">
<text x="60" y="134">0</text>
<text x="252" y="134">1</text>
<text x="443" y="134">2</text>
<text x="635" y="134">3</text>
<text x="827" y="134">4</text>
<text x="1018" y="134">5</text>
<text x="1210" y="134">6</text>
</g>
<text x="1210" y="154" font-size="14" fill="#57606a" text-anchor="end">how far the voice file moved (Euclidean)</text>
</svg>

| direction from | length | α = 1 | α = 2 |
| --- | --- | --- | --- |
| serena, vivian, uncle_fu, two breathy caring takes in | 1.2–2.3 | caring and sorry change the speaker | – |
| the same, outlying pairs dropped | 1.1–1.4 | holds; sorry −0.7 st, fast +0.65 c/s | changes |
| mostly serena's own pairs | 1.0–1.7 | holds; the styles faint | slight change, borderline |

<ul class="note">
<li>serena is the built-in speaker nearest 01_female (5.00), uncle_fu the farthest (6.96): a direction from a nearer speaker carries less identity. Two variants, one listener: a lead.</li>
<li>Seeds alone move pace up to 0.7 c/s and pitch up to 1.6 st, so the α = 1 changes are near the noise.</li>
<li>Next: directions from the built-in speakers nearest the user's voice; or mode B with the user's voice file and another speaker's styled reading.</li>
</ul>
