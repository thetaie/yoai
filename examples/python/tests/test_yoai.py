#!/usr/bin/env python3
"""Usage: python test_yoai.py [--talker GGUF --codec GGUF] [--asr GGUF] [--gguf GGUF]

Checks the INSTALLED yoai package -- the wheel examples/python/build.py makes,
after pip install -- against the core it carries. With no options it needs no
model and no network: the binding itself, the day log, the prompts, the
provider registry, a turn streamed from a local stand-in for a provider and
two that fail, and prosody on a synthetic tone. Each option adds a lane, run
on the model given:

  --talker, --codec   speech output: one sentence spoken, then the reader
                      speaking text fed to it in pieces
  --asr               recognition; with --talker and --codec, that sentence
                      transcribed back
  --gguf              on-device chat: one short turn

Exits 0 when every check passed, 1 otherwise.

Python 3.8 or newer, standard library only.
"""

import argparse
import array
import math
import os
import sys
import tempfile
import time

import yoai

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")
failures = []


def expect(ok, what):
    # Not `assert`, which python -O removes.
    if not ok:
        raise AssertionError(what)


def check(name, fn):
    try:
        detail = fn()
    except Exception as e:
        failures.append(name)
        print("FAIL %s: %s: %s" % (name, type(e).__name__, e), flush=True)
    else:
        print("ok   %s%s" % (name, "  (%s)" % detail if detail else ""), flush=True)


def raises(fn):
    """The yoai.Error `fn` raises; failing the check when it raises none."""
    try:
        fn()
    except yoai.Error as e:
        expect(str(e), "the error has no message")
        return e
    raise AssertionError("no yoai.Error raised")


# --- No model, no network ----------------------------------------------------

def every_function_bound():
    header = os.path.join(ROOT, "src", "yoai.h")
    if not os.path.isfile(header):
        return "skipped: no src/yoai.h beside this script"
    import re
    with open(header, encoding="utf-8") as f:
        names = set(re.findall(r"^[A-Za-z][^/\n]*[ *]yoai_([a-z0-9_]+)\(", f.read(), re.M))
    missing = sorted(n for n in names if not callable(getattr(yoai, n, None)))
    expect(not missing, "not bound: " + ", ".join(missing))
    return "%d functions, version %s, from %s" % (len(names), yoai.version(), os.path.dirname(yoai.__file__))


def timeline():
    with tempfile.TemporaryDirectory() as d:
        yoai.timeline_open(d)
        try:
            expect(yoai.timeline_opened(), "not opened")
            yoai.timeline_switch_to("work")
            yoai.timeline_note_add("noodles")
            now = yoai.timeline_now()
            expect(now["current"]["stateId"] == "work", "current is %r" % now["current"])
            expect(now["current"]["notes"][0]["text"] == "noodles", "the note is missing")
            yoai.timeline_end()
            now = yoai.timeline_now()
            expect(now["current"] is None and now["lastEnded"]["stateId"] == "work",
                   "after end: %r" % now)
            yoai.timeline_undo()
            expect(yoai.timeline_now()["current"]["stateId"] == "work", "undo did not reopen")
            yoai.timeline_end()
            e = raises(lambda: yoai.timeline_note_add("while idle"))
            today = yoai.timeline_today()
            iso = yoai.timeline_day_to_iso(today)
            expect(yoai.timeline_iso_to_day(iso) == today, "%s does not round-trip" % iso)
            expect(len(yoai.timeline_day(today)) == 1, "today has no entry")
        finally:
            yoai.timeline_close()
    return "idle note refused: %s" % e


def prompts():
    table = yoai.prompts()
    names = [p["name"] for p in table]
    expect("system" in names, "no system prompt in %s" % names)
    expect(yoai.prompt_get("system"), "the system prompt is empty")
    e = raises(lambda: yoai.prompt_get("no-such-prompt"))
    expect(isinstance(yoai.prompt_turn_system(None, 1), str), "no turn system prompt")
    return "%d prompts; unknown one: %s" % (len(names), e)


def preview():
    msgs = [{"role": "user", "content": "hello",
             "annotations": {"time": int(time.time() * 1000), "utcOffset": 480}}]
    seen = yoai.chat_preview(msgs)
    expect(seen[0]["content"].endswith("hello") and seen[0]["content"] != "hello",
           "annotations not written in: %r" % seen[0]["content"])
    return repr(seen[0]["content"])


def registry():
    yoai.provider_set("test/m", "http://127.0.0.1:9", "key", "m", {"stream": True})
    expect("test/m" in yoai.providers(), "not listed")
    expect(yoai.provider_remove("test/m") is True, "remove did not find it")
    expect(yoai.provider_remove("test/m") is False, "removed twice")
    raises(lambda: yoai.provider_set("test/m", None, None, None))
    yoai.provider_clear()
    expect(yoai.providers() == [], "clear left %s" % yoai.providers())


