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

// The first channel at the file's own rate, which lands in *rate.
inline bool read_wav_mono(const char* path, std::vector<float>* pcm, int* rate) {
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

    int      format = 0, channels = 0, bits = 0, file_rate = 0;
    size_t   data_at = 0, data_len = 0;
    size_t   at = 12;
    while (at + 8 <= buf.size()) {
        const char* id = reinterpret_cast<const char*>(&buf[at]);
        const uint32_t len = le32(&buf[at + 4]);
        const size_t body = at + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16 && body + 16 <= buf.size()) {
            format    = le16(&buf[body]);
            channels  = le16(&buf[body + 2]);
            file_rate = static_cast<int>(le32(&buf[body + 4]));
            bits      = le16(&buf[body + 14]);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_at  = body;
            data_len = len;
            if (data_at + data_len > buf.size()) {
                data_len = buf.size() - data_at;   // truncated file, take what is there
            }
        }
        at = body + len + (len & 1);   // chunks are word-aligned
    }

    if (!data_len || channels <= 0 || file_rate <= 0) {
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

    *rate = file_rate;
    pcm->swap(mono);
    return true;
}

inline bool read_wav_16k_mono(const char* path, std::vector<float>* pcm, int* source_rate) {
    std::vector<float> mono;
    int rate = 0;
    if (!read_wav_mono(path, &mono, &rate)) {
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

inline void put_le16(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}

inline void put_le32(unsigned char* p, uint32_t v) {
    put_le16(p, v & 0xFFFF);
    put_le16(p + 2, v >> 16);
}

// Mono 16-bit PCM, rounded to the nearest step and clipped at full scale.
inline bool write_wav_mono16(const char* path, const std::vector<float>& pcm, int rate) {
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    std::vector<unsigned char> out(44 + static_cast<size_t>(bytes));
    unsigned char* h = &out[0];
    std::memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);                                // PCM
    put_le16(h + 22, 1);                                // mono
    put_le32(h + 24, static_cast<uint32_t>(rate));
    put_le32(h + 28, static_cast<uint32_t>(rate) * 2);  // bytes a second
    put_le16(h + 32, 2);                                // bytes a frame
    put_le16(h + 34, 16);                               // bits
    std::memcpy(h + 36, "data", 4);
    put_le32(h + 40, bytes);
    for (size_t i = 0; i < pcm.size(); ++i) {
        double s = pcm[i] * 32767.0;
        s = s < 0 ? s - 0.5 : s + 0.5;
        long v = static_cast<long>(s);                  // truncates, so this rounds
        v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        put_le16(h + 44 + i * 2, static_cast<uint16_t>(static_cast<int16_t>(v)));
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::printf("      cannot write %s\n", path);
        return false;
    }
    const bool ok = std::fwrite(h, 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

}  // namespace testing
}  // namespace yoai
