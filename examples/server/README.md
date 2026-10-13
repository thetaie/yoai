# The speech server

The core behind a port. Today that is one service: **text in, speech out, over
a WebSocket**. The text can arrive a token at a time while a language model is
still writing it, and the speech starts before the text ends.

It is one executable, `yoai-server`, that links `yoai_core` from the root
CMake project, the way `examples/desktop/native/` does. It is the one host
that binds a port, and the one that needs libwebsockets.

```
src/main.cpp          the command line: loads the engine and the voice, then serves
src/tts_server.hpp    the server: who owns what, and on which thread
src/tts_server.cpp
tts_client.py         a client for trying it from a shell
```

## Building

Needs what the root build needs; see the root [README](../../README.md).

```
cd examples/server
python build.py
```

`python3` on macOS and Linux. The result is `build/yoai-server.exe`
(`build/yoai-server` off Windows), with the DLLs it loads beside it on
Windows.

Speech output is always compiled in. Its SDK is the root build's,
`build/qtts-sdk` at the repository root, built the same way `python build.py
tts` builds it there: it uses the GPU through CUDA or Vulkan when there is one,
and otherwise the CPU, with a warning. libwebsockets comes from vcpkg through
`vcpkg.json`'s `server` feature, installed into `build/vcpkg_installed` here.
`python build.py clean` starts `build/` over; `--help` lists the settings.

## Running

```
build\yoai-server.exe Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf qwen-tokenizer-12hz-Q8_0.gguf ^
    --voice ..\..\speaker\01_female\qwen3-tts-1.7b.spk
```

`--help` lists the options. The reading settings are the desktop's (see
`examples/desktop/src/main/reader.ts`). Clients cannot change them.

`tts_client.py` is a client for trying it: Python 3.8+, standard library only.
It sends a passage at a fixed rate, as a language model would, and writes what
comes back to a WAV:

```
python tts_client.py --rate 15 --out out.wav "你好，今天天气不错。"
```

## Protocol

One WebSocket connection, at any path on the port. The subprotocol is
`yoai-tts`; a client may leave it out.

The client sends **text messages, each a JSON object**. The server sends JSON
text messages for events and **binary messages for audio**.

A connection can read any number of passages, one after another. **Only one
passage is read at a time across the whole server**: there is one engine. A
`start` while another client is being read to gets a `busy` error. That client
stays connected and can try again later.

### Client to server

| `type` | fields | |
|---|---|---|
| `start` | `format`: `"s16"` (default) or `"f32"` | Open a passage. Answered by `started`. |
| `text` | `text`: string | Append text. Any size, from one token up; each message is at most 64 KB. Markdown and emoji are fine: the core turns them into speech. |
| `finish` | | No more text. Audio keeps coming until the passage has been read, then `done`. |
| `cancel` | | Stop reading, with or without `finish` first. Answered by `cancelled`. Closing the connection does the same. |

### Server to client

| `type` | fields | |
|---|---|---|
| `started` | `sample_rate` (24000), `channels` (1), `format` | The passage is open. Send `text`. |
| *(binary)* | | Audio, in reading order: raw little-endian PCM, 16-bit signed or 32-bit float as asked, mono at 24 kHz. No header; message boundaries have no meaning. |
| `done` | `sentences`, `speech_seconds`, `elapsed_seconds`, `first_audio_seconds`, `starved_seconds` | The last audio has been sent. `start` can be sent again. |
| `cancelled` | | After `cancel`. `start` can be sent again. |
| `error` | `code`, `message` | See below. |

Error codes:

- `synthesis`: the engine failed. It ends the passage and the connection returns to idle.
- `busy`: another client is being read to.
- `already_started`: `start` while a passage is open.
- `not_started`: `text` or `finish` before `start`.
- `bad_format`, `bad_message`, `too_large`: a malformed message. The message is dropped and the connection stays open.

Every other error leaves the connection as it was.

A minimal exchange:

```
→ {"type":"start"}
← {"type":"started","sample_rate":24000,"channels":1,"format":"s16"}
→ {"type":"text","text":"我查了一下，"}
→ {"type":"text","text":"明天上海多云转小雨。"}
← (binary) ...
→ {"type":"finish"}
← (binary) ...
← {"type":"done","sentences":1,"speech_seconds":3.1,...}
```

### Timing

- **The writer must stay ahead of the reader.** By default a passage is read
  as one generation that follows the text, at 12.5 tokens a second. A writer
  slower than that makes the audio slower than real time. `--pieces` reads in
  pieces instead, each its own generation, which suits a slow writer.
- **Cancelling is not instant.** The engine stops when the next block of audio
  arrives, which took 0.7 s after a client disconnected mid-passage. A `start`
  from another client inside that window gets `busy`.
- **Clocks.** `first_audio_seconds` and `elapsed_seconds` are measured from
  `start`, so they include the time the client took to send text.
- **Buffering.** Audio is queued per client with no limit. The engine runs at
  about twice real time, so a client that cannot receive 48 KB/s (s16) holds
  the difference in the server's memory until the passage ends.

## Threads

See `src/tts_server.hpp`. In short:

- The libwebsockets event loop does every socket call, plus `Reader::begin`
  and `Reader::feed`.
- `Reader::finish` blocks, so it runs on a thread of its own per passage.
- Audio is queued from the Reader's emitter thread, and the emitter wakes the
  loop to send it.
