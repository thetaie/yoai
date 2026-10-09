# YoAI

Your conversations live on your device, and models are called with your own API
keys — there is no YoAI account and no YoAI server.

HarmonyOS first, Android next.

This is the v0 skeleton: one C++ core, one HarmonyOS app, remote
OpenAI-compatible models working end to end — and, on HarmonyOS, GGUF models
running on the device itself through llama.cpp. The core keeps the user's day
in its own SQLite database, and has the pieces for searching it by meaning:
on-device text embedding and vector search, not yet wired to the timeline.

The shape of the core is what keeps each of these an addition rather than a
rewrite. A model is a `Chat` or an `Embedder`, and where it runs — llama.cpp,
MNN, or an HTTP API — is decided when it is built and nowhere above; see
[src/llm/README.md](src/llm/README.md).

## What is here

```
src/                  the core -- a static library plus a C ABI
  yoai.h                the whole public surface, a C ABI every host links
  yoai.cpp              its implementation
  llm/                  chat and embedding models, on device or remote
  store/                yoai.db: the one connection, the timeline, the vectors
  voice/                speech in and out
  utils/                what is too small for a directory of its own:
                        logging, HTTP + TLS, bundles, a memory benchmark
cli/                  a desktop harness -- run the core without a device
examples/             the hosts, each linking the core as it is
  harmony/              the HarmonyOS app (ArkTS) + its NAPI bridge
  android/              a measurement bench (Compose) + its JNI bridge
  desktop/              the desktop client (Electron) + its Node-API bridge
```

### The core is a library, not a server

There is no port to bind, no config file, and no session state. The host app
owns settings and conversation history, tells the core which models the user
has keys for, and asks it to run a turn:

```c
yoai_provider_set("zhipu/glm-4.7-flash",
                  "https://open.bigmodel.cn/api/paas/v4",
                  api_key, "glm-4.7-flash", NULL);

yoai_chat("zhipu/glm-4.7-flash", messages_json,
          /*system_prompt=*/NULL, /*grammar=*/NULL, on_event, user_data);
```

An on-device model registers with `yoai_local_set` instead — a GGUF file or an
MNN export directory, and the path decides which engine runs it — and is then
chatted with through the same `yoai_chat`.

The one thing the core persists itself is the user's own data: the day
timeline, in a database in a directory the host names, because its rules are
the same on every platform and easy to get subtly wrong.

Two consequences worth stating plainly, because they are the design and not an
implementation detail:

- **Keys never touch our disk.** The registry is process memory. On HarmonyOS the
  key comes out of the platform's encrypted asset store at the moment it is
  handed to the core, and the app refills the registry after every cold start.
- **The host owns history.** `yoai_chat` takes the whole transcript every turn.
  One place decides what to keep and what to trim, so two policies cannot
  disagree.

`yoai_chat` blocks and streams through a callback on the calling thread;
returning 0 from that callback cancels the turn. Everything above it — a worker
thread per turn, a threadsafe function back to the UI — is the host's business,
which is why the ABI stays this small.

## Building

### Desktop (to exercise the core)

Needs git, CMake and a C++11 compiler. curl and OpenSSL come from vcpkg on every
platform: the first run clones it into `third/vcpkg` at the baseline
`vcpkg.json` pins, and builds the two for this machine, which takes a few
minutes once. RapidJSON, SQLite and sqlite-vec are vendored in `third/`. On
Linux vcpkg has prerequisites of its own — on Debian/Ubuntu,
`apt install build-essential curl zip unzip tar pkg-config`; on macOS, the Xcode
command-line tools.

```
python build.py      # python3 on macOS / Linux
```

Every build script is Python 3.8 or newer, standard library only, and the same
file on every platform.

A first build also fetches the llama.cpp and qwentts.cpp checkouts into
`third/`, at the commits `third/manifest.json` pins, and adds speech
recognition and speech output from them — so it needs git and the network that
once. [third/README.md](third/README.md) says why they live there and how to
move them.

