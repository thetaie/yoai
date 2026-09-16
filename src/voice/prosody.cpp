#include "voice/prosody.hpp"

#include <algorithm>
#include <cmath>

namespace bodyclock {
namespace voice {
namespace {

// One frame's pitch, or 0 when the frame is not voiced.
//
// Autocorrelation, with the peak height doubling as the voicing test -- which is
// what keeps unvoiced frames out of the statistics instead of contributing noise
// to them.
//
// The normalization is by the energy of the WHOLE frame rather than by the
// energy of the overlapping part. That under-normalizes long lags and so leans
// slightly toward reporting a pitch an octave high. It is kept because it is
// what produced the result this file reproduces; changing it would mean the
// numbers can no longer be checked against anything.
float frame_f0(const float* samples, int length, int sample_rate,
               const ProsodyOptions& opt) {
    double mean = 0.0;
    for (int i = 0; i < length; ++i) {
        mean += samples[i];
    }
    mean /= length;

    // Doubles throughout, not floats. The reference implementation this
    // reproduces is Python, where every one of these is a double, and the
    // comparison that picks the winning lag is often a near-tie between adjacent
    // lags -- rounding the accumulator to float is enough to pick the other one
    // and report a pitch an octave out for that frame.
    std::vector<double> f(length);
    double energy = 0.0;
    for (int i = 0; i < length; ++i) {
        f[i] = samples[i] - mean;
        energy += f[i] * f[i];
    }
    if (energy <= 1e-12) {
        return 0.0f;
    }

    const int lo = static_cast<int>(sample_rate / opt.f0_max);
    const int hi = std::min(static_cast<int>(sample_rate / opt.f0_min), length - 1);

    int    best_lag = 0;
    double best     = 0.0;
    for (int lag = lo; lag < hi; ++lag) {
        double sum = 0.0;
        for (int i = 0; i < length - lag; ++i) {
            sum += f[i] * f[i + lag];
        }
        const double r = sum / energy;
        if (r > best) {
            best     = r;
            best_lag = lag;
        }
    }
    return (best_lag && best > opt.voicing_threshold)
               ? static_cast<float>(sample_rate) / best_lag
               : 0.0f;
}

// The percentile convention the scratchpad tools used: nearest index from below,
// clamped. Not interpolated -- with a few dozen voiced frames per utterance the
// difference is noise, and matching the tool that produced the reference numbers
// is worth more than a better estimator.
float percentile(std::vector<float> values, float p) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(values.size() * p);
    if (index >= values.size()) {
        index = values.size() - 1;
    }
    return values[index];
}

// Population standard deviation, and never zero: a recording where every
// utterance measured the same would otherwise divide by nothing. Returning 1
// there makes every deviation come out 0, which is the honest answer.
void mean_and_sd(const std::vector<float>& values, float* mean, float* sd) {
    if (values.empty()) {
        *mean = 0.0f;
        *sd   = 1.0f;
        return;
    }
    double sum = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        sum += values[i];
    }
    const double m = sum / values.size();
    double var = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        const double d = values[i] - m;
        var += d * d;
    }
    const double s = std::sqrt(var / values.size());
    *mean = static_cast<float>(m);
    *sd   = s > 0.0 ? static_cast<float>(s) : 1.0f;
}

}  // namespace

