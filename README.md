# Body Clock

Your conversations live on your device, and models are called with your own API
keys — there is no Body Clock account and no Body Clock server.

HarmonyOS first, Android next.

This is the v0 skeleton: one C++ core, one HarmonyOS app, remote
OpenAI-compatible models working end to end — and, on HarmonyOS, GGUF models
running on the device itself through llama.cpp. Local personal-data storage is
the next item, and the shape of the core is what makes it an addition rather
than a rewrite: the on-device engine went in as a second `llm::Client` behind
the same C ABI, and nothing above it had to learn about lanes.

## What is here

```
src/                  the core -- a static library plus a C ABI
  bodyclock.h                the whole public surface (~150 lines of contract)
  llm/                  one streaming client per provider protocol
  client/               HTTP + TLS
  platform/             logging, and the C ABI's implementation
cli/                  a desktop harness -- run the core without a device
harmony/              the HarmonyOS app (ArkTS) + its NAPI bridge
android/              a measurement bench (Compose) + its JNI bridge
desktop/              the desktop client (Electron) + its Node-API bridge
```

### The core is a library, not a server

There is no port to bind, no config file, and no session state. The host app
owns persistence and settings, tells the core which models the user has keys
for, and asks it to run a turn:

```c
bodyclock_provider_set("zhipu/glm-4.7-flash",
                  "https://open.bigmodel.cn/api/paas/v4",
                  api_key, "glm-4.7-flash", NULL);

bodyclock_chat("zhipu/glm-4.7-flash", messages_json, NULL, on_event, user_data);
```

Two consequences worth stating plainly, because they are the design and not an
implementation detail:

- **Keys never touch our disk.** The registry is process memory. On HarmonyOS the
  key comes out of the platform's encrypted asset store at the moment it is
  handed to the core, and the app refills the registry after every cold start.
- **The host owns history.** `bodyclock_chat` takes the whole transcript every turn.
  One place decides what to keep and what to trim, so two policies cannot
  disagree.

`bodyclock_chat` blocks and streams through a callback on the calling thread;
returning 0 from that callback cancels the turn. Everything above it — a worker
thread per turn, a threadsafe function back to the UI — is the host's business,
which is why the ABI stays this small.

## Building

### Desktop (to exercise the core)

Needs CMake, a C++11 compiler, and libcurl / OpenSSL; RapidJSON and SQLite are
vendored in `third/`. `build.sh` takes the two libraries from vcpkg when a
checkout is there and from the system's own packages when it is not — `apt
install libcurl4-openssl-dev libssl-dev`, or `brew install openssl@3`.
`build.cmd` wants vcpkg, which is where a Windows machine gets both anyway.

```
build.cmd build      # Windows
./build.sh build     # macOS / Linux
```

With no arguments either script prints its commands and builds nothing.

Paths that differ per machine -- Visual Studio, vcpkg, the llama.cpp checkout
-- go in `.env` at the root, which git ignores and every C/C++ build script
reads (root, `harmony/`, `desktop/`). Copy [.env.example](.env.example); it
lists every `YOAI_*` setting.

Then talk to a real provider without a phone in the loop:

```
rem Windows
set BODYCLOCK_API_KEY=...
build\bodyclock-cli.exe --base-url https://open.bigmodel.cn/api/paas/v4 --model glm-4.7-flash
```

```
# macOS / Linux
export BODYCLOCK_API_KEY=...
./build/bodyclock-cli --base-url https://open.bigmodel.cn/api/paas/v4 --model glm-4.7-flash
```

With no question argument it goes interactive and keeps the transcript, so
multi-turn context is exercised too. `--usage` prints the token summary,
`--debug` turns on core logging. [cli/README.md](cli/README.md) covers every
desktop tool, including `bodyclock-asr` for speech recognition.

Any OpenAI-compatible endpoint works the same way — only the URL, the key and
the model name change. Google Gemini is worth spelling out because its
compatibility layer sits at a path nobody guesses:

```
export BODYCLOCK_API_KEY=...          # from https://aistudio.google.com/apikey
./build/bodyclock-cli --base-url https://generativelanguage.googleapis.com/v1beta/openai \
                 --model gemini-3.6-flash
