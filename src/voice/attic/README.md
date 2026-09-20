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

Measured on the same phone:

| | omni | qtts |
| --- | --- | --- |
| Real time factor | 3.70 at the shipped sampler, 2.8 at its coarsest — **Talker only** | 2.14 fast, 3.52 sustained, whole synthesis |
| Has to think first | yes, and it is 17.2 s of the 46.2 s exchange | no, the text is llama.cpp's |
| Size | 3.8 GB | 1.4 GB |
| Voice | the model's own, with no control over it | written, and stable across passages |
| Engine | a second framework, MNN | ggml, already here for two other lanes |

**It did not lose on speed**, and an earlier version of this file said it did by
putting omni's coarsest setting beside qtts's fast-regime number. Those measure
different things: omni's excludes the thinker, and its Talker ran 37.5 s
continuously, so most of it was past the clock step that qtts's 2.14 is entirely
before. Against qtts's sustained 3.52, omni at 2.8 was the faster of the two —
while producing audible artefacts, which is what spending its only lever costs.

It lost on 3.8 GB against 1.4, on a second inference framework against the ggml
already linked, on having to think before it can speak, and most of all on
having no way to ask for a voice.

Removing it from the build also retires the include-order workaround that kept
two copies of rapidjson from colliding, which was the cause of one SIGSEGV that
looked like a memory bug for a day. `design/voice.md` keeps that story and every
number above; this directory keeps the code they were taken with.

**It is still the only end-to-end answer here.** The shipping chain is three
stages — recognise, think, speak — and omni is the one thing that was not. If
that ever matters again, this is cheaper than writing it twice, though the MNN
SDK would have to be rebuilt (`harmony/build.cmd <abi> mnn`).
