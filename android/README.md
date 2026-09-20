# The Android lane

A bench, not the app. One screen of measurements against the same
`bodyclock_core` the HarmonyOS lane runs, so a figure taken here and a figure in
[design/voice.md](../design/voice.md) are comparable.

The reason it exists is that every number in that file was taken on one chip, a
Kirin 9020, and several of its conclusions are about the chip rather than about
the engines — the codec decode that five attempts could not move, the throttle
step at thirty seconds, the bandwidth ceiling that bounds decode, and the GPU
that HarmonyOS killed by policy before it could be timed. None of those is
settled until a second device disagrees or agrees.

## What it measures

| panel | needs | reads |
| --- | --- | --- |
| Device | nothing | SoC, cluster shape, and the arm64 feature words — see **-march** below |
| Memory bandwidth | nothing | GB/s swept one thread at a time |
| Speech out | two GGUFs | per-stage timings and RTF, from the engine's own object |
| Recognition | a GGUF and a WAV | transcript, prosody, and RTF timed here |
| On-device LLM | a GGUF | prefill and decode split at the first `reply` event |

Everything prints the engine's own JSON under the one derived figure worth
reading at a glance. That is deliberate: design/voice.md is largely a record of
conclusions drawn from numbers that turned out to be something else, and the
cheapest defence is never hiding the object a figure came from.

## Building

```
cd android
./build.sh                # deps, the LLM engine, the APK -- the first run
./build.sh apk            # every run after that
./build.sh qtts           # add speech out
./build.sh vulkan         # add speech out with ggml's Vulkan backend, for Adreno
```

`build.cmd` is the same thing on Windows; it is a wrapper rather than a second
copy, because unlike the HarmonyOS lane nothing here needs native batch — the
NDK, vcpkg and the Gradle wrapper behave the same on all three platforms.

**A lane is on when its SDK exists.** `prebuilt/llama-sdk/<abi>/` turns on the
on-device LLM and recognition together; `prebuilt/qtts-sdk/<abi>/` turns on
speech out. Nothing in a tracked build file names a machine-local path, and the
Device panel prints which lanes came out on.

Android Studio is for editing and debugging, not a build dependency. Open
`android/` in it when you want it.

## -march is asserted, not detected

A cross build with no `-march` lands on baseline `armv8-a`, because CMake's
feature probes compile **and run** a test binary and cannot run one for the
phone. design/voice.md measures that costing 2.9x prefill and 2.2x decode.

So `build.sh` asserts a feature set, and its default is the Kirin's:
`armv8.6-a+i8mm+bf16+dotprod+fp16`. A current Qualcomm core is armv9.2-a and has
more. **The Device panel is how the assertion gets checked** — it prints what
the chip reports, and `sve2` showing up there against a build that never asked
for it is a lever nobody has pulled yet.

```
./build.sh llama arch=armv9.2-a+i8mm+bf16+dotprod+fp16+sve2
```

Keep the default when the point is comparing against the Kirin figures, and
change it when the point is finding this chip's ceiling. Those are different
questions and the same script answers both.

## Models

Pushed by hand, to a directory that needs no permission:

```
adb push <file> /sdcard/Android/data/ai.bodyclock.app/files/
```

The Models panel lists what is actually there, and the app's own path is
printed above it in case the package name ever changes. Nothing here downloads
or manages a model — this is a bench, and which checkpoint is on the device is
part of what is being recorded.

## What is not here

- **No chat, settings, providers or session storage.** The HarmonyOS lane has
  all of it and this one is not a port of that; it is the measurement half.
- **No recording.** Every clip in design/voice.md is a file, so the bench reads
  files and never asks for a microphone.
- **No playback.** `nativeTtsSay` takes an optional sink and the bench passes
  none: copying a megabyte of samples across the bridge to discard them would
  be part of what the stopwatch measured.
