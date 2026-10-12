"""The YoAI core, through ctypes.

Every function in src/yoai.h is here without its yoai_ prefix --
yoai_timeline_open is yoai.timeline_open -- and takes the same arguments in
the same order. What each one does and returns is in yoai.h and is not
restated here. What this file adds is the marshalling:

  - str in, str out. Text goes to the core as UTF-8; a returned string is
    copied and decoded at once, since the core reuses its buffer on the next
    call from the same thread.
  - A JSON argument -- messages, options, extra request fields -- takes a dict
    or a list as well as a string, and a JSON result comes back parsed.
  - A failure raises yoai.Error instead of returning a code, with the core's
    own reason where it keeps one (yoai_timeline_last_error and its siblings).
  - A callback is a Python function, and yoai.chat() is a generator.

yoai.lib is the library itself, every signature declared, for anything this
leaves out.

Python 3.8 or newer, standard library only.
"""

import array
import ctypes
import json
import os
import queue
import sys
import threading
from ctypes import (CFUNCTYPE, POINTER, c_char_p, c_double, c_float, c_int,
                    c_longlong, c_short, c_void_p)

_HERE = os.path.dirname(os.path.abspath(__file__))

if sys.platform == "win32":
    _NAME = "yoai.dll"
elif sys.platform == "darwin":
    _NAME = "libyoai.dylib"
else:
    _NAME = "libyoai.so"


def _load():
    path = os.path.join(_HERE, _NAME)
    try:
        if sys.platform == "win32":
            # LOAD_WITH_ALTERED_SEARCH_PATH: what yoai.dll imports is looked
            # for beside it first -- curl, OpenSSL and zlib are there -- and
            # then where Windows looks for any program's DLLs, PATH included,
            # which is where the CUDA Toolkit puts its runtime. Python's own
            # default since 3.8 leaves PATH out, and a build with CUDA would
            # not load.
            return ctypes.CDLL(path, winmode=0x00000008)
        return ctypes.CDLL(path)
    except OSError as e:
        raise ImportError("could not load %s: %s. A build with CUDA also needs the CUDA "
                          "runtime installed." % (path, e))


lib = _load()


class Error(Exception):
    """A call into the core failed. The message carries the core's reason
    where it gave one."""


# -----------------------------------------------------------------------------
# Marshalling
# -----------------------------------------------------------------------------

def _text(value):
    """str, bytes or a path as a char*; None as NULL."""
    if value is None or isinstance(value, bytes):
        return value
    return os.fspath(value).encode("utf-8")


def _json(value):
    """A dict or a list as JSON text; a str or bytes is taken to be JSON
    already."""
    if value is None or isinstance(value, (str, bytes)):
        return _text(value)
    return json.dumps(value, ensure_ascii=False).encode("utf-8")


def _str(raw):
    return None if raw is None else raw.decode("utf-8", "replace")


# Where a lane keeps the reason for its last failure. Any other failure is
# reported by the function's name and what it returned.
_LAST_ERROR = (("timeline_", "yoai_timeline_last_error"),
               ("aec_", "yoai_aec_last_error"),
               ("tts_", "yoai_tts_last_error"),
               ("reader_", "yoai_reader_last_error"),
               ("prompt_get", "yoai_prompt_last_error"))


def _fail(name, what):
    for prefix, getter in _LAST_ERROR:
        if name.startswith(prefix):
            reason = _str(getattr(lib, getter)())
            if reason:
                raise Error("yoai_%s: %s" % (name, reason))
    raise Error("yoai_%s %s" % (name, what))


# Argument kinds: the C type, and how a Python value becomes one.
_ARGS = {
    "s": (c_char_p, _text),
    "j": (c_char_p, _json),
    "i": (c_int, int),
    "q": (c_longlong, int),
    "f": (c_float, float),
}

# Result kinds, each with its C type:
#   v  void            i  int, as it is       b  int, read as yes or no
#   f  float           s  string, None for NULL
#   S  string, NULL raises                    j  JSON, parsed; NULL raises
#   z  0 is success, anything else raises     n  1 is success, 0 raises
_RESULTS = {"v": None, "i": c_int, "b": c_int, "f": c_float, "s": c_char_p,
            "S": c_char_p, "j": c_char_p, "z": c_int, "n": c_int}


