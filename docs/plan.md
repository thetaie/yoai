# Plan

What is still to do, by module, as laid out on the first slide of
[slides.md](slides.md): one table for the main line, which ends in Qwen3-TTS,
and one for the two Qwen-Omni paths. The reasoning stays in the linked
section; a row leaves this file when it is done or dropped.

**To build**: decided, not built. **To fix**: built, and wrong. **To measure**:
a measurement decides it. **Open**: not decided. **Parked**: not being pursued
now. **Known limit**: accepted, no plan.

## The Qwen3-TTS line

SenseVoice in, a chat model, Qwen3-TTS out; left to right along the slide.

| Module | Item | Status | Next step | See |
| --- | --- | --- | --- | --- |
| **Microphone** | — | | | |
| **AEC** | Echo it cannot remove: loudspeaker distortion, paths over 256ms; ASR still transcribes echo 20 dB down | Known limit | — | [slides](slides.md#aec-take-the-reply-out-of-the-microphone) |
| **ASR** | ITN off gives the right speaking rate and drops all punctuation | Open | Two passes, or a syllable count that expands digits | [voice](voice.md#store-measurements-not-conclusions) |
| **Barge-in** | Pause on a voice; judge "answerable alone?" and "adds or corrects?" on the words; history keeps what was played | To build | — | [slides](slides.md#barge-in-a-second-sentence-mid-reply) |
| | Whether Qwen3.5-2B holds to a wait marker | To measure | Else a small classifier | [slides](slides.md#barge-in-a-second-sentence-mid-reply) |
| | A KV snapshot before sentence 1, so a merge does not prefill the whole prompt again | To build | — | [slides](slides.md#barge-in-a-second-sentence-mid-reply) |
| **Emotion in** | One voice per recording: check each utterance is the user | To build, deferred | Measure with sherpa-onnx and CAM++ first, no code | [voice](voice.md#one-voice-per-recording-assumed-and-not-checked) |
| | Diarization | Open | Only if the app records the user with others: a product question | [voice](voice.md#verification-not-diarization) |
| | Per-utterance measurements stored, with the `seg_version` that cut them | To build | — | [voice](voice.md#store-measurements-not-conclusions) |
| | Unpredictability and intensity | To build | Kept out of display until checked the way arousal was | [voice](voice.md#the-dimensions-past-the-three) |
| | Dominance from the sound: terminal contour, disfluency rate | To build | — | [voice](voice.md#dominance-is-the-weakest-of-the-three-and-that-changes-how-it-is-used) |
| | Voice quality: jitter ~6.5%, HNR ~4 dB in the prototype | To fix | Glottal pulse marking; not ported until then | [voice](voice.md#what-else-the-voice-carries) |
| | Cross-day baseline | Parked | If built, condition on time of day | [voice](voice.md#the-cross-day-version-is-much-harder-and-is-not-being-built-yet) |
| **Chat model** | The 4B decodes at about half its bench speed in the app, 5.7–6.7 against 11.4 tokens a second | Open | Unexplained; the 2B shows no gap | [voice](voice.md#cpu) |
| **Emotion out** | The chat model states V, A, D ahead of the reply; the reader takes them off | To build | — | [voice](voice.md#speaking-on-the-same-three-axes) |
| | Fit d_V, d_A, d_D over eight labeled CustomVoice instructions | To build | — | [voice](voice.md#speaking-on-the-same-three-axes) |
| | A style on the user's own voice | To measure | Directions from the nearest built-in speakers, or mode B | [voice](voice.md#the-first-two-measured-a-style-rides-on-the-voice-file) |
| **TTS** | A long reply sounds read aloud | To build | Mode B on the previous piece's tail; a per-piece time-stretch | [voice](voice.md#why-a-long-reply-sounds-read-aloud-and-what-is-being-done) |
| | `Q8_0` against `Q4_K_M` | To measure | On Base; back to back on the phone | [voice](voice.md#q4-moves-the-talker-by-a-fifth-and-the-codec-not-at-all) |
| | A 3–5s reference for mode B | To measure | — | [voice](voice.md#cosyvoice3-was-built-and-measured-and-the-reference-length-is-the-result) |
| | "……", "——", written fillers, final particles | To measure | Listen | [voice](voice.md#the-text-is-the-one-channel-that-is-free) |
| | The fused snake kernel, lost with the move to llama.cpp's ggml | To measure | Decides whether it returns as a patch | [voice](voice.md#two-things-the-port-settled-and-neither-stayed-settled) |
| | Other TTS | To measure | VoxCPM first; MOSS-TTS-Realtime's codec cost | [voice](voice.md#which-to-look-at-first) |
| **Speaker** | — | | | |

## The Qwen-Omni paths

One model takes the audio and speaks the reply itself; top to bottom along the
slide.

| Module | Item | Status | Next step | See |
| --- | --- | --- | --- | --- |
| **Qwen3-Omni-30B-A3B**, remote, kept | A remote provider path: nothing in the core sends audio to a provider yet | Open | — | [slides](slides.md#voice-streaming) |
| | It hears tone: shown on one clip at three tape speeds, not on real emotional states | To measure | One person recording one sentence in several real states | [voice](voice.md#the-larger-model-was-run-and-it-hears-clearly) |
| **Qwen2.5-Omni-3B**, on the device, replaced | Adopting it at all | Parked | Only if the Talker nears RTF 1; what is left is the NPU backend or a smaller Talker | [voice](voice.md#whether-to-adopt-it) |
| | The Talker at 4 bits | Parked | Needs the 11 GB checkpoint | [voice](voice.md#on-a-desktop-the-split-was-measured-and-it-kills-the-diffusion-argument) |
| | token2wav on Vulkan: the device survives and the output is noise | Parked | At least one wrong operator in the buffer path | [voice](voice.md#vulkan-for-the-vocoder-two-real-bugs-found-and-it-still-does-not-work) |
| | Whether MNN's quantization transcribes as well as the GGUF | Parked | The reference clip is the one that crashes | [voice](voice.md#above-rtf-1-a-player-must-not-stream-at-all) |
| | The desktop RTF 3 is from `clock()`, not a wall clock | Parked | Re-time it; the 3.0 against 3.7 comparison rests on it | [voice](voice.md#the-real-defects-and-one-that-is-not) |