def streamed_turn():
    """A whole turn against a local stand-in for an OpenAI-compatible
    endpoint: what the core sends, and every kind of event coming back."""
    import http.server
    import json
    import threading
    chunks = [{"choices": [{"delta": {"reasoning_content": "Thinking."}}]},
              {"choices": [{"delta": {"content": "Hel"}}]},
              {"choices": [{"delta": {"content": "lo"}}]},
              {"choices": [], "usage": {"prompt_tokens": 5, "completion_tokens": 2}}]
    requests = []

    class Endpoint(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            requests.append({"path": self.path, "auth": self.headers.get("Authorization"),
                             "body": json.loads(self.rfile.read(int(self.headers["Content-Length"])))})
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for c in chunks:
                self.wfile.write(("data: %s\n\n" % json.dumps(c)).encode("utf-8"))
                self.wfile.flush()
            self.wfile.write(b"data: [DONE]\n\n")

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Endpoint)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        yoai.provider_set("test/sse", "http://127.0.0.1:%d/v1" % server.server_port, "secret", "m")
        events = list(yoai.chat("test/sse", "hi", "Be brief."))
        # Leaving after the first event cancels the turn, and must neither
        # raise nor leave the turn's thread behind.
        turn = yoai.chat("test/sse", "hi")
        next(turn)
        turn.close()
    finally:
        yoai.provider_remove("test/sse")
        server.shutdown()
        server.server_close()
    expect(len(requests) == 2, "%d requests" % len(requests))
    seen = requests[0]
    expect(seen["path"] == "/v1/chat/completions", "posted to %r" % seen["path"])
    expect(seen["auth"] == "Bearer secret", "Authorization %r" % seen["auth"])
    messages = seen["body"]["messages"]
    expect(messages[-1]["content"] == "hi", "last message %r" % messages[-1])
    expect(any(m["role"] == "system" and "Be brief." in m["content"] for m in messages),
           "no system prompt in %r" % messages)
    reply = "".join(c for k, c in events if k == "reply")
    thinking = "".join(c for k, c in events if k == "thinking")
    usage = [c for k, c in events if k == "usage"]
    expect(reply == "Hello", "reply %r" % reply)
    expect(thinking == "Thinking.", "thinking %r" % thinking)
    expect(len(usage) == 1 and usage[0]["outputTokens"] == 2, "usage %r" % usage)
    return "%d events: %s" % (len(events), ", ".join(k for k, _ in events))


def failed_turn():
    unknown = raises(lambda: list(yoai.chat("no/such", "hi")))
    # Port 9 is discard, and nothing listens on it here: a transport failure.
    yoai.provider_set("test/refused", "http://127.0.0.1:9", "key", "m")
    try:
        refused = raises(lambda: list(yoai.chat("test/refused", "hi")))
    finally:
        yoai.provider_remove("test/refused")
    return "unknown: %s | refused: %s" % (unknown, refused)


def prosody():
    # Three seconds of a tone that swells and fades twice a second, which
    # frames into utterances; what it measures does not matter here.
    rate = 16000
    pcm = array.array("h", (int(8000 * (0.5 + 0.5 * math.sin(2 * math.pi * 2 * i / rate)) *
                                math.sin(2 * math.pi * 220 * i / rate)) for i in range(3 * rate)))
    result = yoai.voice_analyze(pcm)
    expect(abs(result["audioSeconds"] - 3.0) < 0.01, "audioSeconds %r" % result.get("audioSeconds"))
    raises(lambda: yoai.voice_analyze(pcm, 8000))
    return "keys: %s" % ", ".join(sorted(result))


def bandwidth():
    result = yoai.memory_bandwidth(1, 16, 1)
    expect(result["gbPerSecond"] > 0, "no figure: %r" % result)
    return "%.1f GB/s on one thread" % result["gbPerSecond"]


def not_a_bundle():
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(b"not a bundle")
    try:
        return str(raises(lambda: yoai.bundle_list(f.name)))
    finally:
        os.remove(f.name)


# --- With a model ------------------------------------------------------------

SENTENCE = "今天天气很好，我们去公园散步吧。"


def speak(talker, codec):
    expect(yoai.tts_available(), "this build has no speech output")
    yoai.tts_load(talker, codec)
    kind = yoai.tts_model_type()
    if kind == "custom_voice":
        yoai.tts_set_speaker(yoai.tts_speakers()[0])
    elif kind == "voice_design":
        yoai.tts_set_voice("a calm adult voice, unhurried")
    else:
        raise AssertionError("a %s talker needs a reference recording; pass a "
                             "customvoice or voicedesign one" % kind)
    yoai.tts_set_seed(42)
    chunks, lasts = [], []

    def on_audio(pcm, last):
        chunks.append(pcm)
        lasts.append(last)

    result = yoai.tts_say(SENTENCE, on_audio)
    samples = array.array("f")
    for c in chunks:
        samples.extend(c)
    expect(lasts and lasts[-1] and not any(lasts[:-1]), "last arrived %r" % lasts)
    expect(abs(len(samples) / 24000.0 - result["speechSeconds"]) < 0.05,
           "%d samples against %.2f s" % (len(samples), result["speechSeconds"]))
    speak.samples = samples
    return "%s, %.2f s of speech at %.2fx real time, %d chunks" % (
        kind, result["speechSeconds"], result["realtime"], len(chunks))


