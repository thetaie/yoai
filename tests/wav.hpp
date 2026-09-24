// WAV, only as much as the desktop harnesses need.
//
// Deliberately NOT in the core. On a device the microphone hands over PCM
// already, so a file decoder would be weight that never travels -- and the one
// the reference ASR runtime uses cannot open a non-ASCII path on Windows, which
// costs a desktop test nothing and would cost a user everything.
//
// Shared by the harnesses rather than copied into each, because a bug in the
// reader would otherwise look like a bug in whichever one of them noticed.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace yoai {
namespace testing {

inline uint32_t le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t le16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

inline bool read_wav_16k_mono(const char* path, std::vector<float>* pcm, int* source_rate) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("      cannot open %s\n", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(static_cast<size_t>(size > 0 ? size : 0));
    if (buf.size() < 44 || std::fread(&buf[0], 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        std::printf("      %s is too short to be a WAV\n", path);
        return false;
    }
    std::fclose(f);

    if (std::memcmp(&buf[0], "RIFF", 4) != 0 || std::memcmp(&buf[8], "WAVE", 4) != 0) {
        std::printf("      %s is not a RIFF/WAVE file\n", path);
        return false;
    }

    int      format = 0, channels = 0, bits = 0, rate = 0;
    size_t   data_at = 0, data_len = 0;
    size_t   at = 12;
    while (at + 8 <= buf.size()) {
        const char* id = reinterpret_cast<const char*>(&buf[at]);
        const uint32_t len = le32(&buf[at + 4]);
        const size_t body = at + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16 && body + 16 <= buf.size()) {
            format   = le16(&buf[body]);
            channels = le16(&buf[body + 2]);
            rate     = static_cast<int>(le32(&buf[body + 4]));
            bits     = le16(&buf[body + 14]);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_at  = body;
            data_len = len;
            if (data_at + data_len > buf.size()) {
                data_len = buf.size() - data_at;   // truncated file, take what is there
            }
        }
        at = body + len + (len & 1);   // chunks are word-aligned
    }

    if (!data_len || channels <= 0 || rate <= 0) {
        std::printf("      %s has no usable data chunk\n", path);
        return false;
    }

    // Channel 0 rather than a downmix: these are voice recordings, and summing
    // channels that are not in phase quietly changes the spectrum.
    std::vector<float> mono;
    if (format == 1 && bits == 16) {
        const size_t n = data_len / 2;
        mono.reserve(n / channels);
        for (size_t i = 0; i < n; i += channels) {
            const short s = static_cast<short>(le16(&buf[data_at + i * 2]));
            mono.push_back(s / 32768.0f);
        }
    } else if (format == 3 && bits == 32) {
        const size_t n = data_len / 4;
        mono.reserve(n / channels);
        for (size_t i = 0; i < n; i += channels) {
            float s;
            std::memcpy(&s, &buf[data_at + i * 4], 4);
            mono.push_back(s);
        }
    } else {
        std::printf("      %s is format %d at %d bits, which this harness does not read\n",
                    path, format, bits);
        return false;
    }

    *source_rate = rate;
    if (rate == 16000) {
        pcm->swap(mono);
        return true;
    }

    // Linear interpolation, with no anti-aliasing filter. Good enough to look at
    // a clip that is already on hand; a device never takes this path, because
    // the microphone is opened at 16 kHz.
    const double ratio = rate / 16000.0;
    const size_t out_n = static_cast<size_t>(mono.size() / ratio);
    pcm->resize(out_n);
    for (size_t i = 0; i < out_n; ++i) {
        const double src = i * ratio;
        const size_t j = static_cast<size_t>(src);
        const double frac = src - j;
        const float a = mono[j];
        const float b = j + 1 < mono.size() ? mono[j + 1] : a;
        (*pcm)[i] = static_cast<float>(a + (b - a) * frac);
    }
    return true;
}


}  // namespace testing
}  // namespace yoai
