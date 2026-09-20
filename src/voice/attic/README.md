# attic

Code kept for its measurements, not built.

Nothing under this directory is in any target. The root `CMakeLists.txt` does
not list it, and the entry points that used to reach it are commented out where
they stood — in `src/bodyclock.h`, `src/platform/c_api.cpp`,
`harmony/entry/src/main/cpp/napi_init.cpp` and the `/omni` route in
`Index.ets` — so the shape of what was removed is visible next to what
replaced it.

## omni

Qwen2.5-Omni on MNN: audio in, an answer out, spoken in the model's own voice.
A second inference engine carried for one capability, and the first serious
attempt at the thing `src/voice/qtts.*` now does.

It lost on every axis, measured on the same phone:

| | omni | qtts |
| --- | --- | --- |
| Real time factor | 2.8×, at the coarsest sampler that still sounds like anything | 2.14× in the fast regime |
| Has to think first | yes — the chain measured 1.4 s of silence | no, the text is llama.cpp's |
| Size | 3.8 GB | 1.4 GB |
| Voice | the model's own, with no control over it | written, and stable across passages |
| Engine | a second framework, MNN | ggml, already here for two other lanes |

Removing it from the build also retires the include-order workaround that kept
two copies of rapidjson from colliding, which was the cause of one SIGSEGV that
looked like a memory bug for a day. `design/voice.md` keeps that story and every
number above; this directory keeps the code they were taken with.

**It is still the only end-to-end answer here.** The shipping chain is three
stages — recognise, think, speak — and omni is the one thing that was not. If
that ever matters again, this is cheaper than writing it twice, though the MNN
SDK would have to be rebuilt (`harmony/build.cmd <abi> mnn`).
