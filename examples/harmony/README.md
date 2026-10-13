# YoAI for HarmonyOS

The HarmonyOS app: an ArkTS UI over the embedded C++ core, reached through a
NAPI module (`libyoai.so`).

There is no server in this picture at all. The app talks to whichever
OpenAI-compatible endpoint the user configured, with the user's own key, and
keeps everything else on the device — or, with an on-device model, does not talk
to anything at all.

## Layout

```
entry/src/main/
  cpp/                    the NAPI bridge; links yoai_core from the repo root
    napi_init.cpp           one worker thread per turn, one tsfn back to ArkTS
    types/libyoai/          the .d.ts ArkTS imports
  ets/
    core/ChatEngine.ets       ChatMessage[] -> the native ABI and back
    core/ChatSettings.ets     the system prompt: the user's, or the core's default
    core/ModelStore.ets       configured models (preferences) + registry refill,
                              preload and prompt warm-up of the on-device model
    core/SecureKeyStore.ets   keys, in the platform's encrypted asset store
    core/LocalModelStore.ets  picking a .gguf or a folder, and keeping it openable
    core/SessionStore.ets     conversations, on this device only
    core/Download.ets         model files from Hugging Face: resume, SHA-256, mirror
    core/ModelDownloads.ets   the two downloads on offer, and where they go
    core/ModelFiles.ets       which files those are, and a speech file's slot by name
    core/SpeechStore.ets      speech output's settings and model files
    core/Reader.ets           the voice: a reply read aloud as it is written
    core/SpeechPlayer.ets     24 kHz PCM to an AudioRenderer, held when too slow
    core/Speak.ets            a message's 朗读 and the settings test
    core/Markdown.ets         a reply's markdown, parsed for streaming
    model/Provider.ets        built-in vendor records; ModelConfig
    components/RichMessage.ets   a reply rendered from its markdown
    components/ReferenceRecorder.ets   recording a reference voice
    pages/Index.ets           the chat screen
    pages/Settings*.ets       models, system prompt, speech output, interface
    pages/VoiceDebug.ets      /asr: recognition, prosody, a voice loop, timed
    pages/TtsDebug.ets        /tts: one speech engine, benchmarked
```

Several files are ports of the desktop client's or the other way round, and
are meant to stay so: where one gains a rule the other should.
`examples/desktop/src/main/models.ts` is `ModelStore.ets`, `chatEngine.ts` is
`ChatEngine.ets`, `App.tsx` is `Index.ets`, `chat.ts` is `ChatSettings.ets`,
`download.ts` is `Download.ets`, `reader.ts` (main and renderer) is
`Reader.ets`, and `audio.ts` is `SpeechPlayer.ets`.

### Where a key lives

In ASSET Kit, one asset per vendor, alias `key.<vendorId>`. It is read out only
to hand to the core, and the core holds it in process memory for as long as the
process lives. It is not in preferences, not in a log, and there is nowhere to
upload it to.

Two consequences: the native registry is empty after every cold start (which is
why `Index.aboutToAppear` calls `ModelStore.syncToCore`), and uninstalling the
app takes the keys with it (no `IS_PERSISTENT` tag, which would need
`ohos.permission.STORE_PERSISTENT_DATA`).

## Prerequisites

- DevEco Studio, with an OpenHarmony SDK that has `native/` (the NDK). Its
  bundled node, hvigor and ohpm are used too, so nothing else needs installing.
- `git`, for the vcpkg checkout the dependency build clones.

The SDK path must not contain a space. OpenSSL's generated Makefile passes the
compiler path to `/bin/sh` unquoted, so a space breaks the build; the script
detects this and creates a junction/symlink alias rather than failing.

## Building

One script builds everything, selected by token — `python build.py`
(`python3` on macOS/Linux):

| | |
|---|---|
| `python build.py` | everything, in the order below — the default |
| `python build.py deps` | the C++ dependencies — the required part |
| `python build.py llama` | the on-device engine (see below) |
| `python build.py qtts` | speech output, which reads replies aloud |
| `python build.py hap` | compile and sign the app |
| `python build.py all` | deps, llama, qtts and hap, each named, so a missing checkout stops it |
| `python build.py --help` | every token, including `clean` and `arch=` |

Tokens compose, so `python build.py deps hap` is a full rebuild of what changed most
days — and it is also how you build the app *without* the engines.

The bare command includes `llama` and `qtts` because on-device chat and speech
output are part of the app, not opt-in lanes, as on the desktop. They are the
expensive part of a first run: the llama.cpp and qwentts.cpp checkouts are
fetched into `../../third/` when they are not there (git and the network,
once), and each is a few minutes of cross-compile. A lane whose checkout is
missing, or differs from what `third/manifest.json` pins, is left out of a bare
build with a line saying so; name it to build it anyway.

