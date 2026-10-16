// The C ABI over WebRTC's AEC3, see yoai_aec3.h for what it promises and why.
//
// Built by the WebRTC tree (build_aec3.py copies this directory into src/ and
// adds a shared_library target for it), so it is compiled with WebRTC's own
// toolchain and flags -- no exceptions, no RTTI, C++20, Chromium's libc++.

#include "yoai_aec3.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "api/audio/audio_processing.h"
#include "api/audio/echo_canceller3_factory.h"
#include "api/audio/echo_control.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "modules/audio_processing/audio_buffer.h"

namespace {

bool SupportedRate(int hz) {
  return hz == 16000 || hz == 32000 || hz == 48000;
}

// Largest frame: 10 ms at 48 kHz.
constexpr size_t kMaxFrame = 480;

// Output nobody collected is dropped past this, oldest first, so a caller that
// never drains cannot grow the queue without bound.
constexpr size_t kMaxQueuedSeconds = 1;

}  // namespace

struct yoai_aec3 {
  explicit yoai_aec3(int sample_rate_hz)
      : rate(static_cast<size_t>(sample_rate_hz)),
        frame(rate / 100),
        env(webrtc::EnvironmentFactory().Create()),
        stream(sample_rate_hz, 1),
        render(rate, 1, rate, 1, rate, 1),
        capture(rate, 1, rate, 1, rate, 1),
        aec(webrtc::EchoCanceller3Factory().Create(env, sample_rate_hz, 1, 1)) {}

  // One 10 ms frame of each, appended to `queued`.
  void ProcessFrame(const float* mic, const float* ref) {
    // The reference is analysed before the capture it belongs to, as the audio
    // processing module does: AEC3 reads it from a queue the capture side drains.
    const float* ref_channels[1] = {ref};
    render.CopyFrom(ref_channels, stream);
    if (render.num_bands() > 1) {
      render.SplitIntoFrequencyBands();
    }
    aec->AnalyzeRender(&render);

    const float* mic_channels[1] = {mic};
    capture.CopyFrom(mic_channels, stream);
    if (capture.num_bands() > 1) {
      capture.SplitIntoFrequencyBands();
    }
    aec->AnalyzeCapture(&capture);
    aec->ProcessCapture(&capture, /*level_change=*/false);
    if (capture.num_bands() > 1) {
      capture.MergeFrequencyBands();
    }

    float frame_out[kMaxFrame];
    float* out_channels[1] = {frame_out};
    capture.CopyTo(stream, out_channels);
    queued.insert(queued.end(), frame_out, frame_out + frame);
  }

  size_t Process(const float* mic,
                 const float* ref,
                 size_t n,
                 float* out,
                 size_t out_capacity) {
    if (mic && n > 0) {
      mic_in.insert(mic_in.end(), mic, mic + n);
      if (ref) {
        ref_in.insert(ref_in.end(), ref, ref + n);
      } else {
        ref_in.insert(ref_in.end(), n, 0.0f);
      }

      size_t used = 0;
      while (mic_in.size() - used >= frame) {
        ProcessFrame(mic_in.data() + used, ref_in.data() + used);
        used += frame;
      }
      mic_in.erase(mic_in.begin(), mic_in.begin() + used);
      ref_in.erase(ref_in.begin(), ref_in.begin() + used);
    }

    const size_t max_queued = kMaxQueuedSeconds * rate;
    if (queued.size() > max_queued) {
      queued.erase(queued.begin(), queued.begin() + (queued.size() - max_queued));
    }

    if (!out) {
      return 0;
    }
    const size_t k = std::min(out_capacity, queued.size());
    if (k > 0) {
      std::memcpy(out, queued.data(), k * sizeof(float));
      queued.erase(queued.begin(), queued.begin() + k);
    }
    return k;
  }

  const size_t rate;
  const size_t frame;
  webrtc::Environment env;
  webrtc::StreamConfig stream;
  webrtc::AudioBuffer render;
  webrtc::AudioBuffer capture;
  std::unique_ptr<webrtc::EchoControl> aec;

  // Less than a frame of each, waiting for the rest of it; and finished output
  // that has not fit in a caller's buffer yet.
  std::vector<float> mic_in;
  std::vector<float> ref_in;
  std::vector<float> queued;
};

extern "C" {

yoai_aec3* yoai_aec3_create(int sample_rate_hz) {
  if (!SupportedRate(sample_rate_hz)) {
    return nullptr;
  }
  return new (std::nothrow) yoai_aec3(sample_rate_hz);
}

void yoai_aec3_destroy(yoai_aec3* aec) {
  delete aec;
}

size_t yoai_aec3_frame_samples(const yoai_aec3* aec) {
  return aec ? aec->frame : 0;
}

size_t yoai_aec3_process(yoai_aec3* aec,
                         const float* mic,
                         const float* ref,
                         size_t n,
                         float* out,
                         size_t out_capacity) {
  if (!aec) {
    return 0;
  }
  return aec->Process(mic, ref, n, out, out_capacity);
}

void yoai_aec3_set_delay_ms(yoai_aec3* aec, int delay_ms) {
  if (aec) {
    aec->aec->SetAudioBufferDelay(delay_ms);
  }
}

int yoai_aec3_get_metrics(yoai_aec3* aec, yoai_aec3_metrics* metrics) {
  if (!aec || !metrics) {
    return 0;
  }
  const webrtc::EchoControl::Metrics m = aec->aec->GetMetrics();
  metrics->erl_db = m.echo_return_loss;
  metrics->erle_db = m.echo_return_loss_enhancement;
  metrics->delay_ms = m.delay_ms;
  return 1;
}

}  // extern "C"