def _result(name, kind, raw):
    if kind == "b":
        return raw != 0
    if kind == "s":
        return _str(raw)
    if kind in ("S", "j"):
        if raw is None:
            _fail(name, "returned NULL")
        return json.loads(_str(raw)) if kind == "j" else _str(raw)
    if kind == "z":
        if raw != 0:
            _fail(name, "returned %d" % raw)
        return None
    if kind == "n":
        if raw == 0:
            _fail(name, "returned 0")
        return None
    return raw


def _declare(name, result, args):
    """yoai_<name> as a Python function. `args` is one kind per argument; a
    `?` after one makes it optional, defaulting to NULL or 0, which yoai.h
    reads as "the default" wherever it allows leaving one out."""
    fn = getattr(lib, "yoai_" + name)
    kinds, optional = [], []
    for ch in args:
        if ch == "?":
            optional[-1] = True
        else:
            kinds.append(ch)
            optional.append(False)
    fn.restype = _RESULTS[result]
    fn.argtypes = [_ARGS[k][0] for k in kinds]
    required = optional.count(False)

    def call(*values):
        if not required <= len(values) <= len(kinds):
            raise TypeError("yoai.%s takes %s arguments, %d given" % (
                name, required if required == len(kinds) else
                "%d to %d" % (required, len(kinds)), len(values)))
        values = values + (None,) * (len(kinds) - len(values))
        c_args = []
        for kind, value in zip(kinds, values):
            if value is None and kind in "iqf":
                value = 0
            c_args.append(_ARGS[kind][1](value))
        return _result(name, result, fn(*c_args))

    call.__name__ = call.__qualname__ = name
    call.__doc__ = "yoai_%s(), as src/yoai.h declares it." % name
    return call


# -----------------------------------------------------------------------------
# The functions that need nothing but the marshalling above
# -----------------------------------------------------------------------------
#
# In yoai.h's order. The ones with a callback or a sample buffer, and
# yoai_providers, are written out further down.