```

In the app that same key goes in under Settings → add a model → **Google
Gemini**, which prefills the URL and a model name, so the only thing to paste is
the key itself — and it lands in the platform's encrypted asset store, not in a
config file. The built-in vendor list is
[Provider.ets](harmony/entry/src/main/ets/model/Provider.ets); its `custom`
entry takes any endpoint that is not on it.

### HarmonyOS

Cross-compile the native halves once, then repackage as you go:

```
cd harmony
build.cmd           # Windows        -- dependencies, on-device engine, the app
build.cmd hap       #                -- just repackage, every run after that

./build.sh          # macOS / Linux
./build.sh hap
```

The bare command expects a llama.cpp checkout beside the repo (see below). For
the app without the on-device engine, name the other two targets:
`build.cmd deps hap`.

`hap` drives the same hvigor build DevEco does — same tasks, same signing — and
drops an installable `entry-default-signed.hap` under
`harmony/entry/build/default/outputs/default/`. The IDE is for editing and
debugging, not a build dependency. Open `harmony/` in DevEco Studio when you want
it; see [harmony/README.md](harmony/README.md) for signing and the SDK
requirements.

#### On-device models

The app can also run a GGUF on the phone itself, with no network. That lane is
part of the default build; the one thing the script will not fetch for you is
llama.cpp:

```
git clone --depth 1 https://github.com/ggml-org/llama.cpp ../llama.cpp
cd harmony
build.cmd             # Windows       -- or `build.cmd llama` for just the engine
./build.sh            # macOS / Linux
```

It costs about 4.3 MiB in `libbodyclock.so`, a ~200 MB checkout, and a few minutes of
cross-compile. `build.cmd deps hap` builds the app without it.

The checkout goes beside the repo (`YOAI_LLAMA_SRC` moves it) and is never updated
for you: this build wants a source you can pin and re-measure against, not one
that changes under you between two runs.

That assembles `harmony/prebuilt/llama-sdk/<abi>/`, which the app's CMakeLists
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
./build.sh          # deps, the on-device engine, the APK -- the first run
./build.sh apk      # every run after that
```

`build.cmd` is the same thing on Windows. Lanes turn on the same way they do on
HarmonyOS: `./build.sh qtts` for speech out, `./build.sh vulkan` to also build
ggml's Vulkan backend for Adreno — which is the one lane HarmonyOS could not
measure, having killed the process by policy before it could be timed.

One thing differs and is worth knowing before reading a number off it. The
`-march` feature set is **asserted by the build, not detected**, because CMake's
probes compile and run a test binary and cannot run one for the phone; the
default is the Kirin 9020's, so the first run on a new device is comparable
rather than optimal. The Device panel prints what the chip actually reports.
See [android/README.md](android/README.md).

### Desktop client (Electron)

A real client, not a harness — the third host to embed the core, after the
HarmonyOS app and this bench. `cli/` above is still the fastest way to exercise
the core; this is the same core with a window on it.

```
cd desktop
build.cmd            # Windows
./build.sh           # macOS / Linux
```

That builds the Node-API addon and the Electron bundles; `npm run dev` after
that is the loop while writing UI. The on-device lane is off by default for the
same reason it is everywhere else and turns on the same way: `build.cmd clean
llama`, with a llama.cpp checkout beside the repo.

One directory rather than an `electron/` and a `web/`, and
[desktop/README.md](desktop/README.md) argues it: a browser cannot mmap a GGUF
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
| Local personal-data storage | not yet |
| Desktop client (Electron) | chat and on-device models; builds, not yet packaged |
| Android measurement bench | scaffolded; not yet run on a device |
| Android app | not yet |
| Tools / MCP, attachments, markdown rendering | not yet |

## The name

**Body Clock** is the ordinary English term for the body's own sense of time.

Always written with the space. The wordmark depends on it: the ring stands in
for the `o` of *Body*, and the rest is set as `dy Clock`.

The launcher label is the one exception — `BodyClock`, closed up. Closing the
space buys back about half a character of width, which is 5–6% of the label
across the usual UI sans faces, and the launcher is the only place the name has
to survive a hard width limit. It is a separate resource from the app name
(`EntryAbility_label`, not `app_name`), so nothing else follows it.

Where a single token is needed, it is lowercase `bodyclock`: the C ABI's
`bodyclock_` prefix, `libbodyclock.so`, the bundle id `ai.bodyclock.app`. Never
`BodyClock` as a second display spelling.

## License

MIT — see [LICENSE](LICENSE). A personal project.
