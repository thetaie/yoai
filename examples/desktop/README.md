# The desktop client

The fourth host on the same core, after `examples/harmony/`, `examples/android/` and `cli/`. It
links `yoai_core` through a Node-API addon and reaches it with the same C
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
native/               the Node-API addon -- yoai_core + one binding.cpp
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

Needs what the root build needs — git, CMake, a C++11 compiler — plus Node 20
or newer.

```
python build.py      # python3 on macOS / Linux
```

That builds the addon into `build/yoai.node`, with the on-device engine and
speech output, and the Electron bundles into `out/`, and on Windows puts the app
together in `dist-win-x64/` — run `YoAI.exe` there. The first run also fetches
the llama.cpp and qwentts.cpp checkouts into `../../third/` and builds both
engines, which takes a while; later runs are incremental.

The halves are also separate arguments (`native`, `app`), because the C++ half
is minutes and rarely changes while the UI half is seconds, and naming either
leaves the package out. After a change under `src/` or `native/`,
`python build.py native` rebuilds the addon alone; close the app first, since a
running one holds `yoai.node` open.

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

### The app on its own

On Windows the bare build ends by packaging the app with electron-builder
([electron-builder.yml](electron-builder.yml)) into `dist-win-x64/`: one
directory holding everything the app runs with, which runs where it is put —
`YoAI.exe`. There is no installer; to hand it on, copy or zip the directory.
`python build.py dist` is the same thing named. It takes a few seconds, and a
running `YoAI.exe` has to be closed first.

`yoai.node` cannot load from inside the asar, so it goes into the package's
`resources/`, with every DLL it needs that a fresh Windows may lack: curl,
OpenSSL and zlib from vcpkg; `vcruntime140.dll` and `vcomp140.dll` from Visual
Studio's redistributable; and `vulkan-1.dll`, the Vulkan loader, without which
the addon would not load on a machine with no GPU driver. They are found by
reading the imports, and an import nothing supplies stops the build rather
than the app. The speaker's voice files go into `resources/speaker/`.

### On-device models

The GGUF lane is on by default: a bare `python build.py` builds it, fetching
llama.cpp first when it is not there. Speech output (`qtts`) comes along the
same way, on the GPU through Vulkan where there is one, and on the CPU, with a
warning that says what to install, where there is none. Naming lanes builds
those instead — `python build.py native llama` — and MNN's omni lane only ever
comes that way.

Adding a lane to an existing tree needs nothing more: the addon is configured
again on every run. Dropping one is what needs `clean` — a lane not asked for is
not passed at all, so CMake keeps the value the tree had.

The settings screen asks `yoai_local_available()` before offering the
file picker, so a build without the lane says so rather than failing later. The
`.gguf` is referenced where it sits: never copied, and removing the model never
deletes the file. A file that has moved by the next launch is dropped from the
list at startup, because an entry in the picker that fails only when used is
worse than one that is gone.

## Where the data lives

Under Electron's `userData` directory — `%APPDATA%\yoai-desktop` on
Windows, `~/Library/Application Support/yoai-desktop` on macOS,
`~/.config/yoai-desktop` on Linux.

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
`examples/harmony/entry/src/main/ets/core/SecureKeyStore.ets` makes, kept the only way it
can be kept here.

## Colours

From [design/palettes.json](../../design/palettes.json), through
`design/make-css-palettes.py`, which writes
`src/renderer/src/palettes.css`. Do not hand-edit that file, and do not take a
colour from anywhere else — see [design/README.md](../../design/README.md) for what
goes wrong when someone reads one off the canvas instead.

```
npm run palettes
```

## What is not here

- **Packaging off Windows, and signing.** `python build.py dist` packages for
  Windows only so far; macOS and Linux follow once the Windows package has run
  on a machine that never built anything. Nothing is signed, so Windows
  SmartScreen warns the first time a downloaded copy of `YoAI.exe` is run.
- **Voice, TTS and bundles.** They are in the C ABI and deliberately not bound
  in [native/src/binding.cpp](native/src/binding.cpp) — nothing here calls them
  yet, and an export nobody uses is a second place to keep correct.
- **Markdown rendering.** Replies are shown as text, as they are on HarmonyOS.