`--help` lists the options: `asr` for speech recognition, `llama` for the
on-device engine (chat and embedding on a GGUF), `tts` for speech output, and
`clean` to start over. They combine: `python build.py clean asr llama tts`.

Paths that differ per machine -- Visual Studio, vcpkg, the llama.cpp checkout
-- go in `.env` at the root, which git ignores and every C/C++ build script
reads (root, `examples/harmony/`, `examples/desktop/`, `examples/android/`, `third/sync.py`) by the
rules in the root `build.py`. Copy
[.env.example](.env.example); it lists every setting.

Then talk to a real provider without a phone in the loop:

```
rem Windows
set YOAI_API_KEY=...
build\yoai-cli.exe --base-url https://open.bigmodel.cn/api/paas/v4 --model glm-4.7-flash
```

```
# macOS / Linux
export YOAI_API_KEY=...
./build/yoai-cli --base-url https://open.bigmodel.cn/api/paas/v4 --model glm-4.7-flash
```

With no question argument it goes interactive and keeps the transcript, so
multi-turn context is exercised too. `--usage` prints the token summary,
`--debug` turns on core logging.

A GGUF on the machine itself goes through `yoai-llama`, once the build has
`llama` -- chat with it, or embed text and see how similar the pieces are:

```
build\yoai-llama.exe chat  Qwen3.5-2B-Q4_K_M.gguf "Say hello in one word."
build\yoai-llama.exe embed Qwen3-Embedding-0.6B-Q8_0.gguf --query "What did I eat?" ^
    "Had beef noodles for lunch" "Ran five kilometers"
```

[cli/README.md](cli/README.md) covers every desktop tool, including `yoai-asr`
for speech recognition.

Any OpenAI-compatible endpoint works the same way — only the URL, the key and
the model name change. Google Gemini is worth spelling out because its
compatibility layer sits at a path nobody guesses:

```
export YOAI_API_KEY=...          # from https://aistudio.google.com/apikey
./build/yoai-cli --base-url https://generativelanguage.googleapis.com/v1beta/openai \
                 --model gemini-3.6-flash
```

In the app that same key goes in under Settings → add a model → **Google
Gemini**, which prefills the URL and a model name, so the only thing to paste is
the key itself — and it lands in the platform's encrypted asset store, not in a
config file. The built-in vendor list is
[Provider.ets](examples/harmony/entry/src/main/ets/model/Provider.ets); its `custom`
entry takes any endpoint that is not on it.

### HarmonyOS

Cross-compile the native halves once, then repackage as you go:

```
cd harmony
python build.py       # dependencies, on-device engine, the app
python build.py hap   # just repackage, every run after that
```

The bare command expects a llama.cpp checkout (see below). For
the app without the on-device engine, name the other two targets:
`python build.py deps hap`.

`hap` drives the same hvigor build DevEco does — same tasks, same signing — and
drops an installable `entry-default-signed.hap` under
`examples/harmony/entry/build/default/outputs/default/`. The IDE is for editing and
debugging, not a build dependency. Open `examples/harmony/` in DevEco Studio when you want
it; see [examples/harmony/README.md](examples/harmony/README.md) for signing and the SDK
requirements.

#### On-device models

The app can also run a GGUF on the phone itself, with no network. That lane is
part of the default build; the one thing the script will not fetch for you is
llama.cpp:

```
python third/sync.py clone llama.cpp
cd harmony
python build.py       # or `python build.py llama` for just the engine
```

It costs about 4.3 MiB in `libyoai.so`, a ~200 MB checkout, and a few minutes of
cross-compile. `python build.py deps hap` builds the app without it.

The checkout goes into `third/llama.cpp` at the commit `third/manifest.json`
pins (`LLAMA_SRC` in `.env` moves it) and is never updated for you: this build wants a source you can pin and re-measure against, not one that
changes under you between two runs.

