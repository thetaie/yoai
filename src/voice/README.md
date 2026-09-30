# src/voice

Everything in the core that listens to audio or speaks it. The reasons for this
split -- recognition outside the language model, arousal from the signal rather
than from a model's emotion label, and why no single omni pass does all of it --
are in [design/voice.md](../../design/voice.md). This file only says what is
where.

## Layout

Engine-backed files are named after the model they run (`qwen25_omni`,
`qwen3_tts`). The class names (`Omni`, `Qtts`) and the C ABI in
`src/yoai.h` are unchanged. Everything is in namespace `yoai::voice`.

### algorithms/ -- signal processing with no model behind it

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [fft](algorithms/fft.hpp) | The radix-2 FFT everything here shares. Its last bits are pinned by `fbank_funasr`, so it is never changed for another user | none | always built |
| [fbank_funasr](algorithms/fbank_funasr.hpp) | `FunAsrFbank`: the 80-bin log-mel front end SenseVoice was trained on, plus low-frame-rate stacking (7 frames, hop 6, 560 wide), matching FunASR's output bit for bit | none | always built |
| [resampler](algorithms/resampler.hpp) | `Resampler`: fixed-ratio polyphase FIR, for the echo canceller's reference signal only (24 kHz playback to the 16 kHz capture) | none | always built |
| [echo_canceller](algorithms/echo_canceller.hpp) | `EchoCanceller`: what the speaker played taken back out of the microphone. The linear stage only -- a partitioned frequency-domain adaptive filter with Valin's step size; no residual suppression or delay estimation yet | none | always built |

### asr/ -- audio in, text out

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [sensevoice](asr/sensevoice.hpp) | SenseVoiceSmall: SAN-M encoder, greedy CTC, tokens with 60 ms timestamps, language / emotion / event tags | ggml | `YOAI_ASR` |

### prosody/ -- what the voice says about the speaker, from the signal alone

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [prosody](prosody/prosody.hpp) | Pitch, loudness and continuity per utterance, scored against the recording's own distribution; derives arousal | none | always built |

### omni/ -- audio in, a reply and the model's own voice out

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [qwen25_omni](omni/qwen25_omni.hpp) | Qwen2.5-Omni: audio in, a text reply and the model's own voice out | MNN | `YOAI_MNN` |

### tts/ -- text in, speech out

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [qwen3_tts](tts/qwen3_tts.hpp) | Qwen3-TTS: text in, speech out, with a voice described in words, a named speaker, or a reference recording | qwentts.cpp | `YOAI_QTTS` |
| [reader](tts/reader.hpp) | A passage read aloud on top of Qtts: ordered streaming across engines, seam shaping, incremental `begin` / `feed` / `finish` | via Qtts | always built |

### text/ -- streamed text cut into pieces that can be acted on one by one

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [sentence_stream](text/sentence_stream.hpp) | `split_sentences` for a finished passage and `SentenceStream` for one still being written: where a cut may fall, and when it can no longer move. No audio and no dependency on the rest of `voice/`; its defaults (`SplitOptions`) are tuned for speech, and the comments say what each was measured against | none | always built |
| [speakable_stream](text/speakable_stream.hpp) | `speakable` for a finished reply and `SpeakableStream` for one still being written: markdown and emoji dropped, line breaks turned into full stops, ahead of the cutter | none | always built |

## Audio formats

- **Input** is mono float in [-1, 1] at **16 kHz**: `SenseVoice::transcribe`
  and `Omni::ask`. `prosody` takes any rate and any scale.
- **Output** is mono float at **24 kHz**: every sink in `omni/` and `tts/`.
  Nothing in the buffer says which rate it is, so a player has to be told.
- Nothing that is analyzed or spoken is resampled in the core, on purpose.
  Recognition and prosody take 16 kHz from the capture, and a Qtts reference
  WAV must be mono 16-bit PCM at exactly 24 kHz; anything else is refused, not
  converted.
- The one exception is `algorithms/resampler`, and it is for the echo
  canceller's reference signal: what was played, brought to the capture's rate
  so it can be subtracted. An error there becomes a little residual echo, never
  a change to what the speaker said.

## Build flags and stubs

Each engine compiles to a stub when its flag is off. `available()` returns
false, `load()` fails with a logged reason, and every symbol still links, so a
host can check at runtime before offering the feature. `algorithms/`,
`prosody/` and `text/` have no engine and work in every build.

| Flag | Needs |
|---|---|
| `YOAI_ASR` | ggml archives under `LLAMA_CPP_DIR` |
| `YOAI_MNN` | `-DMNN_DIR=<sdk>`, including the rapidjson libMNN was built with. `YOAI_MNN_VULKAN` if that MNN has the Vulkan backend |
| `YOAI_QTTS` | `-DQTTS_DIR=<sdk>` with `qwen.h` and `qwen-core`. Vulkan tuning is chosen at load time (see `apply_vulkan_env` in `tts/qwen3_tts.cpp`) |

## Threading

- `SenseVoice`, `Omni` and `Qtts`: one call at a time per instance. Every
  `load` and every inference call blocks, so none of them belong on the UI
  thread.
- The `Qtts` sink runs on qwentts.cpp's compute worker. It must return quickly
  and must not call back into `Qtts`.
- The `Reader` sink runs on the Reader's own emitter thread. It may be slow;
  that delays playback but does not stall generation.
- Returning false from any sink cancels. That is not an error: the call returns
  true with whatever was produced.
- `SentenceStream`, `Resampler` and `EchoCanceller` are not thread-safe; each
  has one writer. The canceller takes the reference and the microphone in one
  call, in lockstep; pairing a playback thread with a capture thread is not
  in it yet.

## Tests

`yoai-sensevoice-test`, `yoai-prosody-test`, `yoai-resampler-test`,
`yoai-echo-canceller-test` and `yoai-reader-test` are plain executables, not
registered with CTest; run them directly from the build directory. With no
arguments the reader test checks `split_sentences` and `SentenceStream`; given
a talker and a codec GGUF it also reads a passage aloud. The prosody test needs
a WAV. The resampler test checks tones and streaming with no arguments; given
`<in.wav> <out.wav> [rate]` it converts a file, lined up with the input, for
listening. The echo canceller test runs white noise through synthetic rooms
with no arguments; given a reply WAV, a talker WAV and a directory, it plays
the reply into a synthetic room, talks over it, and writes the microphone, the
output and what is left of the echo.

## Timing

Durations are wall clock from `steady_clock`, never `clock()`. The engines are
multithreaded and `clock()` adds up time across threads: on the first phone run
it reported 87.9 s for work that took 18.2 s.