No ABI is named above because there is only one that matters: HarmonyOS NEXT is
64-bit ARM throughout, so `arm64-v8a` is both the script's default and the only
entry in `entry/build-profile.json5`'s `abiFilters`.

The emulator is the one reason to want another. It runs an x86_64 image on an
x86 host, so that means `python build.py x86_64` for the native libraries AND adding
`"x86_64"` to `abiFilters` — the script only produces the libraries, it does not
decide what goes into the HAP.

`deps` clones vcpkg at the baseline pinned in `../../vcpkg.json`, builds what that
file lists -- curl with OpenSSL, OpenSSL and zlib -- for OHOS, and lays them out
under `prebuilt/<abi>/`. It has to run at least once: without it, configuring the
native module fails on the first `find_package` — deliberately, with a message
telling you to run this.

### No IDE required

DevEco Studio is an IntelliJ front end over `hvigor`, and hvigor is a
command-line tool, so `hap` drives the same build the IDE does — same tasks,
same signing, same artifact:

```
entry/build/default/outputs/default/entry-default-signed.hap
                                    entry-default-unsigned.hap
```

Install the signed one with `hdc install <file>`. Signing uses the
`signingConfigs` block in `build-profile.json5` — the one DevEco wrote — so a
HAP built here is signed exactly as one built in the IDE. Without that block the
build still succeeds and produces only the unsigned artifact, which is what CI
wants anyway.

The script finds DevEco by reading the `.home` marker it leaves in its own
config directory, and derives the NDK, node and `DEVECO_SDK_HOME` from there, so
none of that needs to be in your environment. With the command-line tools
instead, which leave no marker, it takes the install from the `DEVECO_SDK_HOME`
their setup has you set.

Two things worth knowing if you ever run hvigor by hand: `DEVECO_SDK_HOME` must
point at the SDK *root* (the directory holding `default/`), not at the
`openharmony` directory the NDK lives in — get it wrong and it fails with
"Invalid value of 'DEVECO_SDK_HOME' in the system environment path", which does
not hint that the variable was simply unset. And hvigor needs a node, which
DevEco ships at `tools/node`.

## On-device models

Built by default; a bare `python build.py` fetches llama.cpp into the repo's
`third/` when it is not there (or set `LLAMA_SRC`). To build only this part:

```
python build.py llama
```

That cross-compiles llama.cpp and assembles `prebuilt/llama-sdk/<abi>/`
(`include/` + static `lib/`), which `entry/src/main/cpp/CMakeLists.txt` finds by
itself. **Its presence IS the switch**: build it and `libyoai.so` links the
engine, delete it and the core compiles its stub and the app still builds, with
on-device chat reporting itself unavailable. That off state is still supported —
`python build.py deps hap` is how you ask for it — it just is not what a bare
`python build.py` gives you any more. Nothing to paste into `build-profile.json5`
either, which matters: that file is `skip-worktree`d for signing (below), so any
machine-local path put there would be invisible to everyone else.

Static archives, and CPU only. A HAP has nowhere to put a companion `.so`, so
the engine has to link in; and GPU offload measured worse on every axis on this
class of device, because it copies every tensor into memory that shares its
bandwidth with the CPU. `build.py` has the details, including why the default
`-march` string is what it is (a cross build with no `-march` silently lands on
baseline armv8-a, because CMake's feature probes cannot run a target binary —
which costs the fast integer kernels). Override it with `arch=<march>`, or
`baseline` for plain armv8-a.

### How a .gguf stays readable

The engine mmaps the model BY PATH, so an ArkTS URI or an fd is not enough. A
file the user grants through `DocumentViewPicker` does resolve to a path native
code can open, so the app references the file where it already lives and never
copies it — a multi-GB model has no business being duplicated into the sandbox,
and removing the model here never deletes the file.

The grant is what needs care. It is persisted and then RE-ACTIVATED at startup
(`ModelStore.syncToCore` → `LocalModelStore.activate`), because a persisted grant
is dormant until activated: without that step a model works in the session that
picked it and fails on every launch after, with the file still sitting there.
That needs `ohos.permission.FILE_ACCESS_PERSIST` in `module.json5`, or
`persistPermission` answers 201. Models whose file has moved are dropped from the
list at that point, rather than being left to fail at load time.

The selected on-device model is read at startup and when it is switched to,
not on the first message; the models page shows how that went beside it. A new
conversation drops the model's KV cache (`nativeLocalForget`), and the system
prompt is prefilled ahead of the first turn (`nativeLocalWarm`).

Every model file is read once with plain `read()` before an engine maps it
(`nativePrefetch`). Without that, a 2.74 GB model loaded at startup from
`/storage/Users/currentUser` froze the ArkTS thread for 9.5s on a Mate 70 Pro+,
and the system killed the app for not answering input (`APP_INPUT_BLOCK`). The
thread sat in one IPC that the window manager had already handled. The likely
cause, not yet confirmed by a stack, is llama.cpp's `mmap(MAP_POPULATE)` reading
the whole file under the process's memory-map lock, which the IPC reply needs.
With the pages already in memory, the mapping is quick.

