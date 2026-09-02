# YoAI

A personal AI client. Your conversations live on your device, and models are
called with your own API keys — there is no YoAI account and no YoAI server.

HarmonyOS first, Android next.

This is the v0 skeleton: one C++ core, one HarmonyOS app, remote
OpenAI-compatible models working end to end. On-device inference and local
personal-data storage are the next two items, and the shape of the core is
what makes them additions rather than rewrites.

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

Needs CMake, a C++17 compiler, and vcpkg.

```
build.cmd            # Windows
./build.sh           # macOS / Linux
```

Then talk to a real provider without a phone in the loop:

```
set YOAI_API_KEY=...
build\yoai-cli.exe --base-url https://open.bigmodel.cn/api/paas/v4 --model glm-4.7-flash
```

With no question argument it goes interactive and keeps the transcript, so
multi-turn context is exercised too. `--usage` prints the token summary,
`--debug` turns on core logging.

### HarmonyOS

```
cd harmony
build-prebuilt.cmd arm64-v8a     # cross-compile curl/openssl/rapidjson, once per ABI
```

Then open `harmony/` in DevEco Studio and run. See
[harmony/README.md](harmony/README.md) for signing and the SDK requirements.

## Status

| | |
|---|---|
| Remote models (OpenAI-compatible, streaming) | working |
| Reasoning output split from the answer | working |
| Token / cost summary per turn | working |
| Conversations persisted on device | working |
| On-device inference (llama.cpp + GGUF) | not yet |
| Local personal-data storage | not yet |
| Android app | not yet |
| Tools / MCP, attachments, markdown rendering | not yet |

## License

MIT — see [LICENSE](LICENSE). A personal project.
