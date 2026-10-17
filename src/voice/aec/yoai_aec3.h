/* A C ABI over WebRTC's AEC3 echo canceller, for yoai.
 *
 * Why a C ABI and a shared library rather than a static archive: AEC3 is built
 * by WebRTC's own toolchain -- clang-cl, Chromium's libc++ (std::__Cr), static
 * CRT, C++20 -- and none of that matches yoai_core (MSVC STL, /MD, C++11).
 * Nothing C++ may cross this boundary, so nothing does: only plain C types, no
 * ownership transfer other than the handle, and no memory allocated on one side
 * and freed on the other. libc++ and the CRT stay inside the library.
 *
 * Audio is mono float in [-1, 1], at 16000, 32000 or 48000 Hz. AEC3 itself works
 * on 10 ms frames; this layer buffers, so callers pass any number of samples.
 *
 * Not thread-safe: one handle, one caller, microphone and reference in lockstep.
 */
#ifndef YOAI_AEC3_H_
#define YOAI_AEC3_H_

#include <stddef.h>

#if defined(_WIN32)
#  if defined(YOAI_AEC3_BUILDING)
#    define YOAI_AEC3_API __declspec(dllexport)
#  else
#    define YOAI_AEC3_API __declspec(dllimport)
#  endif
#else
#  define YOAI_AEC3_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yoai_aec3 yoai_aec3;

typedef struct yoai_aec3_metrics {
    double erl_db;    /* echo return loss, dB */
    double erle_db;   /* echo return loss enhancement, dB */
    int    delay_ms;  /* the delay AEC3 currently estimates, ms */
} yoai_aec3_metrics;

/* NULL for an unsupported sample rate (anything but 16000, 32000, 48000). */
YOAI_AEC3_API yoai_aec3* yoai_aec3_create(int sample_rate_hz);

/* NULL is allowed. */
YOAI_AEC3_API void yoai_aec3_destroy(yoai_aec3* aec);

/* Samples in one AEC3 frame (10 ms) at the handle's rate; 0 for NULL. */
YOAI_AEC3_API size_t yoai_aec3_frame_samples(const yoai_aec3* aec);

/* `mic` and `ref` are `n` samples each and in lockstep: ref[i] is what was sent
 * to the speaker when mic[i] was captured. A NULL `ref` is read as silence.
 *
 * Writes up to `out_capacity` echo-removed samples to `out` and returns how many.
 * Output comes in whole 10 ms frames, so it trails the input by less than one
 * frame; what does not fit in `out_capacity` is kept and returned by the next
 * call. A capacity of `n + yoai_aec3_frame_samples(aec)` always drains it. */
YOAI_AEC3_API size_t yoai_aec3_process(yoai_aec3* aec,
                                       const float* mic,
                                       const float* ref,
                                       size_t n,
                                       float* out,
                                       size_t out_capacity);

/* An external estimate of the playback-to-capture buffer delay, when the host
 * knows one. Optional: AEC3 estimates the delay itself. */
YOAI_AEC3_API void yoai_aec3_set_delay_ms(yoai_aec3* aec, int delay_ms);

/* 1 and `*metrics` filled, or 0 for a NULL argument. */
YOAI_AEC3_API int yoai_aec3_get_metrics(yoai_aec3* aec, yoai_aec3_metrics* metrics);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* YOAI_AEC3_H_ */