_FUNCTIONS = (
    # Diagnostics
    ("version", "s", ""),
    ("set_log_level", "v", "s"),
    # The day log
    ("timeline_open", "z", "s"),
    ("timeline_opened", "b", ""),
    ("timeline_close", "v", ""),
    ("timeline_switch_to", "z", "s"),
    ("timeline_end", "z", ""),
    ("timeline_end_at", "z", "q"),
    ("timeline_undo", "z", ""),
    ("timeline_note_add", "z", "s"),
    ("timeline_now", "j", ""),
    ("timeline_recent", "j", "i"),
    ("timeline_day", "j", "i"),
    ("timeline_today", "i", ""),
    ("timeline_day_to_iso", "s", "i"),
    ("timeline_iso_to_day", "i", "s"),
    ("timeline_last_error", "s", ""),
    # Provider registry
    ("provider_set", "z", "ssssj?"),
    ("provider_remove", "b", "s"),
    ("provider_clear", "v", ""),
    # On-device models
    ("local_available", "b", ""),
    ("local_set", "z", "ssj?"),
    ("local_load", "z", "s"),
    ("local_set_threads", "z", "si"),
    ("local_forget", "z", "s"),
    ("local_warm", "z", "ss?"),
    ("memory_bandwidth", "j", "i?i?i?"),
    # Chat
    ("chat_preview", "j", "j"),
    # Prompts
    ("prompts", "j", ""),
    ("prompt_get", "S", "sj?"),
    ("prompt_set", "z", "ss?"),
    ("prompt_turn_system", "s", "s?i?"),
    ("prompt_last_error", "s", ""),
    # Voice
    ("voice_available", "b", ""),
    ("voice_load", "z", "s"),
    ("voice_loaded", "b", ""),
    ("voice_unload", "v", ""),
    ("voice_set_itn", "z", "i"),
    ("voice_set_threads", "z", "i"),
    # Echo cancellation
    ("aec_set_high_latency", "v", "i"),
    ("aec_set_reference_rate", "z", "i"),
    ("aec_reset", "v", ""),
    ("aec_last_error", "s", ""),
    # Omni
    ("omni_available", "b", ""),
    ("omni_load", "z", "s"),
    ("omni_loaded", "b", ""),
    ("omni_unload", "v", ""),
    ("omni_set_dit_steps", "z", "i"),
    # Speech output
    ("tts_available", "b", ""),
    ("tts_load", "z", "ss"),
    ("tts_loaded", "b", ""),
    ("tts_unload", "v", ""),
    ("tts_model_type", "s", ""),
    ("tts_speakers", "j", ""),
    ("tts_set_speaker", "v", "s"),
    ("tts_set_voice", "v", "s"),
    ("tts_set_seed", "v", "q"),
    ("tts_set_top_k", "v", "i"),
    ("tts_set_reference", "z", "s?s?"),
    ("tts_reference_seconds", "f", ""),
    ("tts_load_reference_embedding", "z", "s"),
    ("tts_set_streaming", "v", "i"),
    ("tts_set_temperature", "v", "f"),
    ("tts_set_streaming_text", "v", "i"),
    ("tts_set_eos_hold", "v", "i"),
    ("tts_set_threads", "v", "i"),
    ("tts_last_error", "s", ""),
    # Reading a reply aloud while it is still being written
    ("reader_available", "b", ""),
    ("reader_load", "z", "ssi?"),
    ("reader_loaded", "b", ""),
    ("reader_unload", "v", ""),
    ("reader_model_type", "s", ""),
    ("reader_speakers", "j", ""),
    ("reader_set_voice", "v", "s"),
    ("reader_set_speaker", "v", "s"),
    ("reader_set_reference_voice", "z", "s"),
    ("reader_load_reference_embedding", "z", "s"),
    ("reader_talker_hidden_size", "i", ""),
    ("reader_save_reference_embedding", "z", "s"),
    ("reader_set_seed", "v", "q"),
    ("reader_set_split", "v", "iii"),
    ("reader_set_clause_cuts", "v", "i"),
    ("reader_set_later_split", "v", "ii"),
    ("reader_set_first_stop_chars", "v", "i"),
    ("reader_set_temperature", "v", "f"),
    ("reader_set_streaming_text", "v", "i"),
    ("reader_set_eos_hold", "v", "i"),
    ("reader_set_whole_reply", "v", "i"),
    ("reader_set_threads", "v", "i"),
    ("reader_feed", "n", "s"),
    ("reader_cancel", "v", ""),
    ("reader_last_error", "s", ""),
    # Model bundles
    ("bundle_list", "j", "s"),
)

for _name, _kind, _args in _FUNCTIONS:
    globals()[_name] = _declare(_name, _kind, _args)
del _name, _kind, _args

__version__ = version()  # noqa: F821 -- declared just above


# -----------------------------------------------------------------------------
# The rest: callbacks, sample buffers, and the provider list
# -----------------------------------------------------------------------------

_CHAT_HANDLER = CFUNCTYPE(c_int, c_char_p, c_char_p, c_void_p)
_AUDIO_HANDLER = CFUNCTYPE(c_int, POINTER(c_float), c_int, c_int, c_void_p)
_TEXT_HANDLER = CFUNCTYPE(None, c_char_p, c_void_p)
_READER_HANDLER = CFUNCTYPE(c_int, POINTER(c_float), c_int, c_int, c_int, c_void_p)
_BUNDLE_PROGRESS = CFUNCTYPE(c_int, c_char_p, c_double, c_void_p)


def _raw(name, restype, *argtypes):
    fn = getattr(lib, "yoai_" + name)
    fn.restype = restype
    fn.argtypes = list(argtypes)
    return fn


_providers = _raw("providers", c_char_p)
_chat = _raw("chat", c_int, c_char_p, c_char_p, c_char_p, c_char_p, _CHAT_HANDLER, c_void_p)
_voice_analyze = _raw("voice_analyze", c_char_p, POINTER(c_short), c_int, c_int)
_aec_process = _raw("aec_process", c_int, POINTER(c_short), c_int, POINTER(c_float), c_int,
                    POINTER(c_short), c_int)
_omni_ask = _raw("omni_ask", c_char_p, POINTER(c_short), c_int, c_char_p,
                 _AUDIO_HANDLER, _TEXT_HANDLER, c_void_p)
