# YoAI

*[中文](README.zh-CN.md)*

Your AI. Your conversations live on your device, and models are called with
your own API keys — there is no YoAI account and no YoAI server.

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
  yoai.h                the whole public surface (~150 lines of contract)
  llm/                  one streaming client per provider protocol
  client/               HTTP + TLS
  platform/             logging, and the C ABI's implementation
cli/                  a desktop harness -- run the core without a device
harmony/              the HarmonyOS app (ArkTS) + its NAPI bridge
```

### The core is a library, not a server

There is no port to bind, no config file, and no session state. The host app
owns persistence and settings, tells the core which models the user has keys
for, and asks it to run a turn:

```c
yoai_provider_set("zhipu/glm-4.7-flash",
                  "https://open.bigmodel.cn/api/paas/v4",
                  api_key, "glm-4.7-flash", NULL);

yoai_chat("zhipu/glm-4.7-flash", messages_json, NULL, on_event, user_data);
```

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

Needs CMake, a C++11 compiler, and libcurl / OpenSSL / RapidJSON. `build.sh`
takes those from vcpkg when a checkout is there and from the system's own
packages when it is not — `apt install libcurl4-openssl-dev libssl-dev
rapidjson-dev`, or `brew install openssl@3 rapidjson`. `build.cmd` wants vcpkg,
which is where a Windows machine gets all three anyway.

```
build.cmd            # Windows
./build.sh           # macOS / Linux
```

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
[Provider.ets](harmony/entry/src/main/ets/model/Provider.ets); its `custom`
entry takes any endpoint that is not on it.

### HarmonyOS

Cross-compile the native dependencies once per ABI, then build the app:

```
cd harmony
build.cmd arm64-v8a       # Windows      -- the C++ dependencies, via vcpkg
build.cmd arm64-v8a hap   #              -- compile and sign the app

./build.sh arm64-v8a      # macOS / Linux
./build.sh arm64-v8a hap
```

`hap` drives the same hvigor build DevEco does — same tasks, same signing — and
drops an installable `entry-default-signed.hap` under
`harmony/entry/build/default/outputs/default/`. The IDE is for editing and
debugging, not a build dependency. Open `harmony/` in DevEco Studio when you want
it; see [harmony/README.md](harmony/README.md) for signing and the SDK
requirements.

#### On-device models (optional)

The app can also run a GGUF on the phone itself, with no network. That lane is
off until you build the engine, because llama.cpp is a large static dependency
nobody should pay for by default:

```
git clone --depth 1 https://github.com/ggml-org/llama.cpp ../llama.cpp
cd harmony
build.cmd arm64-v8a llama        # Windows
./build.sh arm64-v8a llama       # macOS / Linux
```

(`all` instead of `llama` does the dependencies and the engine in one go.)

The checkout goes beside the repo (`LLAMA_SRC` moves it) and is never updated
for you: this build wants a source you can pin and re-measure against, not one
that changes under you between two runs.

That assembles `harmony/prebuilt/llama-sdk/<abi>/`, which the app's CMakeLists
finds by itself — **building it is what turns the lane on**, and deleting it
turns the lane off again with the app still compiling. There is no path to paste
into `build-profile.json5` and then keep out of commits.

In the app it is Settings → **On-device models** → pick a `.gguf`. The file stays
where it is: it is referenced, never copied into the sandbox, and removing the
model never deletes it. From there it is a model like any other in the picker.

## Status

| Capability | State |
|---|---|
| Remote models (OpenAI-compatible, streaming) | working |
| Reasoning output split from the answer | working |
| Token / cost summary per turn | working |
| Conversations persisted on device | working |
| On-device inference (llama.cpp + GGUF) | working on HarmonyOS, opt-in at build time |
| Local personal-data storage | not yet |
| Android app | not yet |
| Tools / MCP, attachments, markdown rendering | not yet |

## The name

**YoAI** is *Your AI*, which is the whole product in two words. Read it in
Chinese and it is **有爱** — the same two syllables, meaning warm, or fond of
someone.

Always written `YoAI`. The capital `A` is what splits it into `Yo` + `AI`, so
`Yoai` and `YOAI` both throw the name away. The C ABI keeps its own lowercase
`yoai_` prefix and does not follow the product name.

## License

MIT — see [LICENSE](LICENSE). A personal project.
