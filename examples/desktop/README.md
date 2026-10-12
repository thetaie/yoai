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
speech output, and the Electron bundles into `out/`, and puts the app together
in `dist-<os>-<arch>/` — `YoAI.exe` in `dist-win-x64/`, `YoAI.app` in
`dist-mac-arm64/`, `yoai` in `dist-linux-x64/`. The first run also fetches
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

The bare build ends by packaging the app with electron-builder
([electron-builder.yml](electron-builder.yml)) into `dist-<os>-<arch>/`: one
directory holding everything the app runs with, which runs where it is put.
There is no installer; to hand it on, copy or zip the directory.
`python build.py dist` is the same thing named. It takes a few seconds, and a
running copy has to be closed first. The package is for the machine that builds
it — Windows on Windows, an arm64 Mac on an arm64 Mac — never cross-built.

`yoai.node` cannot load from inside the asar, so it goes into the package's
`resources/`, with every library it needs that a fresh machine may lack. They
are found by reading what the addon loads, and one that nothing supplies stops
the build rather than the app. The speaker's voice files go into
`resources/speaker/`.

- **Windows**: curl, OpenSSL and zlib from vcpkg; `vcruntime140.dll` and
  `vcomp140.dll` from Visual Studio's redistributable; and `vulkan-1.dll`, the
  Vulkan loader, without which the addon would not load on a machine with no
  GPU driver.
- **Linux**: `libgomp.so.1` (ggml's OpenMP), and `libvulkan.so.1` when speech
  output is built with Vulkan. The addon looks beside itself first
  (RUNPATH `$ORIGIN`). glibc is not bundled, so the package needs a distro at
  least as new as the one that built it, and Electron's own dependencies (GTK,
  NSS, ALSA) come from the desktop.
- **macOS**: nothing — every library is static and the rest is macOS's. The
  build checks that, and stops on anything from Homebrew. The app is signed ad
  hoc: it runs where it was built, and a copy on another Mac has to be allowed
  in System Settings first.

One thing is left out on purpose: the CUDA runtime, when speech output is built
with CUDA. See "Speech output on an NVIDIA GPU" below.

### On-device models

The GGUF lane is on by default: a bare `python build.py` builds it, fetching
llama.cpp first when it is not there. Speech output (`qtts`) comes along the
same way, on the GPU where there is one — CUDA or Vulkan, see below — and on
the CPU, with a warning that says what to install, where there is none. Naming lanes builds
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

### Speech output on an NVIDIA GPU

On Windows and Linux, speech output gets one GPU backend, chosen when the addon
is built: CUDA where the build machine has a CUDA Toolkit (`CUDA_PATH`, `nvcc`
on `PATH`, or `/usr/local/cuda` on Linux), else Vulkan, else the CPU.
`TTS_BACKEND` in `.env` — `cuda`, `vulkan` or `cpu` — picks one instead, and
stops the build when that one is not there rather than building another. macOS
always uses Metal.

On Windows, each CUDA release compiles only with a range of MSVC versions, and
its nvcc refuses any other:

| CUDA | Visual Studio |
|---|---|
| 13.2 and later | 2019, 2022, 2026 |
| 12.4 – 13.1 | 2019, 2022 |
| 12.0 – 12.3 | 2019, 2022 17.0 – 17.9 |

17.9 is an update of Visual Studio 2022, as Help → About and the Visual Studio
Installer show it. 17.10 moved MSVC from 19.3x to 19.40, past what CUDA 12.3
and earlier accept.

The build compiles with the newest Visual Studio under Program Files, or the
one at `VS_DIR`. With 2026 picked and an older CUDA, it stops while CMake
checks the CUDA compiler, on `unsupported Microsoft Visual Studio version!`.
Point `VS_DIR` in `.env` at a Visual Studio 2022 install, which can sit beside
2026, or install a newer CUDA.
NVIDIA's CUDA Installation Guide for Windows lists the range of each release.

