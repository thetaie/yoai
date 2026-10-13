# cli

Desktop tools for trying the core on a workstation, without a phone. They are
not shipped; each one drives a part of the core so a change can be checked
from a shell.

| Tool | What it does | Source |
|---|---|---|
| `yoai-cli` | Chats with any OpenAI-compatible provider through the C ABI, the same path a host app uses | [main.cpp](main.cpp) |
| `yoai-asr` | Transcribes WAV files with the on-device SenseVoice model | [asr.cpp](asr.cpp) |
| `yoai-llama` | Chats with a GGUF, or embeds text with one, on llama.cpp | [llama.cpp](llama.cpp) |

The speech server, `yoai-server`, is not here: it is a host of its own, with
its own build; see [examples/server/README.md](../examples/server/README.md).

## Building

The tools are targets of the root CMake project, so they have no build script
of their own. The root script builds the core plus these tools into `build/`:

```
python build.py        # python3 on macOS / Linux
```

`--help` lists the options.

`yoai-asr` is always built, but it can only transcribe when speech
recognition is compiled in. That needs ggml, which the `asr` argument builds
from a llama.cpp checkout (by default `third/llama.cpp`, which the first
build fetches; set `LLAMA_SRC` in `.env` otherwise -- see
[.env.example](../.env.example)):

```
python build.py asr
```

`yoai-llama` is always built too, and needs the on-device engine to do
anything. `llama` builds libllama from the same checkout, on the same ggml --
longer than `asr`, since it is the whole engine:

```
python build.py llama
```

`tts` compiles speech output (Qwen3-TTS) into the core the same way, without
the desktop app. It needs a qwentts.cpp checkout as well as llama.cpp, whose
ggml it compiles; both default to checkouts in `third/` (`QTTS_SRC`,
`LLAMA_SRC` in `.env` move them). It uses the GPU through Vulkan when `VULKAN_SDK` is set and
otherwise builds for the CPU, with a warning. No tool here drives it yet;
`yoai-reader-test` with a talker and a codec GGUF reads a passage aloud.

```
python build.py tts
```

CMake caches which of these are on, so adding `asr`, `llama` or `tts` to a
`build/` that was configured without it does nothing. The script notices,
stops, and tells you to run `python build.py clean asr` (or `clean llama`,
`clean tts`) instead. They combine: `python build.py clean asr llama tts`.

## yoai-cli

```
yoai-cli [options] ["question"]

  --base-url URL   OpenAI-compatible API root, no /chat/completions
  --key KEY        bearer token
  --model NAME     model to send in the request
  --system TEXT    system prompt
  --no-thinking    drop reasoning output instead of dimming it
  --usage          print the token/cost summary after each turn
  --debug          verbose core logging
```

With a question it runs one turn and exits. Without one it reads turns from
stdin and keeps the transcript, so multi-turn context is exercised too. The
root [README](../README.md) has provider examples.

## yoai-asr

```
yoai-asr <model.gguf> [options] <audio.wav> [more.wav ...]

  -t N        encoder threads (default 4)
  --no-itn    no inverse text normalization: numbers stay spoken words
  --tokens    list every decoded token with its timestamp
  -q          print only the text, one line per file
  --debug     verbose core logging
```

The model is a SenseVoiceSmall GGUF (`q8` is 254 MB). It is loaded once, and
every file after it goes through the same instance.

```
build\yoai-asr.exe sensevoice-small-q8.gguf clip.wav

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

## yoai-llama

```
yoai-llama chat  <model.gguf> [options] ["question"]
yoai-llama embed <model.gguf> [options] <text> [more text ...]

common
  -t N             threads (default 4; <= 0 means every core)
  --gpu N          layers to offload to a GPU (default 0; 999 for all)
  --debug          verbose core logging

chat
  --system TEXT    system prompt
  --thinking       let the model reason before answering (off by default)
  --grammar FILE   constrain every reply with a GBNF grammar; greedy decoding
  --ctx N          context ceiling in tokens (default 32768)
  --predict N      most tokens per reply (default 8192)
  --stats          prefill and decode speed after each turn

embed
  --query TEXT     rank the texts by similarity to TEXT instead
  --task TEXT      the instruction put in front of the query
  --ctx N          longest text in tokens (default 2048)
  --vectors        print every vector in full, one JSON array per line
```

It drives `llm::LocalClient` and `llm::LocalEmbedder` directly, through the
`Chat` and `Embedder` interfaces, rather than the C ABI: embedding has no C
entry points yet, and the numbers worth seeing here -- prefill and decode
speed, how similar two texts are -- are what the ABI leaves out.

**chat** with a question runs one turn and exits. Without one it reads turns
from stdin and keeps the transcript, so the KV-cache reuse across turns is
exercised too. `/new` starts a new conversation; Ctrl-D quits (Ctrl-Z, Enter on
Windows).

```
build\yoai-llama.exe chat Qwen3.5-2B-Q4_K_M.gguf --stats "Say hello in one word."
Hi!
[stats] prompt 18 tokens (0 reused), prefill 349 ms (51.6 tok/s), 2 tokens decoded (29.3 tok/s), ctx 4096, threads 8
```

**embed** prints the first components of each vector and every pair's
similarity. With `--query` it ranks the texts against the query instead,
embedding the query as a query -- with the model's instruction in front -- and
the texts as documents, which is how search uses it:

```
build\yoai-llama.exe embed Qwen3-Embedding-0.6B-Q8_0.gguf --query "What did I eat?" ^
    "Had beef noodles for lunch" "Ran five kilometers"
0.5984  Had beef noodles for lunch
0.3620  Ran five kilometers
```

- **Model.** A GGUF. For embed it has to be one llama.cpp pools, which is what
  an embedding model is; a chat model is refused with a message saying so.
- **Similarity.** The dot product of two unit vectors, i.e. their cosine.
- **Chinese on Windows.** Arguments, typed input and output all go through the
  console's wide API when there is a console, so Chinese text reaches the model
  and the screen intact; see [console.hpp](console.hpp).
- **Exit code.** 0 on success, 1 when the model will not load, a turn fails or
  the build has no llama.cpp, 2 for a usage error.
