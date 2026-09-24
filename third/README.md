# third/

Everything this repo builds against that somebody else wrote. Two kinds, kept
differently:

- **Vendored.** Small and stable, committed here, and used as-is. Each has a
  README saying which version it is and where it came from.
- **Checked out on demand.** Four large trees that live *outside* the repo —
  [below](#why-the-checkouts-are-siblings-not-children) says why — recorded in
  `manifest.txt` with the patches we carry against them, and kept in step by
  `sync`.

## Vendored

| | what it gives us | used by |
| --- | --- | --- |
| [**sqlite3/**](sqlite3/README.md) | the SQLite amalgamation, compiled straight into the core | `CMakeLists.txt` |
| [**rapidjson/**](rapidjson/README.md) | RapidJSON's headers, pinned to one upstream commit | `CMakeLists.txt`, for `src/llm` and `src/platform` |
| [**lucide/**](lucide/README.md) | a pinned copy of the icon set; the source of truth for icon path data | the apps' icon tables, via [design/icons.md](../design/icons.md) |

## Checked out on demand

```
sync.cmd check      what is on this machine, and how it differs from the record
sync.cmd clone      fetch whatever is missing, at the pinned commit, patched
sync.cmd update     move everything to its branch tip and re-pin
sync.cmd verify     silent when on the pin and patched; else say what is off, exit 1
```

`verify` is what the build scripts run before they compile a checkout. It
never stops a build: the build prints a warning and carries on, because a tree
ahead of its pin is a normal state while trying something.

`sync.sh` is the same script for POSIX shells. Both read `manifest.txt`; both
write it back on `update`. A name limits either to one dependency:
`sync.cmd update qwentts.cpp`.

| | what it gives us | patched |
| --- | --- | --- |
| **llama.cpp** | the on-device LLM, **and the one ggml every lane compiles** — SenseVoice ASR included, which is our own code in `src/voice/asr/` on top of that ggml | 1 |
| **qwentts.cpp** | Qwen3-TTS: talker + codec, the whole speech-out half | 1 |
| **MNN** | the Qwen2.5-Omni evaluation lane. Nothing ships against it | 4 |
| **vcpkg** | the C++ dependencies every lane's `deps` target installs | — |

llama.cpp is load-bearing twice over. Every build now passes qwentts.cpp
`-DGGML_SOURCE_DIR=$LLAMA_SRC/ggml`, so the two lanes compile one ggml from one
commit instead of two that drift apart — which they did, and which stopped the
desktop addon linking. `design/voice.md` has the symbol tables. The practical
consequence here: **`qtts` needs a llama.cpp checkout even when `llama` is not
being built**, and moving llama's pin moves the ggml underneath speech output
as well as under the LLM.

## Why the checkouts are siblings, not children

They are multi-gigabyte trees that carry their own build output. One
`git clean -xdf` in this repo would delete all four, and a `.gitignore` entry is
not protection — it is exactly what makes `clean -x` eat them. So they sit
beside the repo:

```
projects/
  llama.cpp/   MNN/   qwentts.cpp/   vcpkg/
  yoai/        <- this repo
```

One checkout at a time is moved by the same setting its build script reads —
`YOAI_LLAMA_SRC`, `YOAI_MNN_SRC`, `YOAI_QTTS_SRC`, `YOAI_VCPKG_ROOT`, in `.env`
at the repository root (see `.env.example`) or in the environment. Both
scripts load it through `scripts/load-env.*`, so there is no second place to
configure and `sync.cmd` and `build.cmd` cannot end up pointing at different
trees.

`THIRD_ROOT` moves the whole directory for `sync` only; the build scripts do
not read it and still look beside the repo. Use it together with the four
settings above, or not at all.

## The pin

`manifest.txt` records a commit, not a tag or a branch. The lane that depends on
this most is qwentts.cpp, where a single upstream change to the stage split
would move every number in `design/voice.md` without anything failing to build.

`sync.cmd check` compares the checkout against the pin and says so. It does not
enforce it: a tree ahead of the pin is a normal state to be in while trying
something. `update` is what makes it official, and it rewrites `manifest.txt`
so the pin cannot silently rot behind the checkout.

## The patches

Seven, all in `patches/`, all against somebody else's tree. Each file starts
with the reasoning; `manifest.txt` says which work tree each one applies in
(`file@.` for the repo root, `file@ggml` for a submodule).

| | | applied |
| --- | --- | :---: |
| `llama-ggml-ohos-affinity.patch` | OHOS thread affinity: `_GNU_SOURCE` for `cpu_set_t`, and `sched_setaffinity` because musl has no `pthread_setaffinity_np` | ✓ |
| `qwentts-thread-count-override.patch` | `QWEN_N_THREADS`, so the CPU thread count can be measured rather than assumed | ✓ |
| `qwentts-ggml-ohos-affinity.patch` | the same affinity fix against qwentts's own ggml submodule | — |
| `mnn-talker-context.patch` | `getTalkerContext()`, so the Talker's prefill, decode and token2wav time reach a host and the cost of speech can be split | ✓ |
| `mnn-vulkan-static-register.patch` | a no-op symbol to reference, so a static libMNN keeps its Vulkan backend instead of silently falling back to the CPU | ✓ |
| `mnn-vulkan-device-limits.patch` | two Vulkan limits the backend ignored — storage-buffer offset alignment, and 16-bit access to uniform blocks — found as `VK_ERROR_DEVICE_LOST` on an Intel Arc | ✓ |
| `mnn-export-talker-quant-bit.patch` | `--talker_quant_bit` for the exporter, which otherwise hard-codes the Talker's weight bits by model size | ✓ |

`qwentts-ggml-ohos-affinity.patch` is kept and not applied. Since every lane builds llama.cpp's ggml
through `GGML_SOURCE_DIR`, the submodule is not compiled and patching it would
dirty a tree nobody reads. It stays because pointing `GGML_SOURCE_DIR` back at
the submodule is a one-flag experiment, and the fix would be needed again the
moment anyone runs it on OHOS.

They are applied here rather than committed upstream because the checkouts are
upstream's and get updated. `sync.cmd` reverses them before a fetch and
re-applies afterwards, so a patch that no longer fits **stops** that dependency
rather than leaving it half-moved. That is the case worth designing for: a
`git pull` over a patched file either conflicts or silently merges, and the
second one is discovered during a build three days later.

Idempotence is `git apply --reverse --check`, not a marker file. It succeeds
only when the change is already present, so running twice is a no-op, a
checkout that moved under a patch fails loudly, and a file somebody edited by
hand is detected rather than patched on top of.

## Why this directory exists at all

The two affinity patches are the same two-line fix for the same sysroot. The
llama.cpp one was found *already applied, by hand, undocumented*, while writing
this directory — and it was the finished version of a fix the qwentts lane had
settled for a worse answer to. Nothing recorded that it was there, so nothing
compared them. See "Nineteen misses" in [../design/voice.md](../design/voice.md).

A checkout that carries undocumented edits is a build that only reproduces on
one machine. `check` is the cheap version of noticing: it reports modified and
untracked paths beyond the patches, as a prompt to look rather than a failure,
because a patched tree is *supposed* to be dirty.

## Adding one

**A checkout:** append a line to `manifest.txt` — the columns are in its
header — and, if the build script reads an env var for it, use that name in
the `env` column. Write `-` for an empty column rather than leaving it blank:
`sync.cmd` splits on `|` with `for /f`, which collapses consecutive delimiters
and would shift every column after the gap. A new variable also goes in the
list in `scripts/load-env.{cmd,sh}` and in `.env.example`; without that, a
`YOAI_` setting for it in `.env` is read and then ignored.

**A vendored copy:** commit it as `third/<name>/` with its LICENSE and a README
that says which version it is and where it came from. It needs no manifest
line, because there is nothing to fetch.
