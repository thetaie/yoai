# 01_female

A reference voice for the Qwen3-TTS Base checkpoint, clone mode A: every
utterance is conditioned on one speaker embedding, with no reference audio in
context. See `Qtts::set_reference_embedding` in `src/voice/tts/qwen3_tts.hpp`.

| File | What it is |
| --- | --- |
| `demo.wav` | The recording: 22.4 s of Mandarin, mono, 16-bit, 24000 Hz. |
| `qwen3-tts-1.7b.spk` | Its speaker embedding for the 1.7B Base talker: 2048 f32 values. |
| `qwen3-tts-0.6b.spk` | The same for the 0.6B Base talker: 1024 f32 values. |

A `.spk` file holds raw f32 values in host byte order and nothing else. It is
the format that qwentts.cpp's `qwen-codec` writes and `qwen-tts --ref-spk`
reads.

Each `.spk` works only with the model size it was extracted with: the length
must equal that talker's hidden size. The quantization does not matter. The
Q4_K_M and Q8_0 talkers of each size produced bit-identical vectors from this
recording, so the speaker encoder is not quantized.

To regenerate, run the reader test against the recording. It extracts the
embedding and writes `reference-base.spk` to the working directory:

    yoai-reader-test <qwen-talker-1.7b-base.gguf> <qwen-tokenizer-12hz.gguf> 1 x 23 -1 speaker/01_female/demo.wav
