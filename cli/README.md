# cli

Desktop tools for trying the core on a workstation, without a phone. They are
not shipped; each one drives a part of the core so a change can be checked
from a shell.

| Tool | What it does | Source |
|---|---|---|
| `bodyclock-cli` | Chats with any OpenAI-compatible provider through the C ABI, the same path a host app uses | [main.cpp](main.cpp) |
| `bodyclock-asr` | Transcribes WAV files with the on-device SenseVoice model | [asr.cpp](asr.cpp) |

## Building

The tools are targets of the root CMake project, so they have no build script
of their own. The root script builds the core plus these tools into `build/`:

```
build.cmd build        # Windows
./build.sh build       # macOS / Linux
```

With no arguments either script prints its commands and builds nothing.

`bodyclock-asr` is always built, but it can only transcribe when speech
recognition is compiled in. That needs ggml, which the `asr` argument builds
from a llama.cpp checkout (by default one next to this repo; set
`YOAI_LLAMA_SRC` in `.env` otherwise -- see [.env.example](../.env.example)):

```
build.cmd asr
./build.sh asr
```

CMake caches the setting, so adding `asr` to a `build/` that was configured
without it does nothing. The script notices, stops, and tells you to run
`build.cmd clean asr` (or `./build.sh clean asr`) instead.

## bodyclock-cli

```
bodyclock-cli [options] ["question"]

  --base-url URL   OpenAI-compatible API root, no /chat/completions
  --key KEY        bearer token
  --model NAME     model to send in the request
  --system TEXT    system prompt
  --no-thinking    drop reasoning output instead of dimming it
  --usage          print the token/cost summary after each turn
  --debug          verbose core logging
```

`--base-url`, `--key` and `--model` fall back to `BODYCLOCK_BASE_URL`,
`BODYCLOCK_API_KEY` and `BODYCLOCK_MODEL`. Pass the key through the
environment: a flag ends up in shell history and in the process list.

With a question it runs one turn and exits. Without one it reads turns from
stdin and keeps the transcript, so multi-turn context is exercised too. The
root [README](../README.md) has provider examples.

## bodyclock-asr

```
bodyclock-asr <model.gguf> [options] <audio.wav> [more.wav ...]

  -t N        encoder threads (default 4)
  --no-itn    no inverse text normalization: numbers stay spoken words
  --tokens    list every decoded token with its timestamp
  -q          print only the text, one line per file
  --debug     verbose core logging
```

The model is a SenseVoiceSmall GGUF (`q8` is 254 MB). It is loaded once, and
every file after it goes through the same instance.

```
build\bodyclock-asr.exe sensevoice-small-q8.gguf clip.wav

clip.wav
  text     这张照片看起来拍的很不错，能告诉我这是在哪里拍的吗？
  tags     language=zh emotion=HAPPY event=Speech itn=yes
  timing   5.12 s of audio in 0.24 s (20.9x real time)
```

- **Audio.** WAV, 16-bit PCM or 32-bit float. Only the first channel is used,
  with no downmix. A device records at 16 kHz; any other rate is resampled by
  the tool, linearly and without an anti-aliasing filter, and the output
  prints a `note` line when that happens. Treat a transcript of a resampled
  file as close to, not identical with, what a device would produce.
- **Tags.** `language`, `emotion` and `event` are the model's own labels,
  passed through unchanged. The app does not read emotion from them; see
  `src/voice/asr/sensevoice.hpp` for why.
- **`--no-itn`.** With ITN on, "five hundred and sixty-seven" comes back as
  "567", which is right for display and wrong for anything that counts
  syllables.
- **`--tokens`.** One line per token: time into the clip, vocabulary id and
  piece. The tag tokens have no time and show `-`. Tokens removed by the
  stray-capital filter are listed and marked `dropped`.
- **Exit code.** 0 when every file was transcribed, 1 when any file failed or
  the build has no recognition engine, 2 for a usage error.