_tts_say = _raw("tts_say", c_char_p, c_char_p, _AUDIO_HANDLER, c_void_p)
_reader_begin = _raw("reader_begin", c_int, _READER_HANDLER, c_void_p)
_reader_finish = _raw("reader_finish", c_char_p)
_bundle_extract = _raw("bundle_extract", c_int, c_char_p, c_char_p, _BUNDLE_PROGRESS, c_void_p)


def _guard(errors, fn, failed=0):
    """`fn` made safe to call from C: an exception is kept in `errors`, to be
    raised once the C call returns, and `failed` is returned in its place --
    0, which tells the core to stop. Left to ctypes, the exception would be
    printed and the return value made up."""
    def call(*args):
        try:
            return fn(*args)
        except BaseException as e:  # KeyboardInterrupt too: it should stop the core
            errors.append(e)
            return failed
    return call


def _go_on(answer):
    """A Python callback's answer as the C one: anything but False keeps
    going, so a function that returns nothing does not stop the core."""
    return 0 if answer is False else 1


def _pcm16(pcm):
    """16-bit mono samples -- bytes in native byte order, an array('h'), or
    any sequence of ints -- as a C array and its length."""
    if not isinstance(pcm, (bytes, bytearray, memoryview)):
        pcm = array.array("h", pcm)
    data = memoryview(pcm).cast("B")
    n = len(data) // 2
    return (c_short * n).from_buffer_copy(data[:n * 2]), n


def _floats(pcm, n):
    """`n` float samples from C as an array('f'), copied: the buffer is only
    valid during the callback."""
    return array.array("f", ctypes.string_at(pcm, n * 4)) if n > 0 else array.array("f")


def _audio_handler(errors, on_audio):
    if on_audio is None:
        return _AUDIO_HANDLER()  # NULL
    return _AUDIO_HANDLER(_guard(errors, lambda pcm, n, last, _:
                                 _go_on(on_audio(_floats(pcm, n), bool(last)))))


def providers():
    """yoai_providers(), as a list of ids."""
    return [p for p in _str(_providers()).split("\n") if p]


def chat(provider_id, messages, system_prompt=None, grammar=None):
    """One turn of yoai_chat(), as a generator of (event_type, content) pairs:
    "reply" and "thinking" text as it streams, then "usage" with its content
    parsed.

    `messages` is the whole transcript, a list of {"role", "content"} dicts,
    or a str for a single user message.

    An "error" event, or a turn that fails without one, raises Error when the
    turn ends, after whatever arrived before it. Leaving the loop early
    cancels the turn, as returning 0 from the C callback does.

    yoai_chat blocks, so it runs on a thread of its own while this yields."""
    if isinstance(messages, str):
        messages = [{"role": "user", "content": messages}]
    # Converted here, so a bad argument raises in the caller's thread.
    args = (_text(provider_id), _json(messages), _text(system_prompt), _text(grammar))
    events = queue.Queue()
    stop = threading.Event()
    code = []

    def handler(kind, content, _):
        if stop.is_set():
            return 0
        events.put((_str(kind), _str(content)))
        return 1

    callback = _CHAT_HANDLER(handler)

    def run():
        try:
            code.append(_chat(*(args + (callback, None))))
        finally:
            events.put(None)

    worker = threading.Thread(target=run, name="yoai-chat", daemon=True)
    worker.start()
    errors = []
    try:
        while True:
            event = events.get()
            if event is None:
                break
            kind, content = event
            if kind == "error":
                errors.append(content)
            elif kind == "usage":
                yield kind, json.loads(content)
            else:
                yield kind, content
    finally:
        stop.set()
        worker.join()
    if errors:
        raise Error("yoai_chat: " + "; ".join(errors))
    if code and code[0] != 0:
        raise Error("yoai_chat returned %d" % code[0])


def voice_analyze(pcm, sample_rate=16000):
    """yoai_voice_analyze(). `pcm` is 16-bit mono at 16000 Hz: bytes, an
    array('h'), or a sequence of ints."""
    samples, n = _pcm16(pcm)
    return _result("voice_analyze", "j", _voice_analyze(samples, n, int(sample_rate)))