**A package built with CUDA needs CUDA on every machine it runs on.**
`yoai.node` links the CUDA runtime (cudart) and cuBLAS, and the package does not
carry them: they are several hundred megabytes, and NVIDIA ships them for any
machine with its GPU. Where they are missing the addon does not load. The window
opens with the load error where the composer would be, and nothing works,
chat with a remote model included. On Windows that error does not name the
missing DLL. It does not fall back to Vulkan, and a machine without an NVIDIA
GPU is no exception. For a package that runs anywhere, build it with
`TTS_BACKEND=vulkan`.

The build says which CUDA it used — `qtts backend: CUDA 12.6` — and `dist`
repeats it beside the libraries it left out. The machine the package runs on
needs:

- an NVIDIA driver that supports that CUDA version. `nvidia-smi` prints the
  highest one it supports, top right, as `CUDA Version`;
- cudart and cuBLAS of the same major version and not older: a package built
  with 12.6 runs with 12.6 or a later 12.x, not with 12.4 or 13.0.

To install the two:

- **Windows**: NVIDIA's CUDA Toolkit installer. In a custom install, tick only
  the runtime libraries; run silently, name only those two components, e.g.
  `<installer>.exe -s cudart_12.6 cublas_12.6`. The DLLs land in the toolkit's
  `bin` directory, which the installer puts on `PATH`, and the app finds them
  there. `where cudart64_12.dll` in a new terminal should print a path.
- **Linux**: the two runtime packages alone, from NVIDIA's CUDA repository for
  the distribution: `sudo apt install cuda-cudart-12-6 libcublas-12-6` on Debian
  and Ubuntu, `sudo dnf install cuda-cudart-12-6 libcublas-12-6` on Fedora and
  RHEL. A distribution's own packages work too when they are not older than the
  build's CUDA; on Ubuntu those are `libcudart12` and `libcublas12`. NVIDIA's
  runfile installer puts the libraries in `/usr/local/cuda/lib64`, which the
  loader does not search until told:
  `echo /usr/local/cuda/lib64 | sudo tee /etc/ld.so.conf.d/cuda.conf && sudo ldconfig`.
  Either way, `ldconfig -p | grep -E 'libcudart|libcublas'` should list both.

A Windows package built with CUDA has been run, with speech output on the GPU.

## First run

The app opens with nothing configured. Any one of these is enough to start,
and all three are under 设置:

1. **A remote model.** 添加远程模型, pick a vendor, paste its key. Nothing to
   download; 去获取 key › opens the vendor's sign-up page.
2. **A model on this machine.** 添加本机模型 → 下载到文件夹 fetches
   `Qwen3.5-4B-Q4_K_M.gguf`, adds it, and selects it when nothing else is
   selected. 选择 GGUF 文件 uses one already on disk.
3. **Speech output.** 语音输出 → 下载到文件夹 fetches the files it needs, and
   选择已有文件 uses ones already on disk. Once they are in place, replies are
   read aloud as they are written.

### The model files

| File | Size | For | Set in |
|---|---|---|---|
| `Qwen3.5-4B-Q4_K_M.gguf` | 2.74 GB | on-device chat | 添加本机模型 |
| `qwen-talker-1.7b-base-Q4_K_M.gguf` | 1.22 GB | speech, 克隆音色（Base） | 语音输出 |
| `qwen-talker-1.7b-customvoice-Q4_K_M.gguf` | 1.18 GB | speech, 内置说话人（CustomVoice） | 语音输出 |
| `qwen-tokenizer-12hz-Q8_0.gguf` | 291 MB | speech, either mode | 语音输出 |

For speed, `Qwen3.5-2B-Q4_K_M.gguf` (1.28 GB, below) is the alternative to
pick by hand: [docs/voice.md](../../docs/voice.md) measured the 2B at 14.0
tok/s and the 4B at 6.70 tok/s.
Speech needs the talker of the chosen mode plus the tokenizer; the default mode
is Base, so 1.5 GB to start. The reasons for the speech files' quantizations
are beside their URLs in [src/main/ttsfiles.ts](src/main/ttsfiles.ts).

