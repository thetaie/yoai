# The desktop client

The fourth host on the same core, after `harmony/`, `android/` and `cli/`. It
links `bodyclock_core` through a Node-API addon and reaches it with the same C
ABI everything else uses — `src/yoai.h` is the contract here too, and
nothing in this directory reimplements a line of it.

## One directory, not two

There is no `web/` beside this, and the reason is not tidiness.

A browser cannot be this product. The on-device lane mmaps a `.gguf` from a
real filesystem path, the keys live in process memory in a process the page
cannot reach, and the HTTP to a provider is libcurl inside the core rather than
`fetch` from a document. A browser build would be a different application that
happens to share a colour palette: remote models only, keys in browser storage,
no local inference. It is not on the roadmap in the root README's status table,
and paying for a second directory, build and dependency tree against it now
would be paying early for a thing whose shape nobody has decided.

What Electron actually needs split is main / preload / renderer, and that lives
inside `src/` here. `src/renderer/` is already the separable unit: the day a
browser build is wanted, it is the piece that moves, and moving it then costs no
more than it would today — with the advantage of knowing what it has to do.

## Layout

```
native/               the Node-API addon -- bodyclock_core + one binding.cpp
src/main/             the privileged half: the addon, keys, files, the window
src/preload/          contextBridge -- the whole surface the page can reach
src/renderer/         the UI (React), with no Node and no addon
src/shared/           types both halves need, and the vendor list
```

The split is a security boundary, not a convention. The renderer runs with
`contextIsolation` on, node integration off and the sandbox in force, and it
talks to the main process through the named calls in
[src/shared/api.ts](src/shared/api.ts) and nothing else. The core's registry —
which is where a user's API key sits while the app runs — is in the main
process, so a page that is compromised has not thereby reached a key.

Two files are ports rather than new work, and are meant to stay ports:
[chatEngine.ts](src/renderer/src/chatEngine.ts) is `ChatEngine.ets`, and
[models.ts](src/main/models.ts) is `ModelStore.ets`. Where one gains a rule the
other should.

## Building

Needs what the root build needs — CMake, a C++11 compiler, curl / OpenSSL —
plus Node 20 or newer.

```
build.cmd            # Windows
./build.sh           # macOS / Linux
```

That builds the addon into `build/Release/bodyclock.node` and the Electron
bundles into `out/`. The two are separate arguments (`addon`, `app`) because the
C++ half is minutes and rarely changes while the UI half is seconds.

While writing UI, the loop is:

```
npm run dev
```

which needs the addon built once and then leaves it alone.

`postinstall` in package.json runs `install-electron`, and it is not boilerplate.
The npm package `electron` is a few hundred kilobytes of JavaScript; the runtime
is a 250 MB binary fetched separately, and as of 44.4.5 that fetch is **no longer
the electron package's own postinstall** — the package declares no scripts at all
and ships an `install-electron` bin instead. Without this line, `npm install`
leaves `node_modules/electron/dist` missing and every command that starts the app
fails with `Error: Electron uninstall`, which names neither npm nor a download.

### On-device models

The GGUF lane is off by default here for the same reason it is off in the root
build: it is a few hundred megabytes of static archives, and `src/llm/local.cpp`
compiles to a truthful stub without them. Turning it on:

```
git clone --depth 1 https://github.com/ggml-org/llama.cpp ../../llama.cpp
build.cmd clean llama            # Windows
./build.sh clean llama           # macOS / Linux
```

`clean` is not optional when changing the lane set — CMake caches it, so adding
`llama` to a tree configured without it silently builds the same addon again.

The settings screen asks `yoai_local_available()` before offering the
file picker, so a build without the lane says so rather than failing later. The
`.gguf` is referenced where it sits: never copied, and removing the model never
deletes the file. A file that has moved by the next launch is dropped from the
list at startup, because an entry in the picker that fails only when used is
worse than one that is gone.

## Where the data lives

Under Electron's `userData` directory — `%APPDATA%\bodyclock-desktop` on
Windows, `~/Library/Application Support/bodyclock-desktop` on macOS,
`~/.config/bodyclock-desktop` on Linux.

| File | What |
|---|---|
| `models.json` | configured models and the selected one — no keys |
| `keys.json` | one OS-encrypted blob per vendor |
| `interface.json` | theme, palette family, text size |
| `sessions/` | the index, plus one file per conversation |

**Keys are encrypted by the operating system or not stored at all.** On Windows
that is DPAPI and on macOS the Keychain; on Linux it depends on whether a
secret service is running, and where one is not, Electron's `safeStorage` falls
back to a built-in password — encryption anyone with the file can undo. This
client refuses that case and says so in settings rather than writing a key
under it. That is the same promise
`harmony/entry/src/main/ets/core/SecureKeyStore.ets` makes, kept the only way it
can be kept here.

## Colours

From [design/palettes.json](../design/palettes.json), through
`design/make-css-palettes.py`, which writes
`src/renderer/src/palettes.css`. Do not hand-edit that file, and do not take a
colour from anywhere else — see [design/README.md](../design/README.md) for what
goes wrong when someone reads one off the canvas instead.

```
npm run palettes
```

## What is not here

- **Packaging.** `npm run build` produces `out/`, which `npm run preview` runs.
  There is no installer configuration: signing and per-platform packaging is its
  own job and an untested config would be worse than none.
- **Voice, TTS and bundles.** They are in the C ABI and deliberately not bound
  in [native/src/binding.cpp](native/src/binding.cpp) — nothing here calls them
  yet, and an export nobody uses is a second place to keep correct.
- **Markdown rendering.** Replies are shown as text, as they are on HarmonyOS.
