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
    core/ModelStore.ets       configured models (preferences) + registry refill
    core/SecureKeyStore.ets   keys, in the platform's encrypted asset store
    core/LocalModelStore.ets  picking a .gguf, and keeping it openable
    core/SessionStore.ets     conversations, on this device only
    model/Provider.ets        built-in vendor records; ModelConfig
    pages/Index.ets           the chat screen
    pages/Settings.ets        models, keys, on-device models, theme
```

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
| `python build.py hap` | compile and sign the app |
| `python build.py all` | the same three the bare command does, named explicitly |
| `python build.py --help` | every token, including `clean` and `arch=` |

Tokens compose, so `python build.py deps hap` is a full rebuild of what changed most
days — and it is also how you build the app *without* the on-device engine.

The bare command includes `llama` because on-device chat is part of the app, not
an opt-in lane. It is the expensive part of a first run, though: it needs a
llama.cpp checkout you clone yourself (~200 MB) and a few minutes of
cross-compile, and it adds about 4.3 MiB to `libyoai.so`.

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
none of that needs to be in your environment. `DEVECO_HOME` overrides it.

Two things worth knowing if you ever run hvigor by hand: `DEVECO_SDK_HOME` must
point at the SDK *root* (the directory holding `default/`), not at the
`openharmony` directory the NDK lives in — get it wrong and it fails with
"Invalid value of 'DEVECO_SDK_HOME' in the system environment path", which does
not hint that the variable was simply unset. And hvigor needs a node, which
DevEco ships at `tools/node`.

## On-device models

Built by default, but the one thing the script will not fetch for you is
llama.cpp itself — clone it once (beside the repo, or set `LLAMA_SRC`) and the
bare `python build.py` covers the rest. To build only this part:

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