The app downloads these files itself. For a machine that will run offline they
can be fetched by hand:

```
curl -L -C - -O https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/e87f176479d0855a907a41277aca2f8ee7a09523/Qwen3.5-4B-Q4_K_M.gguf
curl -L -C - -O https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/f6d5376be1edb4d416d56da11e5397a961aca8ae/Qwen3.5-2B-Q4_K_M.gguf
curl -L -C - -O https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF/resolve/b7ee2e8c7459c3bea99da23e3d178125a7d1713c/qwen-talker-1.7b-base-Q4_K_M.gguf
curl -L -C - -O https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF/resolve/b7ee2e8c7459c3bea99da23e3d178125a7d1713c/qwen-talker-1.7b-customvoice-Q4_K_M.gguf
curl -L -C - -O https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF/resolve/b7ee2e8c7459c3bea99da23e3d178125a7d1713c/qwen-tokenizer-12hz-Q8_0.gguf
```

- `-C -` resumes a download that broke off when the line is run again.
- In Windows PowerShell type `curl.exe`: plain `curl` there is
  Invoke-WebRequest, which takes none of these options.
- From mainland China, put `hf-mirror.com` in place of `huggingface.co`.
- Each URL names a commit, so the file is the one measured here. The speech
  files' SHA-256 are `KNOWN` in [src/main/ttsfiles.ts](src/main/ttsfiles.ts),
  the 4B's is `OFFER` in [src/main/llmfiles.ts](src/main/llmfiles.ts), and the
  2B's is `aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223`.

Keep the speech files in one folder and under these names. Picking any one of
them then fills in the others, and 下载到文件夹 pointed at that folder fetches
nothing.

### How the speech download works

Speech output needs two GGUFs, a talker and the 12 Hz tokenizer. Until the
current voice mode has both, the speech section of the settings screen lists
what is missing and offers two ways to get it, because only the user knows
whether they already have the files:

- **选择已有文件** picks any one of them. Its slot is read from its name, and
  the other empty slots are filled from the same folder by name.
- **下载到文件夹** downloads into a folder the user picks: the talker of the
  current mode and the tokenizer, from the table above. A file already in the
  folder under its name and size is not fetched again.

A file is written as `NAME.part` and takes its real name only after its SHA-256
matches. A transfer that breaks off, or goes 30s without a byte, is retried
three times, after 5s, 15s and 30s, each time resuming from the `.part`; the
count starts over once a retry brings bytes in. When the retries run out the
`.part` stays and the next download resumes it, while 取消 deletes it.

huggingface.co and hf-mirror.com are asked at once and the first to answer is
used, so mainland China needs no setting; that path has not yet been tried from
inside mainland China.

Nothing goes under userData. The folder is the user's, and the app never moves
or deletes the files.

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
`design/scripts/make-css-palettes.py`, which writes
`src/renderer/src/palettes.css`. Do not hand-edit that file, and do not take a
colour from anywhere else — see [design/README.md](../../design/README.md) for what
goes wrong when someone reads one off the canvas instead.

```
npm run palettes
```

## What is not here

- **Installers, and signing.** `python build.py dist` makes a directory, not
  an installer, a `.dmg` or an AppImage. Nothing is signed with a real
  certificate, so Windows SmartScreen warns the first time a downloaded copy of
  `YoAI.exe` is run, and macOS Gatekeeper refuses a downloaded `YoAI.app`
  until it is allowed. The macOS package has not yet been run on a Mac.
- **Voice input and bundles.** They are in the C ABI and deliberately not bound
  in [native/src/binding.cpp](native/src/binding.cpp) — nothing here calls them
  yet, and an export nobody uses is a second place to keep correct.
- **Markdown rendering.** Replies are shown as text, as they are on HarmonyOS.