def aec_process(mic, ref):
    """yoai_aec_process(). `mic` is 16-bit mono at 16000 Hz, as voice_analyze
    takes; `ref` is float mono at the reference rate -- an array('f'), as
    tts_say hands it over, or any sequence of numbers. Returns array('h') with
    everything ready, the output that did not fit included."""
    mic_buf, n_mic = _pcm16(mic)
    ref = ref if isinstance(ref, array.array) and ref.typecode == "f" else array.array("f", ref)
    n_ref = len(ref)
    ref_buf = (c_float * n_ref).from_buffer_copy(ref) if n_ref else None
    out = array.array("h")
    room = n_mic + 512
    buf = (c_short * room)()
    while True:
        n = _aec_process(mic_buf, n_mic, ref_buf, n_ref, buf, room)
        if n < 0:
            _fail("aec_process", "returned %d" % n)
        out.frombytes(ctypes.string_at(buf, n * 2))
        if n < room:
            return out
        # Full: the rest is drained by calls with no input.
        mic_buf, n_mic, ref_buf, n_ref = None, 0, None, 0


def omni_ask(pcm, prompt, on_audio=None, on_text=None):
    """yoai_omni_ask(). `on_audio(pcm, last)` is given the spoken reply as
    array('f') chunks at 24000 Hz, and returning False stops it; `on_text(text)`
    is given the reply text as soon as it exists. Returns the result object."""
    samples, n = _pcm16(pcm)
    errors = []
    audio = _audio_handler(errors, on_audio)

    def text_handler(reply, _):
        on_text(_str(reply))  # a void callback: nothing goes back to C

    text = (_TEXT_HANDLER(_guard(errors, text_handler, failed=None))
            if on_text is not None else _TEXT_HANDLER())
    raw = _omni_ask(samples, n, _text(prompt), audio, text, None)
    if errors:
        raise errors[0]
    return _result("omni_ask", "j", raw)


def tts_say(text, on_audio=None):
    """yoai_tts_say(). `on_audio(pcm, last)` is given each chunk as an
    array('f') of mono samples at 24000 Hz, and returning False stops; the
    last call has no samples and `last` True. Returns the result object."""
    errors = []
    raw = _tts_say(_text(text), _audio_handler(errors, on_audio), None)
    if errors:
        raise errors[0]
    return _result("tts_say", "j", raw)


# The reader's callback runs on the reader's own threads from reader_begin to
# reader_finish, so it is kept here for that long -- a ctypes callback that is
# collected while C still holds it is a crash -- and so are its exceptions.
_reader_handler = None
_reader_errors = []


def reader_begin(on_audio=None):
    """yoai_reader_begin(). `on_audio(pcm, sentence, last)` is given the audio
    in reading order, as array('f') chunks at 24000 Hz, ON THE READER'S OWN
    THREADS; returning False stops. An exception in it is raised by
    reader_finish()."""
    global _reader_handler
    del _reader_errors[:]
    if on_audio is None:
        handler = _READER_HANDLER()
    else:
        handler = _READER_HANDLER(_guard(_reader_errors, lambda pcm, n, sentence, last, _:
                                         _go_on(on_audio(_floats(pcm, n), sentence,
                                                         bool(last)))))
    _reader_handler = handler
    if _reader_begin(handler, None) == 0:
        _reader_handler = None
        _fail("reader_begin", "returned 0")


def reader_finish():
    """yoai_reader_finish(): blocks until the last sample is out, then returns
    the result object."""
    global _reader_handler
    try:
        raw = _reader_finish()
    finally:
        _reader_handler = None
    if _reader_errors:
        error = _reader_errors[0]
        del _reader_errors[:]
        raise error
    return _result("reader_finish", "j", raw)


def bundle_extract(bundle_path, dest_dir, on_progress=None):
    """yoai_bundle_extract(). `on_progress(name, fraction)` is called as it
    runs, and returning False aborts it."""
    errors = []
    progress = (_BUNDLE_PROGRESS(_guard(errors, lambda name, fraction, _:
                                        _go_on(on_progress(_str(name), fraction))))
                if on_progress is not None else _BUNDLE_PROGRESS())
    code = _bundle_extract(_text(bundle_path), _text(dest_dir), progress, None)
    if errors:
        raise errors[0]
    _result("bundle_extract", "z", code)
