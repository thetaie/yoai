# The Python package

The core as a shared library, wrapped with ctypes, as a wheel this machine can
`pip install`. Every function in [src/yoai.h](../../src/yoai.h) is here without
its `yoai_` prefix, and takes the same arguments in the same order:
`yoai_timeline_open(dir)` is `yoai.timeline_open(dir)`. What each one does is
in the header and is not repeated here.

## Building

Needs what the root build needs; see the root [README](../../README.md).

```
cd examples/python
python build.py
pip install --force-reinstall dist/yoai-0.1.0-py3-none-win_amd64.whl
```

`python3` on macOS and Linux, where the wheel's name ends in that platform
instead; `build.py` prints the `pip install` line for this one.

`build.py` runs the root `build.py`, which builds the core as `build/yoai.dll`
(`libyoai.so`, `libyoai.dylib`) beside the CLI tools, with the lanes that tree
has. Then it writes the wheel into `dist/`. Lane arguments go to the root build,
so on-device chat is `python build.py clean asr llama tts`.

The wheel is for this machine only:

- It is tagged with this platform. It is not tied to a Python version, since
  ctypes compiles nothing, so it installs into any Python 3.8 or newer of this
  architecture.
- It carries the library and, on Windows, the DLLs it loads from `build/`:
  curl, OpenSSL and zlib. The rest comes from this machine: the Visual C++
  runtime, the Vulkan loader, and the CUDA runtime for a build with CUDA.
- Built and run on Windows so far. The Linux and macOS paths are written but
  not yet tried.

## Using it

```python
import yoai

yoai.provider_set("zhipu/glm-4.7-flash", "https://open.bigmodel.cn/api/paas/v4",
                  api_key, "glm-4.7-flash")

for kind, content in yoai.chat("zhipu/glm-4.7-flash", "Say hello in one word."):
    if kind == "reply":
        print(content, end="", flush=True)
```

What the wrapper adds to the C ABI:

- Strings in and out as `str`. A JSON argument takes a dict or a list as well
  as a string, and a JSON result comes back parsed.
- A failure raises `yoai.Error`, with the core's own reason where it keeps one.
- `chat()` is a generator of `(event_type, content)` pairs, `"usage"` parsed. An
  `"error"` event raises `yoai.Error` when the turn ends. Leaving the loop early
  cancels the turn.
- A callback is a Python function: `tts_say(text, on_audio)`,
  `reader_begin(on_audio)`, `omni_ask(pcm, prompt, on_audio, on_text)`,
  `bundle_extract(path, dest, on_progress)`. Audio arrives as `array('f')` at
  24000 Hz, and returning `False` stops. The reader calls back on its own
  threads.
- `voice_analyze(pcm)` takes 16-bit samples at 16000 Hz: `bytes`, `array('h')`
  or a list of ints.
- `yoai.lib` is the ctypes library itself, every signature declared.

Do not `import yoai` from inside `examples/python/`: the `yoai/` directory
there has no library in it, and Python imports it instead of the installed
package.

## Checking it

```
python tests/test_yoai.py
```

With no options it needs no model and no network: every function bound, the
day log, the prompts, the provider registry, a turn streamed from a local
stand-in for a provider, two turns that fail, and prosody on a synthetic tone.
Each option adds a lane, run on the model given:

```
python tests/test_yoai.py --talker qwen-talker-0.6b-customvoice-Q4_K_M.gguf ^
    --codec qwen-tokenizer-12hz-Q8_0.gguf --asr sensevoice-small-q8.gguf
```

speaks a sentence, transcribes it back, and has the reader speak text fed to
it in pieces. `--gguf` runs one on-device chat turn, on a build with `llama`.
