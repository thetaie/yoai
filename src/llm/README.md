# src/llm

Everything in the core that runs a language model: chat, and text embedding for
search by meaning, on this device through llama.cpp or behind an
OpenAI-compatible HTTP API. Everything is in namespace `yoai::llm`.

## The interfaces

Code outside this directory holds a `Chat` or an `Embedder` and never asks
where the model runs. Both extend `Backend`, which is the part that differs
between on-device and remote. A model names its engine when it is constructed,
with `Backend::Engine`; whether it runs on this device follows from that:

```
Backend                     load / loaded / set_threads
├── Chat                    ainvoke / forget
│   ├── LocalClient         Engine::LlamaCpp    a GGUF
│   ├── MnnClient           Engine::Mnn         an MNN export directory
│   └── OpenAIChatClient    Engine::Remote      HTTP, /chat/completions
└── Embedder                dim / embed
    └── LocalEmbedder       Engine::LlamaCpp
```

**[Backend](backend.hpp)** is a model's lifecycle:

| | on-device | remote |
|---|---|---|
| `load()` | reads the weights; takes seconds | nothing to read; succeeds |
| `loaded()` | true once the weights are in | always true |
| `set_threads()` | CPU threads from the next call | ignored |

`load()` is optional, because a model that is not loaded loads on first use.
Calling it early moves the wait to a moment when nobody is waiting. It never
touches the network: a remote endpoint's reachability and key are checked by
using it.

`Backend`'s constructor starts the engine's process-wide state once per
process, so no model can reach its engine before it is up: for
`Engine::LlamaCpp`, the library's log and initialization; for `Engine::Mnn`,
keeping its Vulkan backend in a static link; for `Engine::Remote`, nothing.
That is all the engine decides, and a model on one engine never starts
another. The three methods stay pure for every engine: a default could only
be the remote one, and a local model that forgot to override `load()` would
then report itself loaded with no weights read. A remote model's three are one
line each; see `OpenAIChatClient`.

**[Chat](chat/chat.hpp)** streams a reply through an `EventHandler`. The contract
every implementation keeps:

- a turn that completes ends with exactly one `CostStatistics` event;
- a turn that fails sends one `Error` event and returns false;
- a handler that returns false cancels the turn, and that is not an error;
- turns are stateless, since the whole transcript comes in every time. `forget()`
  starts a new conversation. It does nothing for a remote model, but an
  on-device one must be told, because it keeps its KV cache between turns.

**[Embedder](embed/embedder.hpp)** turns one text into one unit-length vector of
`dim()` floats. The caller passes `EmbedKind::Document` for text that is stored
and `EmbedKind::Query` for text that is searched with. The embedder decides
what that means for its model: Qwen3-Embedding puts an instruction in front of
queries only. `store/vector_index.hpp` searches by L2 distance, which between
unit vectors ranks exactly as cosine does.

Errors are returned, never thrown. Everything here sits under the C ABI.

## Layout

Split by what a model does, not by where it runs. The two llama.cpp engines
sit in different directories and share only `Backend`'s constructor.

### The root -- what both halves share

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [backend](backend.hpp) | `Backend`, `Backend::Engine`: one model's lifecycle, as above. The constructor, in `backend.cpp`, starts the engine; for llama.cpp that is a no-op without the flag | llama.cpp for the startup | always built |

### chat/ -- a conversation in, a streamed reply out

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [chat](chat/chat.hpp) | `Chat`: a streamed conversation turn | none | always built |
| [types](chat/types.hpp) | `ChatMessage`, `EventHandler`, `UserContext`, `RequestOptions`: what a turn is made of | none | always built |
| [event](chat/event.hpp) | `Event`, `EventType`, `CostStatistics`: what comes back out | none | always built |
| [local](chat/local.hpp) | `LocalClient`: chat on a GGUF, with KV-cache reuse across turns, `<think>` splitting and GBNF grammars | llama.cpp | `YOAI_ONDEVICE_LLM` |
| [mnn](chat/mnn.hpp) | `MnnClient`: chat on an MNN export, with MNN's prompt cache across turns and the same `<think>` splitting. No grammar; threads apply at load; an omni export chats with its Talker off | MNN | `YOAI_MNN` |
| [openai](chat/openai.hpp) | `OpenAIChatClient`: any OpenAI-compatible endpoint, Azure included, with the tool-call loop | curl | always built |
| [sse_parser](chat/sse_parser.hpp) | Server-sent events, fed in arbitrary chunks | none | always built |

### embed/ -- text in, a vector out

| File | What it does | Engine | Build flag |
|---|---|---|---|
| [embedder](embed/embedder.hpp) | `Embedder`, `EmbedKind`: text to a unit vector | none | always built |
| [local](embed/local.hpp) | `LocalEmbedder`: embeddings from a GGUF that llama.cpp pools; written for Qwen3-Embedding-0.6B (1024 dimensions, last-token pooling) | llama.cpp | `YOAI_ONDEVICE_LLM` |

## Threads and sharing

A remote client holds nothing but its options, so one instance can serve
concurrent calls. An on-device one owns weights and a context, and serializes
its calls on a mutex, so a second caller waits for the first. Either way the
caller owns threading: every call blocks until it is done.

`yoai.cpp` keeps one `Chat` per registered id, of either kind, and
drops it whenever that id is registered again or removed.

## Build flag and stubs

`-DYOAI_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=<SDK with include/llama.h and lib/>`
compiles in the llama.cpp engines, and `-DYOAI_MNN=ON -DMNN_DIR=<MNN SDK>` the
MNN one. Either, both or neither. Without its flag, an engine's files compile
to stubs: `available()` returns false, `load()` fails with a message saying
why, and a chat turn sends a single `Error`. That is the supported off state,
not a degraded one. The default desktop build links with neither.

`yoai_local_set` picks the engine from the model path: a directory holding
`config.json` is MNN, anything else llama.cpp. A host never names the engine.

## Adding an implementation

1. Derive from `Chat` or `Embedder`, pass its `Engine` to that constructor, and implement `load`, `loaded` and `set_threads`. A remote
   model's are one line each; see `OpenAIChatClient`.
2. Keep the contract above. For a `Chat`, a completed turn ends with one
   `CostStatistics`, even when nobody was billed. For an `Embedder`, normalize
   the output, and map `EmbedKind` to whatever the model expects.
3. Add the `.cpp` to `bodyclock_core` in the root `CMakeLists.txt`. If it needs
   an engine, give it a stub under that engine's flag, the way `chat/local.cpp`
   does. It goes in `chat/` or `embed/` by what it does, whatever it runs on.
4. Give it a test under `tests/` that goes through the interface rather than
   the concrete class, the way `tests/embed_test.cpp` does.

## The C ABI

`yoai_chat` takes an id and does not care which kind it names. Registration
still differs by kind: `yoai_provider_set` registers a remote model and
`yoai_local_set` an on-device one, on whichever engine the path needs.
`yoai_local_load`, `yoai_local_set_threads`
and `yoai_local_forget` refuse a remote id. Embedding has no C entry points yet.