def read_aloud(talker, codec):
    """The reader, whose callback runs on threads of the core's own: text fed
    in pieces, as a model writes it."""
    import threading
    yoai.tts_unload()  # one of the two engines at a time, as yoai.h asks
    yoai.reader_load(talker, codec, 1)
    try:
        kind = yoai.reader_model_type()
        if kind == "custom_voice":
            yoai.reader_set_speaker(yoai.reader_speakers()[0])
        elif kind == "voice_design":
            yoai.reader_set_voice("a calm adult voice, unhurried")
        else:
            raise AssertionError("a %s talker needs a reference recording" % kind)
        threads, counts = set(), {"samples": 0, "last": 0}

        def on_audio(pcm, sentence, last):
            threads.add(threading.get_ident())
            counts["samples"] += len(pcm)
            counts["last"] += last

        yoai.reader_begin(on_audio)
        for piece in ("今天天气很好，", "我们去公园散步吧。", "你觉得怎么样？"):
            yoai.reader_feed(piece)
        result = yoai.reader_finish()
    finally:
        yoai.reader_unload()
    expect(counts["last"] == 1, "last arrived %d times" % counts["last"])
    expect(abs(counts["samples"] / 24000.0 - result["speechSeconds"]) < 0.05,
           "%d samples against %.2f s" % (counts["samples"], result["speechSeconds"]))
    return "pieces: %d, %.2f s of speech, handed over on %d thread(s)%s" % (
        result["sentences"], result["speechSeconds"], len(threads),
        ", the caller's among them" if threading.get_ident() in threads else "")


def to_16k(samples):
    """24000 Hz float to 16000 Hz 16-bit, by linear interpolation. Good enough
    to be recognised; nothing here measures prosody on it."""
    out = array.array("h")
    n = len(samples)
    for j in range(n * 2 // 3):
        x = j * 1.5
        i = int(x)
        t = x - i
        v = samples[i] * (1 - t) + (samples[i + 1] if i + 1 < n else 0.0) * t
        out.append(max(-32768, min(32767, int(v * 32767))))
    return out


def recognise(model):
    expect(yoai.voice_available(), "this build has no recognition")
    yoai.voice_load(model)
    expect(yoai.voice_loaded(), "not loaded")
    samples = getattr(speak, "samples", None)
    if samples is None:
        return "loaded; nothing to transcribe without --talker and --codec"
    result = yoai.voice_analyze(to_16k(samples))
    text = result["transcript"]["text"]
    expect(text, "empty transcript")
    return repr(text)


def local_turn(model):
    expect(yoai.local_available(), "this build has no on-device engine")
    yoai.local_set("test/local", model, {"threads": 4})
    try:
        reply, usage = [], None
        for kind, content in yoai.chat("test/local", "Say hello in one word."):
            if kind == "reply":
                reply.append(content)
            elif kind == "usage":
                usage = content
        expect("".join(reply).strip(), "empty reply")
        expect(usage and usage["outputTokens"] > 0, "usage %r" % usage)
        return "%r, %d tokens" % ("".join(reply).strip(), usage["outputTokens"])
    finally:
        yoai.provider_remove("test/local")


def main():
    ap = argparse.ArgumentParser(description="Check the installed yoai package.")
    ap.add_argument("--talker", help="Qwen3-TTS talker GGUF")
    ap.add_argument("--codec", help="Qwen3-TTS 12 Hz tokenizer GGUF")
    ap.add_argument("--asr", help="SenseVoice GGUF")
    ap.add_argument("--gguf", help="chat model GGUF")
    args = ap.parse_args()
    if bool(args.talker) != bool(args.codec):
        ap.error("--talker and --codec go together")

    print("lanes: on-device %s, recognition %s, speech output %s, omni %s" % tuple(
        "yes" if f() else "no" for f in (yoai.local_available, yoai.voice_available,
                                        yoai.tts_available, yoai.omni_available)))
    check("every function bound", every_function_bound)
    check("day log", timeline)
    check("prompts", prompts)
    check("chat preview", preview)
    check("provider registry", registry)
    check("streamed turn", streamed_turn)
    check("failed turn", failed_turn)
    check("prosody", prosody)
    check("memory bandwidth", bandwidth)
    check("not a bundle", not_a_bundle)
    if args.talker:
        check("speech output", lambda: speak(args.talker, args.codec))
    if args.asr:
        check("recognition", lambda: recognise(args.asr))
    if args.talker:
        check("reading aloud", lambda: read_aloud(args.talker, args.codec))
    if args.gguf:
        check("on-device turn", lambda: local_turn(args.gguf))

    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
