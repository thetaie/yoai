#!/usr/bin/env python3
"""RTF of the four Q4_K_M Qwen3-TTS talkers on this machine, one backend at a time.

Usage: python tests/tts_rtf.py --models <dir> [--build <dir>] [--backend <name> ...]
                               [--passes <n>] [--out <dir>]

  --models   the directory holding the GGUFs, named as qwentts.cpp names them
             (see its README for where to get them or how to convert them):
               qwen-talker-0.6b-base-Q4_K_M.gguf
               qwen-talker-1.7b-base-Q4_K_M.gguf
               qwen-talker-0.6b-customvoice-Q4_K_M.gguf
               qwen-talker-1.7b-customvoice-Q4_K_M.gguf
               qwen-tokenizer-12hz-Q8_0.gguf
             A talker that is missing is skipped and said so.
  --build    where `python build.py tts` put yoai-tts-sweep (default: build/)
  --backend  CPU, auto (the engine's own pick: the best GPU when there is
             one), or a ggml device name such as Vulkan0 or CUDA0. Repeat it
             for several; the default is CPU and auto. A name the build does
             not have fails that column and nothing else.
  --passes   how many times each cell is run (default 2)
  --out      where each run's WAVs and log go (default: build/tts-rtf/)

It prints one line per run as it goes, then a table in the shape of
src/voice/slides.md's RTF slide, and the raw figures for design/voice.md.

The protocol is design/voice.md's "All four talkers, one session": a warm-up
sentence that is not counted, then three passages, each read as one say()
(yoai-tts-sweep --whole), at the desktop's reading settings -- seed 101,
talker temperature 1.3, the streaming text layout, a 2-frame eos hold. Base
reads with speaker/01_female's voice file, CustomVoice as `serena`. The CPU
runs at qwentts.cpp's default thread count, half the logical CPUs.

RTF is seconds of computation per second of speech, summed over the three
passages. Close what else is running, and keep a laptop on power: on the PC
where this was written the CPU figures move by up to 15% from one day to the
next, while two passes on the same day agree within 4%.

Python 3.8 or newer, standard library only.
"""

import argparse
import os
import platform
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEAKER = os.path.join(REPO, "speaker", "01_female")
TOKENIZER = "qwen-tokenizer-12hz-Q8_0.gguf"

# (label, talker file, voice file or None, speaker or None)
TALKERS = [
    ("0.6B Base", "qwen-talker-0.6b-base-Q4_K_M.gguf", "qwen3-tts-0.6b.spk", None),
    ("1.7B Base", "qwen-talker-1.7b-base-Q4_K_M.gguf", "qwen3-tts-1.7b.spk", None),
    ("0.6B CustomVoice", "qwen-talker-0.6b-customvoice-Q4_K_M.gguf", None, "serena"),
    ("1.7B CustomVoice", "qwen-talker-1.7b-customvoice-Q4_K_M.gguf", None, "serena"),
]

# The first synthesis on a GPU compiles its shaders, seconds of it, and would
# be billed to the first passage. Read once and not counted.
WARMUP = "你好。"

# tests/tts_sweep.cpp's three passages. The phone's copy is BENCH_PASSAGES in
# examples/harmony/entry/src/main/ets/pages/TtsDebug.ets; change all three together, or
# the figures stop comparing.
PASSAGES = [
    "说起黄浦江，很多人第一个想到的是外滩。其实这条江的故事比外滩长得多。"
    "它从淀山湖一带流出来，一路向东北，穿过上海市区，最后在吴淞口汇入长江。"
    "你知道吗？江上的轮渡到今天还在开。"
    "傍晚坐一趟，看着两岸的灯一盏盏亮起来，真的很美！",
    "我查了一下，明天上海多云转小雨，最高气温二十六度，最低十九度。"
    "早上出门的话，记得带把伞。另外，晚高峰地铁二号线可能会比较挤，"
    "你要是不赶时间，可以晚半小时再走。",
    "这个问题其实不难。简单来说，植物白天靠叶子里的叶绿素吸收阳光，"
    "把二氧化碳和水变成糖，同时放出氧气。到了晚上没有光，这个过程就停下来了，"
    "植物反而会像我们一样呼吸，消耗一点氧气。所以卧室里放太多花草，并不是一个好主意。",
]

