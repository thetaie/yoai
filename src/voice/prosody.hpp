// What the voice says about the speaker, measured from the audio alone.
//
// No model, no history, no baseline accumulated over days: a recording is cut at
// its pauses and each utterance is scored against *that recording's own*
// distribution. Microphone distance, recording gain and the speaker's own pitch
// range are constant within a take, so they cancel, and the numbers mean
// something on the first recording anyone ever makes. The argument, and the
// measured result this implementation reproduces, are in design/voice.md.
//
// Two rules from that document are load-bearing here and are worth restating
// where the code is:
//
//   1. **Segmentation is the unit, not a preprocessing step.** The same features
//      found nothing on ten-second fixed windows and located the emotional turn
//      exactly when cut at pauses. Everything below is per utterance because the
//      cut is what makes the numbers mean anything.
//
//   2. **Measurements are stored; axes are derived.** Utterance carries what was
//      measured and nothing that was concluded. arousal() is a function over
//      those measurements, not a field in them, because today's weighting of
//      pitch against continuity is a guess and re-deriving it must not require
//      keeping the audio.
//
// What this deliberately cannot say: whether a whole recording is low. Every
// take averages to zero by construction. Level across days needs a baseline this
// project has decided not to build yet -- see the cross-day section of
// design/voice.md for why the obvious ones do not survive contact with reality.

#pragma once

#include <cstddef>
#include <vector>

namespace bodyclock {
namespace voice {

struct ProsodyOptions {
    float frame_length = 0.040f;   // seconds
    float frame_shift  = 0.020f;

    // Silence is a fraction of THIS recording's peak, not a fixed dBFS: level
    // varies with microphone distance and gain, and an absolute threshold drifts
    // between takes without saying so.
    float silence_fraction = 0.08f;

    // A pause is this much silence. Under about 0.15 s a gap is a stop closure
    // inside a word rather than a boundary between phrases; too long and two
    // sentences merge into one unit and average each other out, which is the
    // fixed-window problem again in miniature.
    float min_pause = 0.30f;

    // Shorter runs of speech are dropped: below this there are too few voiced
    // frames for a pitch distribution to mean anything. A one-word segment has a
    // pitch, not a range.
    float min_utterance = 0.80f;

    // Pitch search range, and the normalized autocorrelation peak below which a
    // frame is called unvoiced. 0.3 keeps breathy voiced frames while dropping
    // silence and fricatives.
    float f0_min = 60.0f;
    float f0_max = 400.0f;
    float voicing_threshold = 0.30f;
};

// One run of speech between pauses, and what was measured in it.
//
// Nothing here is a conclusion. `f0_mean` is hertz, `rms_mean` is amplitude in
// whatever units the caller's samples were, `speech_ratio` is a fraction --
// three numbers a person could check with a different tool.
struct Utterance {
    size_t first_frame = 0;   // inclusive, into the analysis frame grid
    size_t last_frame  = 0;   // inclusive
    float  start       = 0.0f;   // seconds into the recording
    float  duration    = 0.0f;

    float f0_mean      = 0.0f;   // over voiced frames only; 0 when none were
    float f0_p10       = 0.0f;
    float f0_p90       = 0.0f;
    float f0_range_st  = 0.0f;   // p90 over p10, in semitones
    float rms_mean     = 0.0f;
    float speech_ratio = 0.0f;   // frames above the silence floor, over all frames
    int   voiced_frames = 0;
};

// The recording's own distribution -- the thing every utterance is compared to,
// and the reason none of this needs a personal history.
struct Baseline {
    float f0_mean = 0.0f,     f0_sd = 0.0f;
    float rms_mean = 0.0f,    rms_sd = 0.0f;
    float speech_mean = 0.0f, speech_sd = 0.0f;
};

struct Recording {
    std::vector<Utterance> utterances;
    Baseline baseline;
    float duration = 0.0f;   // seconds
    float peak_rms = 0.0f;   // what the silence threshold was a fraction of
};

// `pcm` is mono at `sample_rate`. Scale does not matter -- every number that
// leaves here is either a ratio, a frequency, or a deviation in units of this
// recording's own spread, so [-1, 1] floats and raw int16 values give the same
// answers.
//
// Returns false when the audio is too short to frame. **Succeeding with fewer
// than a handful of utterances is not the same as succeeding**: z-scores over
// three units are noise. The caller should check `utterances.size()` and decline
// rather than report a confident number -- roughly five is the floor, which at
// conversational rates is fifteen to twenty seconds of speech.
bool analyze(const float* pcm, size_t n_samples, int sample_rate,
             const ProsodyOptions& options, Recording* out);

// How far one utterance sits from the recording's own middle, per feature, in
// standard deviations. Derived, never stored.
struct Deviation {
    float f0         = 0.0f;
    float loudness   = 0.0f;
    float continuity = 0.0f;
};

Deviation deviation(const Recording& recording, size_t index);

// Pitch carries most of it. Loudness is the least trustworthy of the three and
// is weighted down rather than dropped: across a recording it tracks microphone
// distance as much as effort, and on a coarser cut of the validated clip it
// ranked the segments in the OPPOSITE order to everything else.
struct ArousalWeights {
    float f0         = 0.45f;
    float loudness   = 0.20f;
    float continuity = 0.35f;
};

float arousal(const Recording& recording, size_t index,
              const ArousalWeights& weights = ArousalWeights());

}  // namespace voice
}  // namespace bodyclock
