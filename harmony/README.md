# YoAI for HarmonyOS

The HarmonyOS app: an ArkTS UI over the embedded C++ core, reached through a
NAPI module (`libyoai.so`).

There is no server in this picture at all. The app talks to whichever
OpenAI-compatible endpoint the user configured, with the user's own key, and
keeps everything else on the device.

## Layout

```
entry/src/main/
  cpp/                    the NAPI bridge; links yoai_core from the repo root
    napi_init.cpp           one worker thread per turn, one tsfn back to ArkTS
    types/libyoai/          the .d.ts ArkTS imports
  ets/
    core/ChatEngine.ets     ChatMessage[] -> the native ABI and back
    core/ModelStore.ets     configured models (preferences) + registry refill
    core/SecureKeyStore.ets keys, in the platform's encrypted asset store
    core/SessionStore.ets   conversations, on this device only
    model/Provider.ets      built-in vendor records; ModelConfig
    pages/Index.ets         the chat screen
    pages/Settings.ets      models, keys, theme
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

- DevEco Studio with an OpenHarmony SDK that has `native/` (the NDK).
- The core's native dependencies, cross-compiled once per ABI:

  ```
  build-prebuilt.cmd arm64-v8a       # Windows
  ./build-prebuilt.sh arm64-v8a      # macOS / Linux
  ```

  This clones vcpkg at the baseline pinned in `../vcpkg.json`, builds
  `openssl`, `curl[openssl]`, `rapidjson` and `zlib` for OHOS, and lays them out
  under `prebuilt/<abi>/`. Without it, configuring the native module fails on the
  first `find_package` — deliberately, with a message telling you to run this.

  The SDK path must not contain a space. OpenSSL's generated Makefile passes the
  compiler path to `/bin/sh` unquoted, so a space breaks the build; the script
  detects this and creates a junction/symlink alias rather than failing.

- Only `arm64-v8a` is enabled in `entry/build-profile.json5`. Add `x86_64` there
  (and run the script for it) to target the emulator.

## Signing

`build-profile.json5` is tracked, and DevEco writes local signing material into
it — certificate paths under `~/.ohos`, plus store and key passwords. Right after
cloning, **before** opening the project in DevEco:

```
git update-index --skip-worktree harmony/build-profile.json5
```

To change the shared settings in that file (products, modules), lift the flag,
commit, and set it again:

```
git update-index --no-skip-worktree harmony/build-profile.json5
```

## Logging

Core logs go to hilog under the tag `yoai`, so they show up in DevEco's Log
window beside the ArkTS ones. `nativeSetLogLevel('debug')` turns on the verbose
ones. On desktop the same core logs to stderr instead — see
`src/platform/log.cpp`.