SEED = "101"
TEMPERATURE = "1.3"
EOS_HOLD = "2"

# What the engine logs, and what is read back out of it.
SAID = re.compile(r"qtts: ([0-9.]+) s of speech in ([0-9.]+) s \(([0-9.]+)x\)")
BACKEND = re.compile(r"\[Load\] Talker backend: (\S+) \(CPU threads: (\d+)\)")
GPU = re.compile(r"qtts: GPU probe: (.+?) \(type \d+\)")
TUNING = re.compile(r"qtts: (integrated GPU|discrete GPU|GPU memory unknown) -- ")


def cpu_name():
    """The processor's marketing name, for the table header."""
    system = platform.system()
    try:
        if system == "Windows":
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                 r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        if system == "Darwin":
            return subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"],
                                           universal_newlines=True).strip()
        with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
            for line in f:
                key, _, value = line.partition(":")
                if key.strip() in ("model name", "Hardware", "Model"):
                    return value.strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    return platform.processor() or platform.machine()


def sweep_path(build_dir):
    name = "yoai-tts-sweep.exe" if platform.system() == "Windows" else "yoai-tts-sweep"
    return os.path.join(build_dir, name)


def run(sweep, build_dir, models, talker, backend, passages_file, out_dir):
    """One talker on one backend, once. Returns (result, None) or (None, why)."""
    label, talker_file, spk, speaker = talker
    env = dict(os.environ)
    env.pop("GGML_BACKEND", None)
    if backend != "auto":
        env["GGML_BACKEND"] = backend
    # The engine's DLLs sit beside the tool on Windows.
    env["PATH"] = build_dir + os.pathsep + env.get("PATH", "")

    cmd = [sweep, "--whole", "--streaming-text", "--eos-hold", EOS_HOLD, "--seeds", SEED,
           "--passages", passages_file]
    if speaker:
        cmd += ["--speaker", speaker]
    cmd += [os.path.join(models, talker_file), os.path.join(models, TOKENIZER),
            os.path.join(SPEAKER, spk) if spk else "-", out_dir, TEMPERATURE]

    os.makedirs(out_dir, exist_ok=True)
    r = subprocess.run(cmd, cwd=build_dir, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True,
                       encoding="utf-8", errors="replace")
    log = r.stdout
    with open(os.path.join(out_dir, "log.txt"), "w", encoding="utf-8") as f:
        f.write(log)

    said = [(float(a), float(b)) for a, b, _ in SAID.findall(log)]
    if r.returncode != 0 or len(said) != 1 + len(PASSAGES):
        tail = "\n".join(log.strip().splitlines()[-8:])
        return None, "exit %d, %d of %d utterances\n%s" % (
            r.returncode, len(said), 1 + len(PASSAGES), tail)
    body = said[1:]
    speech = sum(s for s, _ in body)
    seconds = sum(t for _, t in body)
    backend_line = BACKEND.search(log)
    gpu = GPU.search(log)
    tuning = TUNING.search(log)
    return {
        "rtf": seconds / speech,
        "per_passage": [t / s for s, t in body],
        "speech": speech,
        "device": backend_line.group(1) if backend_line else backend,
        "threads": backend_line.group(2) if backend_line else "?",
        "gpu": gpu.group(1).strip() if gpu else "",
        "tuning": tuning.group(1) if tuning else "",
    }, None


def dash():
    """The slides' en dash, or a hyphen on a console that cannot print one."""
    try:
        "–".encode(sys.stdout.encoding or "ascii")
        return "–"
    except (UnicodeEncodeError, LookupError):
        return "-"


