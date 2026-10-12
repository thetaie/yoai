#include "voice/prosody/prosody.hpp"

#include <algorithm>
#include <cmath>

namespace yoai {
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

// Everything both entry points share: frame the audio, then measure whatever
// frame ranges it is handed. Split out rather than duplicated -- a second copy
// of the pitch and baseline arithmetic is a second place for the two cuts to
// quietly stop being comparable.
namespace {

struct Framing {
    std::vector<float>  rms;
    std::vector<size_t> offset;
    float floor_rms   = 0.0f;
    float hop_seconds = 0.0f;
    int   frame_length = 0;
};

bool frame_audio(const float* pcm, size_t n_samples, int sample_rate,
                 const ProsodyOptions& opt, Framing* f, Recording* out) {
    const int frame_length = static_cast<int>(sample_rate * opt.frame_length);
    const int frame_shift  = static_cast<int>(sample_rate * opt.frame_shift);
    if (frame_length <= 0 || frame_shift <= 0 ||
        n_samples <= static_cast<size_t>(frame_length)) {
        return false;
    }

    out->duration   = static_cast<float>(n_samples) / sample_rate;
    f->hop_seconds  = static_cast<float>(frame_shift) / sample_rate;
    f->frame_length = frame_length;

    // The last partial frame is dropped rather than zero-padded -- padding would
    // invent a quiet frame and can close a pause the speaker did not take.
    for (size_t i = 0; i + frame_length < n_samples; i += frame_shift) {
        double sum = 0.0;
        for (int j = 0; j < frame_length; ++j) {
            const double v = pcm[i + j];
            sum += v * v;
        }
        f->rms.push_back(static_cast<float>(std::sqrt(sum / frame_length)));
        f->offset.push_back(i);
    }
    if (f->rms.empty()) {
        return false;
    }

    out->peak_rms = *std::max_element(f->rms.begin(), f->rms.end());
    f->floor_rms  = (out->peak_rms > 0.0f ? out->peak_rms : 1.0f) * opt.silence_fraction;
    return true;
}

// Measure one inclusive frame range. Pitch is computed only on frames above the
// floor: running it over silence would drag every mean toward whatever the
// autocorrelation finds in noise.
Utterance measure(const float* pcm, int sample_rate, const Framing& f,
                  const ProsodyOptions& opt, size_t a, size_t b) {
    std::vector<float> voiced;
    double rms_sum = 0.0;
    int    spoken  = 0;
    for (size_t i = a; i <= b; ++i) {
        rms_sum += f.rms[i];
        if (f.rms[i] > f.floor_rms) {
            ++spoken;
            const float hz = frame_f0(pcm + f.offset[i], f.frame_length, sample_rate, opt);
            if (hz > 0.0f) {
                voiced.push_back(hz);
            }
        }
    }

    const size_t count = b - a + 1;
    Utterance u;
    u.first_frame   = a;
    u.last_frame    = b;
    u.start         = static_cast<float>(f.offset[a]) / sample_rate;
    u.duration      = count * f.hop_seconds;
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
    return u;
}

void fill_baseline(Recording* out) {
    std::vector<float> f0s, rmss, speech;
    for (size_t i = 0; i < out->utterances.size(); ++i) {
        f0s.push_back(out->utterances[i].f0_mean);
        rmss.push_back(out->utterances[i].rms_mean);
        speech.push_back(out->utterances[i].speech_ratio);
    }
    mean_and_sd(f0s, &out->baseline.f0_mean, &out->baseline.f0_sd);
    mean_and_sd(rmss, &out->baseline.rms_mean, &out->baseline.rms_sd);
    mean_and_sd(speech, &out->baseline.speech_mean, &out->baseline.speech_sd);
}

}  // namespace

bool analyze_segments(const float* pcm, size_t n_samples, int sample_rate,
                      const std::vector<std::pair<float, float> >& segments,
                      const ProsodyOptions& opt, Recording* out) {
    if (!out) {
        return false;
    }
    *out = Recording();
    if (!pcm || sample_rate <= 0) {
        return false;
    }

    Framing f;
    if (!frame_audio(pcm, n_samples, sample_rate, opt, &f, out)) {
        return false;
    }

    // Adjacent segments are MERGED until each unit reaches min_utterance, and
    // that is not tidying -- it is the same rule the pause cut applies, for the
    // same reason. Below about 0.8 s there are too few voiced frames for a pitch
    // distribution to mean anything, and this file's autocorrelation leans toward
    // reporting an octave high when it is starved.
    //
    // Skipping the rule was measured and it showed: raw punctuation clauses ran
    // as short as 0.8 s, one of them came back at 216 Hz against a speaker
    // baseline of 123, and that single octave error pushed the recording's own
    // standard deviation from 11 Hz to 28 -- inflating the yardstick every other
    // utterance is measured against, and making itself the maximum.
    const int min_frames = static_cast<int>(opt.min_supplied_utterance / f.hop_seconds);
    const long last_frame = static_cast<long>(f.rms.size()) - 1;

    long open_a = -1;
    long open_b = -1;
    for (size_t s = 0; s < segments.size(); ++s) {
        // Clamped to the frame grid. A boundary past the end is not an error: a
        // transcript timestamp is 60 ms granular and the last one can land just
        // beyond the final whole frame.
        long a = static_cast<long>(segments[s].first / f.hop_seconds);
        long b = static_cast<long>(segments[s].second / f.hop_seconds) - 1;
        if (a < 0) a = 0;
        if (b > last_frame) b = last_frame;
        if (b <= a) {
            continue;   // shorter than one frame
        }

        if (open_a < 0) {
            open_a = a;
        }
        open_b = b;

        if (open_b - open_a >= min_frames) {
            out->utterances.push_back(measure(pcm, sample_rate, f, opt,
                                              static_cast<size_t>(open_a),
                                              static_cast<size_t>(open_b)));
            open_a = -1;
        }
    }
    // A trailing run too short to stand alone joins the previous unit rather than
    // being dropped: the words were spoken, and the tail of a recording is where
    // a closing thought lives.
    if (open_a >= 0 && open_b > open_a) {
        if (!out->utterances.empty()) {
            const size_t prev = out->utterances.size() - 1;
            out->utterances[prev] = measure(pcm, sample_rate, f, opt,
                                            out->utterances[prev].first_frame,
                                            static_cast<size_t>(open_b));
        } else if (open_b - open_a > 1) {
            out->utterances.push_back(measure(pcm, sample_rate, f, opt,
                                              static_cast<size_t>(open_a),
                                              static_cast<size_t>(open_b)));
        }
    }

    fill_baseline(out);
    return true;
}

bool analyze(const float* pcm, size_t n_samples, int sample_rate,
             const ProsodyOptions& opt, Recording* out) {
    if (!out) {
        return false;
    }
    *out = Recording();
    if (!pcm || sample_rate <= 0) {
        return false;
    }

    Framing f;
    if (!frame_audio(pcm, n_samples, sample_rate, opt, &f, out)) {
        return false;
    }

    // Runs of speech separated by pauses. A pause has to be whole: a gap shorter
    // than min_pause does not end an utterance, it just sits inside one, which is
    // why speech_ratio can be below 1.
    const int gap_needed = static_cast<int>(opt.min_pause / f.hop_seconds);
    const int min_frames = static_cast<int>(opt.min_utterance / f.hop_seconds);

    std::vector<std::pair<size_t, size_t> > runs;
    size_t begin = 0;
    bool   open  = false;
    size_t last  = 0;
    int    gap   = 0;
    for (size_t i = 0; i < f.rms.size(); ++i) {
        if (f.rms[i] > f.floor_rms) {
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

    for (size_t r = 0; r < runs.size(); ++r) {
        const size_t a = runs[r].first;
        const size_t b = runs[r].second;
        if (static_cast<int>(b - a) < min_frames) {
            continue;
        }
        out->utterances.push_back(measure(pcm, sample_rate, f, opt, a, b));
    }

    fill_baseline(out);
    return true;
}

namespace {

// One feature's leave-one-out deviation for `index`.
//
// With two or fewer utterances there is no distribution to compare against --
// one other unit has no spread of its own -- so the honest answer is 0 rather
// than a number derived from a single neighbour.
float deviate(const std::vector<float>& values, size_t index) {
    if (values.size() < 3 || index >= values.size()) {
        return 0.0f;
    }
    double sum = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != index) {
            sum += values[i];
        }
    }
    const size_t n = values.size() - 1;
    const double mean = sum / n;
    double var = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != index) {
            const double delta = values[i] - mean;
            var += delta * delta;
        }
    }
    const double sd = std::sqrt(var / n);
    if (sd <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>((values[index] - mean) / sd);
}

}  // namespace

Deviation deviation(const Recording& recording, size_t index) {
    Deviation d;
    if (index >= recording.utterances.size()) {
        return d;
    }
    std::vector<float> f0s, rmss, speech;
    f0s.reserve(recording.utterances.size());
    rmss.reserve(recording.utterances.size());
    speech.reserve(recording.utterances.size());
    for (size_t i = 0; i < recording.utterances.size(); ++i) {
        f0s.push_back(recording.utterances[i].f0_mean);
        rmss.push_back(recording.utterances[i].rms_mean);
        speech.push_back(recording.utterances[i].speech_ratio);
    }
    d.f0         = deviate(f0s, index);
    d.loudness   = deviate(rmss, index);
    d.continuity = deviate(speech, index);
    return d;
}

float arousal(const Recording& recording, size_t index, const ArousalWeights& w) {
    const Deviation d = deviation(recording, index);
    return w.f0 * d.f0 + w.loudness * d.loudness + w.continuity * d.continuity;
}

}  // namespace voice
}  // namespace yoai