## Speech output

`python build.py qtts`, part of a bare build, assembles `prebuilt/qtts-sdk/<abi>/`,
and its presence turns the lane on, like llama's. The app then speaks through
one lane, the core's reader: a reply is read aloud as the model writes it, a
message's 朗读 reads that message, and the settings page has a test. It is the
desktop's reader with the desktop's settings — the reasons for each are beside
the constants in `core/Reader.ets`.

What differs on a phone is speed. The 1.7B talker measured 3.15–3.64 RTF on a
Kirin 9020 (the root README's table): synthesis takes about three times as long
as the speech. `core/SpeechPlayer.ets` streams a producer only when it keeps
ahead of playback, so here a reply is synthesized first and then played in one
piece. A faster device streams by itself.

The settings page sets it up: Base (a cloned voice) or CustomVoice (a built-in
speaker, with a reading style); the two files that mode needs; for Base, the
voice every piece is anchored to — `01_female` from the repository's
`speaker/`, a recording made on the page, or a `.wav` or `.spk` the user
picks. `entry/hvigorfile.ts` copies the `.spk` files of `speaker/` into
`rawfile/speaker/` on every build; the copy is not tracked.

The voice loads by itself at startup once both files are there, and again after
any change on that page. Only the ArkTS thread calls the engine, and a load or
a voice extraction holds the engine's lock for seconds, so `Reader.ets` keeps a
mirror of the engine's state and hands it settings only at load and before a
stream. `/tts` loads the other speech engine over the same weights, and the
chat screen loads the voice again when it is shown.

## First run

The app opens with nothing configured. Any one of these is enough to start,
all under 设置:

1. **A remote model.** 模型 → 添加模型, pick a vendor, paste its key.
2. **A model on this device.** 模型 → 下载 fetches `Qwen3.5-2B-Q4_K_M.gguf`,
   adds it, and selects it when nothing else is. 选择 GGUF 文件 uses one already
   on the device.
3. **Speech output.** 语音输出 → 下载 fetches the files the voice mode needs, and
   选择已有文件 uses ones already on the device — all of them in one pick, each
   placed by its name.

The phone is offered the 2B where the desktop offers the 4B: the reply is read
as it is written, so the writer's speed is heard, and the 2B decodes at about
twice the 4B's rate.

| File | Size | For |
|---|---|---|
| `Qwen3.5-2B-Q4_K_M.gguf` | 1.28 GB | on-device chat |
| `qwen-talker-1.7b-base-Q4_K_M.gguf` | 1.22 GB | speech, 克隆音色（Base） |
| `qwen-talker-1.7b-customvoice-Q4_K_M.gguf` | 1.18 GB | speech, 内置说话人（CustomVoice） |
| `qwen-tokenizer-12hz-Q8_0.gguf` | 291 MB | speech, either mode |

Each is pinned to a commit with its SHA-256 in `core/ModelFiles.ets`, and the
desktop README has the download URLs for fetching them by hand.

### Where a download goes

A phone downloads into the app's own storage, `filesDir/models`: the picker's
folder mode is for 2-in-1 devices only. Those files are out of the user's sight
and are deleted with the app. A 2-in-1 device asks for a folder, as the
desktop does, and keeps a read-write grant for it.

The rest is the desktop's download (`examples/desktop/README.md`, "How the
speech download works"): a file is written as `NAME.part` and renamed only once
its SHA-256 matches; a break or 30s of silence is retried after 5, 15 and 30s,
resuming the `.part`; 取消 deletes it; huggingface.co and hf-mirror.com are
asked at once and the first to answer is used. It runs in the app's process, so
keep the app in front: a phone that suspends it breaks the transfer off, and the
next download resumes it.

## Replies

A reply is rendered from its markdown (`core/Markdown.ets`,
`components/RichMessage.ets`, from the mirobody project's HarmonyOS client):
headings, lists, tables, code, quotes and links. Images are shown as their alt
text and never fetched, a link opens in the browser only for http, https or
mailto, and math is shown as its TeX source. The last reply carries its token
count.

## Signing

`build-profile.json5` is tracked, and DevEco writes local signing material into
it — certificate paths under `~/.ohos`, plus store and key passwords. Right after
cloning, **before** opening the project in DevEco:

```
git update-index --skip-worktree examples/harmony/build-profile.json5
```

To change the shared settings in that file (products, modules), lift the flag,
commit, and set it again:

```
git update-index --no-skip-worktree examples/harmony/build-profile.json5
```

## Logging

Core logs go to hilog under the tag `yoai`, so they show up in DevEco's Log
window beside the ArkTS ones. `nativeSetLogLevel('debug')` turns on the verbose
ones. On desktop the same core logs to stderr instead — see
`src/utils/log.cpp`.