def cell(values):
    """min-max of a cell's passes at two places, one number when they agree."""
    if not values:
        return "failed"
    lo, hi = "%.2f" % min(values), "%.2f" % max(values)
    return lo if lo == hi else "%s%s%s" % (lo, dash(), hi)


def column_name(backend, results):
    """A column header from what the engine reported it ran on."""
    seen = [r for r in results if r]
    if not seen:
        return backend
    r = seen[0]
    if r["device"] == "CPU":
        return "%s, CPU, %s threads" % (cpu_name(), r["threads"])
    name = r["gpu"] or r["device"]
    # The tuning line is the Vulkan backend's environment, logged on every load
    # of a build that has Vulkan in it -- a CUDA column would inherit it.
    vulkan = r["device"].startswith("Vulkan")
    return "%s (%s)" % (name, r["device"]) + (", " + r["tuning"] if vulkan and r["tuning"] else "")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--models", required=True)
    p.add_argument("--build", default=os.path.join(REPO, "build"))
    p.add_argument("--backend", action="append")
    p.add_argument("--passes", type=int, default=2)
    p.add_argument("--out")
    a = p.parse_args()

    backends = a.backend or ["CPU", "auto"]
    build_dir = os.path.abspath(a.build)
    models = os.path.abspath(a.models)
    out_root = os.path.abspath(a.out or os.path.join(build_dir, "tts-rtf"))
    sweep = sweep_path(build_dir)
    if not os.path.isfile(sweep):
        sys.exit("no %s -- build it with `python build.py tts`" % sweep)
    if not os.path.isfile(os.path.join(models, TOKENIZER)):
        sys.exit("no %s in %s" % (TOKENIZER, models))

    talkers = []
    for t in TALKERS:
        if os.path.isfile(os.path.join(models, t[1])):
            talkers.append(t)
        else:
            print("skipped %s: no %s in %s" % (t[0], t[1], models), flush=True)
    if not talkers:
        sys.exit("none of the four talkers is in %s" % models)

    # yoai-tts-sweep reads one passage per line; the warm-up comes first.
    fd, passages_file = tempfile.mkstemp(suffix=".txt", prefix="tts-rtf-")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write("\n".join([WARMUP] + PASSAGES) + "\n")

    # results[backend][label] = [result or None, one per pass]
    results = {b: {t[0]: [] for t in talkers} for b in backends}
    try:
        for n in range(a.passes):
            for backend in backends:
                for t in talkers:
                    out = os.path.join(out_root, "pass%d" % n, backend,
                                       t[0].replace(" ", "_"))
                    r, why = run(sweep, build_dir, models, t, backend, passages_file, out)
                    results[backend][t[0]].append(r)
                    if r is None:
                        print("pass %d  %-8s %-17s FAILED: %s" % (n, backend, t[0], why),
                              flush=True)
                        continue
                    print("pass %d  %-8s %-17s on %-8s RTF %.3f  per passage %s  (%.1fs of speech)"
                          % (n, backend, t[0], r["device"], r["rtf"],
                             " ".join("%.2f" % x for x in r["per_passage"]), r["speech"]),
                          flush=True)
    finally:
        os.remove(passages_file)

    print()
    print("%s, %s, Python %s" % (platform.platform(), cpu_name(), platform.python_version()))
    print()
    heads = [column_name(b, [r for rs in results[b].values() for r in rs]) for b in backends]
    print("| `Q4_K_M` | " + " | ".join(heads) + " |")
    print("| --- | " + " | ".join("---" for _ in backends) + " |")
    for t in talkers:
        cells = [cell([r["rtf"] for r in results[b][t[0]] if r]) for b in backends]
        print("| %s | %s |" % (t[0], " | ".join(cells)))
    print()
    print("Raw, one figure per pass:")
    for b in backends:
        for t in talkers:
            print("  %-8s %-17s %s" % (b, t[0], ", ".join(
                "%.3f" % r["rtf"] if r else "failed" for r in results[b][t[0]])))
    print()
    print("WAVs and logs under %s" % out_root)


if __name__ == "__main__":
    main()