That assembles `examples/harmony/prebuilt/llama-sdk/<abi>/`, which the app's CMakeLists
finds by itself — **building it is what turns the lane on**, and deleting it
turns the lane off again with the app still compiling. There is no path to paste
into `build-profile.json5` and then keep out of commits.

In the app it is Settings → **On-device models** → pick a `.gguf`. The file stays
where it is: it is referenced, never copied into the sandbox, and removing the
model never deletes it. From there it is a model like any other in the picker.

### Android

A **bench**, not a second app. One screen of measurements against the same core,
so a figure taken on a phone here and a figure in
[design/voice.md](design/voice.md) are comparable — every number in that file
was taken on one chip, and several of its conclusions are about the chip rather
than about the engines.

```
cd android
python build.py       # deps, the on-device engine, the APK -- the first run
python build.py apk   # every run after that
```

Lanes turn on the same way they do on HarmonyOS: `python build.py qtts` for
speech out, `python build.py vulkan` to also build
ggml's Vulkan backend for Adreno — which is the one lane HarmonyOS could not
measure, having killed the process by policy before it could be timed.

One thing differs and is worth knowing before reading a number off it. The
`-march` feature set is **asserted by the build, not detected**, because CMake's
probes compile and run a test binary and cannot run one for the phone; the
default is the Kirin 9020's, so the first run on a new device is comparable
rather than optimal. The Device panel prints what the chip actually reports.
See [examples/android/README.md](examples/android/README.md).

### Desktop client (Electron)

A real client, not a harness — the third host to embed the core, after the
HarmonyOS app and this bench. `cli/` above is still the fastest way to exercise
the core; this is the same core with a window on it.

```
cd examples/desktop
python build.py
```

That builds the Node-API addon, with the on-device engine and speech output,
and the Electron bundles, and the app as one directory that runs on its own:
`examples/desktop/dist-<os>-<arch>/`, e.g. `dist-win-x64/YoAI.exe`. The first run fetches
llama.cpp and qwentts.cpp into `third/`. `npm run dev` after that is the loop
while writing UI.

One directory rather than an `electron/` and a `web/`, and
[examples/desktop/README.md](examples/desktop/README.md) argues it: a browser cannot mmap a GGUF
or hold a key out of a page's reach, so a web build would be a different
product rather than a second front end for this one.

## Status

| Capability | State |
|---|---|
| Remote models (OpenAI-compatible, streaming) | working |
| Reasoning output split from the answer | working |
| Token / cost summary per turn | working |
| Conversations persisted on device | working |
| On-device inference (llama.cpp + GGUF) | working on HarmonyOS, in the default build |
| On-device chat on MNN | working on the desktop; not yet built for a phone |
| On-device text embedding (Qwen3-Embedding) | working on the desktop; no C ABI yet |
| Vector search (sqlite-vec, in the core's database) | working; no table in the schema yet |
| Local personal-data storage | the day timeline, in the core's own SQLite database |
| Desktop client (Electron) | chat and on-device models; builds, not yet packaged |
| Android measurement bench | scaffolded; not yet run on a device |
| Android app | not yet |
| Tools / MCP, attachments, markdown rendering | not yet |

## The name

**YoAI** — capital Y, lowercase o, capital AI — is how the name is written
wherever a person reads it: the app name, the launcher label, window titles,
prose. One spelling everywhere; with no space in it, the launcher needs no
exception of its own. The app was called Body Clock before, and the Claude
Design project its visuals come from still carries that name.

The wordmark in [design/logo.md](design/logo.md) was drawn for the old name,
with the ring standing in for the `o` of *Body*. YoAI has its `o` in the same
place, second letter, but the mark has not been redrawn.

Where a single token is needed, it is lowercase `yoai`: `libyoai.so`, the header
`yoai.h` and its C ABI's `yoai_` prefix, the desktop tools, and their `YOAI_*`
environment variables. The bundle id is `com.yoai.bodyclock`.

## License

MIT — see [LICENSE](LICENSE). A personal project.