bool analyze(const float* pcm, size_t n_samples, int sample_rate,
             const ProsodyOptions& opt, Recording* out) {
    if (!out) {
        return false;
    }
    *out = Recording();
    if (!pcm || sample_rate <= 0) {
        return false;
    }

    const int frame_length = static_cast<int>(sample_rate * opt.frame_length);
    const int frame_shift  = static_cast<int>(sample_rate * opt.frame_shift);
    if (frame_length <= 0 || frame_shift <= 0 ||
        n_samples <= static_cast<size_t>(frame_length)) {
        return false;
    }

    out->duration = static_cast<float>(n_samples) / sample_rate;
    const float hop_seconds = static_cast<float>(frame_shift) / sample_rate;

    // Frame energies first. The last partial frame is dropped rather than
    // zero-padded -- padding would invent a quiet frame and can close a pause
    // that the speaker did not take.
    std::vector<float>  rms;
    std::vector<size_t> offset;
    for (size_t i = 0; i + frame_length < n_samples; i += frame_shift) {
        double sum = 0.0;
        for (int j = 0; j < frame_length; ++j) {
            const double v = pcm[i + j];
            sum += v * v;
        }
        rms.push_back(static_cast<float>(std::sqrt(sum / frame_length)));
        offset.push_back(i);
    }
    if (rms.empty()) {
        return false;
    }

    out->peak_rms = *std::max_element(rms.begin(), rms.end());
    const float floor_rms =
        (out->peak_rms > 0.0f ? out->peak_rms : 1.0f) * opt.silence_fraction;

    // Runs of speech separated by pauses. A pause has to be whole: a gap shorter
    // than min_pause does not end an utterance, it just sits inside one, which is
    // why speech_ratio can be below 1.
    const int gap_needed = static_cast<int>(opt.min_pause / hop_seconds);
    const int min_frames = static_cast<int>(opt.min_utterance / hop_seconds);

    std::vector<std::pair<size_t, size_t> > runs;
    size_t begin = 0;
    bool   open  = false;
    size_t last  = 0;
    int    gap   = 0;
    for (size_t i = 0; i < rms.size(); ++i) {
        if (rms[i] > floor_rms) {
            if (gap >= gap_needed && open) {
                runs.push_back(std::make_pair(begin, last));
                open = false;
            }
            if (!open) {
                begin = i;
                open  = true;
            }
            last = i;
            gap  = 0;
        } else {
            ++gap;
        }
    }
    if (open) {
        runs.push_back(std::make_pair(begin, last));
    }

    std::vector<float> f0_means, rms_means, speech_ratios;
    for (size_t r = 0; r < runs.size(); ++r) {
        const size_t a = runs[r].first;
        const size_t b = runs[r].second;
        if (static_cast<int>(b - a) < min_frames) {
            continue;
        }

        // Pitch is computed only inside utterances and only on frames above the
        // floor. Running it over silence would drag every mean toward whatever
        // the autocorrelation finds in noise.
        std::vector<float> voiced;
        double rms_sum = 0.0;
        int    spoken  = 0;
        for (size_t i = a; i <= b; ++i) {
            rms_sum += rms[i];
            if (rms[i] > floor_rms) {
                ++spoken;
                const float f0 = frame_f0(pcm + offset[i], frame_length, sample_rate, opt);
                if (f0 > 0.0f) {
                    voiced.push_back(f0);
                }
            }
        }

        const size_t count = b - a + 1;
        Utterance u;
        u.first_frame   = a;
        u.last_frame    = b;
        u.start         = static_cast<float>(offset[a]) / sample_rate;
        u.duration      = count * hop_seconds;
        u.rms_mean      = static_cast<float>(rms_sum / count);
        u.speech_ratio  = static_cast<float>(spoken) / count;
        u.voiced_frames = static_cast<int>(voiced.size());
        if (!voiced.empty()) {
            double sum = 0.0;
            for (size_t i = 0; i < voiced.size(); ++i) {
                sum += voiced[i];
            }
            u.f0_mean = static_cast<float>(sum / voiced.size());
            u.f0_p10  = percentile(voiced, 0.10f);
            u.f0_p90  = percentile(voiced, 0.90f);
            // Semitones, because pitch is perceived as a ratio: 80 to 160 Hz and
            // 160 to 320 Hz are the same interval and must score the same.
            if (u.f0_p10 > 0.0f) {
                u.f0_range_st =
                    static_cast<float>(12.0 * std::log(u.f0_p90 / u.f0_p10) / std::log(2.0));
            }
        }

        out->utterances.push_back(u);
        f0_means.push_back(u.f0_mean);
        rms_means.push_back(u.rms_mean);
        speech_ratios.push_back(u.speech_ratio);
    }

    mean_and_sd(f0_means, &out->baseline.f0_mean, &out->baseline.f0_sd);
    mean_and_sd(rms_means, &out->baseline.rms_mean, &out->baseline.rms_sd);
    mean_and_sd(speech_ratios, &out->baseline.speech_mean, &out->baseline.speech_sd);
    return true;
}

Deviation deviation(const Recording& recording, size_t index) {
    Deviation d;
    if (index >= recording.utterances.size()) {
        return d;
    }
    const Utterance& u = recording.utterances[index];
    const Baseline&  b = recording.baseline;
    d.f0         = (u.f0_mean - b.f0_mean) / b.f0_sd;
    d.loudness   = (u.rms_mean - b.rms_mean) / b.rms_sd;
    d.continuity = (u.speech_ratio - b.speech_mean) / b.speech_sd;
    return d;
}

float arousal(const Recording& recording, size_t index, const ArousalWeights& w) {
    const Deviation d = deviation(recording, index);
    return w.f0 * d.f0 + w.loudness * d.loudness + w.continuity * d.continuity;
}

}  // namespace voice
}  // namespace bodyclock
