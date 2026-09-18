# Voice

How speech gets into the app and back out, what is read from it, and why none
of it is one model doing everything.

Numbers here are labeled: **measured** on this project's hardware, **measured
elsewhere** and reproduced with the method that produced them, or **derived**
from documented architecture.

## The decision

**Recognition and synthesis sit outside the language model, and what is read
from the voice is read by signal processing, not by a model.**

- **In:** SenseVoice-Small, bundled, `q8` — ported into this core in `src/voice/`,
  linking the ggml this repo already ships and not libllama. Transcription,
  language ID and audio events for Chinese, Cantonese, English, Japanese and
  Korean.
- **Out:** the platform's own text-to-speech.
- **The model in the middle:** anything. It is a text model now.
- **What the voice says about the speaker:** computed from the audio, per
  utterance, against that same recording — no model, no history, no baseline.

The third line is the largest consequence and the easiest to skim past. With an
omni model in the loop, voice would work only on the local lane — you cannot
post audio to an arbitrary OpenAI-compatible endpoint. With recognition outside,
**voice works with every provider the user has configured**. And the C++ core's
ABI already takes `messages_json`; transcription produces text.

## Where each axis gets its numbers

The rest of this file is the argument. This is the answer:

| axis | what it reads | from | status |
|---|---|---|---|
| **valence** | what is said | **text** | **built** — chat model, constrained reply |
| **arousal** | how it is said: pitch, continuity, loudness | **audio** | **validated** — the table further down |
| **dominance** | who is the subject of the verbs | **text** | **built**, unproven — same call as valence |
| **intensity** | distance from neutral, in any direction | **audio**, second pass | **built** — norm of the z-vector |
| **unpredictability** | how much the arousal series jumps | **audio**, second pass | **built** — mean \|Δ arousal\| |
| **approach–avoidance** | going-toward against putting-off wording | **text**, checked against the log | not built — weakest signal here |
| **potency** | — | — | not a separate axis; it is dominance |

"Second pass" means a function of numbers the arousal pass already produced —
no extra feature, no extra column in the database.

The two text axes carry a cost this table cannot show: **they are only as good
as the transcript, and the transcript has been measured getting a sentence's
polarity backwards.** See "Its transcription is the weak link" below before
treating either as settled.

### The text pair costs a sampler setting, and getting it wrong is silent

Valence and dominance come from one turn against whichever chat model is
configured, with a GBNF constraining the reply to
`{"valence":<level>,"dominance":<level>}` over five enumerated levels. The
grammar is not decoration: it makes every reply in range and parseable by
construction, and five levels is already finer than a small model resolves.

**A constrained turn must also be decoded greedily**, and this was learned the
expensive way. Asked to score one plainly cheerful sentence about lunch, a 2B at
the conversational defaults this project ships for chat — temperature 0.7,
top-k 20, top-p 0.8 — answered:

    -1  -1  -1   0  -1   0

six seeds, never twice the same, never right. Greedy, it answered 0 every time;
a 4B answered +0.5. **The sampling was the entire difference**, and on screen it
looked exactly like a model that could not read Chinese.

The repetition penalty has to go too, for a reason specific to scoring: it
demotes tokens seen in the last `repeat_last_n`, and a scoring prompt
necessarily shows the model its own scale — "-1 very unpleasant, 0 neutral" — so
the penalty lands on the handful of tokens the grammar allows as the answer.

Both are now automatic: a non-empty grammar makes the turn deterministic in
`llm/local.cpp`, documented at `bodyclock_chat`. The generalisation is worth
keeping — **a reply that is a value, not prose, must not be sampled like
prose.**

Model size matters here as well, though far less than for arousal: the 2B says
0 for that sentence even greedily, and the 4B says +0.5. Valence from text is a
task small models are good at, which is exactly why the axes are split by
channel — but 2B is below the floor.

All of it is computed **per utterance**, and the split that produces those
utterances decides what every number means. That has its own section below.

## Three axes, not a list of emotions

Emotion in speech is better described by a few independent axes than by a list
of labels. This is the PAD model — pleasure, arousal, dominance — and it is not
academic decoration. It is what stops three different questions from being
collapsed into one.

- **Valence** — pleasant to unpleasant. Is this good or bad?
- **Arousal** — activated to deactivated. How much energy is behind it?
- **Dominance** — in control or controlled. Is this person acting, or being
  acted on?

They are independent, and the states that agree on two axes but differ on the
third are exactly the ones a smaller model cannot tell apart:

|  | valence | arousal | dominance |
|---|---|---|---|
| anger | negative | **high** | **high** |
| fear | negative | **high** | **low** |
| sadness | negative | low | low |
| fatigue | **negative** | low | low |
| calm | **positive** | low | high |

Rows three to five are why valence and arousal are both needed: *depleted* and
*rested* are both "low energy" and mean opposite things, and a one-dimensional
tiredness scale scores them the same.

**What the third axis adds** is the first two rows, which are identical without
it. Anger and fear on their own would not pay for it — neither is a common daily
state. What pays for it is the everyday form of the same split: **"busy and on
top of it" against "busy and drowning."** Same high arousal, similar valence,
and opposite meaning for someone looking back over their own week. Likewise "I
decided to stop" against "I gave up," which V and A score alike.

**The three axes come from different channels**, and this is the part that
decides what has to be built:

- **Valence is semantic** — "today was awful" states it outright.
- **Arousal is acoustic** — activation shows in rate, pitch range and continuity
  before it shows in word choice.
- **Dominance is grammatical** — not which words are chosen but who is the
  subject of the verbs. "It got away from me," "I had to," "I couldn't," 「只能」
  「没办法」 put the speaker on the receiving end; "I stopped at four," "I decided
  to" put them at the wheel. The words can be identical in sentiment while the
  agency is reversed.

There is also an asymmetry worth knowing: **valence is easy to mask in words,
arousal is hard to mask in voice.** Someone saying "I'm fine" controls the
valence; the flat slow delivery leaks the arousal. Dominance sits with valence
here — it is said, so it can be dressed up.

**Measured**, on four ten-second segments of one real recording — the same model
asked the same question, once with audio and once with the transcript only:

| segment | words are about | from text | from audio |
|---|---|---|---|
| 1 | a birthday, thanking his daughter | happy | **slightly tired** |
| 2 | "glad and sad at once" | sad | slightly tired |
| 3 | "I lost control of my emotions" | sad | **sadness** |
| 4 | "let us live happily, stay safe" | happy | **calm** |

The two columns are not disagreeing. They are answering different questions.
Segment 1 is cheerful words in a tired voice, and only having both channels
shows that.

### Dominance is the weakest of the three, and that changes how it is used

Three things about it, none of them measured on this project's material:

**Annotators agree on it least.** Across the emotion-corpus literature, valence
and arousal ratings are reliable enough to train on and dominance is
consistently behind them; several well-known corpora publish V and A only. So
the axis is real but noisy, and it should never be the thing an answer turns on.

**Its best acoustic cue is already spent.** The cue most often reported for
dominance is pitch lowered against the speaker's own baseline — which is the
feature carrying arousal here. Inside one recording a drop in F0 cannot be
evidence for both at once, and arousal has the better claim on it — that is the
reading with a table behind it. So the main read on dominance comes from the
transcript, where it competes with nothing.

**Two cheap acoustic features are still open**, because arousal does not use
them: the **terminal contour** over the last fifth of an utterance — falling
reads as assertion, rising as asking for agreement — and the **disfluency rate**,
filled pauses and restarts, which is already listed below under fluency. Both
fall out of what is computed anyway.

The birthday monologue has a plain case of why the axis is worth having:
segment 3 is "I lost control of my emotions." Valence and arousal score that the
same as anger. It is the opposite of anger, and the difference is entirely
dominance — stated in the words, invisible in the sound.

**Nothing here has its own version of the arousal table yet.** That result rests
on measurements; this rests on reading. Compute it, store what it is computed
from, and keep it out of anything shown to a person until it has been checked
the same way.

## Segmentation is the unit, not a preprocessing step

Every number in this file is per utterance. Cutting the recording is not
something that happens before the analysis — it is the analysis's coordinate
system, and the same features give opposite answers under different cuts.

**That is measured, and it is the strongest single result here.** The prosody
features were first asked about ten-second fixed windows, found nothing, and the
conclusion written down at the time was that the path was closed. Re-cut at
pause boundaries, the same features on the same recording put the arousal
minimum exactly on the sentence where the speaker turns to his mother. Nothing
about the features changed. It is the third of the three withdrawn conclusions
below, and the only one where the fix was a change of unit.

Why a fixed window loses it: emotion in speech moves on the scale of a sentence,
and a ten-second window spanning the cheerful opening and the turn that follows
averages the two into nothing. Averaging is all a window does.

Windows are also not comparable to one another, which matters more than it
sounds. The whole method is a z-score against the recording's own distribution,
and a distribution needs units that mean the same thing. One window half full of
silence and one window of continuous speech are not two samples of one
population.

And an utterance is something a person can find again. "You slowed down thirty
percent after the two-minute mark" is a claim about a stretch nobody remembers.
"You slowed down on the sentence about your mother" can be checked. The rule at
the end of this file — never a label, always a deviation the speaker can verify
— needs the unit to be something the speaker recognizes.

### How the cut is made now

**Measured**, the working version, no model: frame the audio at 40 ms with a
20 ms hop, take RMS per frame, and treat anything under **8% of that recording's
own peak** as silence. A **pause is 0.30 s or more** of it. Runs of speech
shorter than **0.8 s** are dropped. On the 37-second monologue that gives eleven
utterances.

Each number is doing a job:

- **8% of the clip's own peak, not a fixed dBFS.** Level varies with microphone
  distance and gain, and a fixed threshold drifts between takes without saying
  so.
- **0.30 s.** Under about 0.15 s a gap is a stop closure inside a word, not a
  boundary between phrases. Too short and phrases fragment; too long and two
  sentences become one unit and average each other out, which is the fixed
  window problem again in miniature.
- **0.8 s.** Below that there are too few voiced frames for an F0 distribution
  to mean anything. A one-word segment has a pitch, not a range.

### Three different things want to cut the audio, and only one of them is this

- **The VAD** (`fsmn-vad`) is there for **recognition** — keeping silence out of
  the encoder and chunking long audio to fit. It may put a boundary anywhere
  convenient.
- **Prosodic segmentation** is there for **analysis**, and its boundaries become
  the units compared against one another and possibly described to a person.
- **The transcript supplies boundaries too, and they are now free.** Greedy CTC
  knows which encoder frame emitted each token, and an encoder frame is 60 ms,
  so every token carries a timestamp out of the same forward pass at no extra
  cost. `src/voice/sensevoice.hpp` returns them. This was an open question until
  the port was written; it is answered, and it means punctuation is available as
  a boundary. Punctuation beats pauses — people pause where sentences do not
  end, and end sentences without pausing — and a pause confirmed by punctuation
  beats either alone.

They can share an energy envelope. **They must not share parameters.** Reusing
the VAD's chunks because they are already computed would mean adopting
boundaries chosen for a different objective, which is the failure this file
keeps recording in other forms.

### Where it stops working

- **A recording too short to have a distribution.** Under about five utterances
  the z-scores are noise, and at the rate measured here that is roughly fifteen
  to twenty seconds of speech. "Going to bed" is two seconds and one utterance.
  The right output there is **nothing**, not a weak number.
- **Speech with no pauses** — fast, continuous, or read aloud from a page —
  gives one long segment and nothing to compare it against.
- **A noisy room** lifts the floor until nothing falls under the threshold and
  no boundary is found at all.

All three are detectable before any analysis runs, which is what makes them
tolerable: the pipeline can decline instead of guessing.

#### The second one is not an edge case — measured on the device

A 16-second note, recorded on the phone, ordinary conversational Mandarin, cut
into **two** utterances: a 0.9 s fragment and then one run of **1.3 s to
15.8 s**. Its transcript has five commas and a full stop in it, so there were
six clause boundaries in that audio and the energy floor found none of them.

The giant segment reports `range 28.7 st, p10 76 Hz, p90 400 Hz` — both ends
pinned to the F0 search bounds. That is not a wide pitch range, it is the
absence of a measurement: fifteen seconds of speech averaged into one "unit"
whose pitch distribution spans everything the estimator can represent.

Two corrections follow.

**"Fast, continuous, or read aloud" understates it.** This was none of those. It
was someone describing their lunch. Fluent speech simply does not stop for
0.30 s between clauses as often as the threshold assumes, and Mandarin — where
the comma does most of the work an English full stop does — gives it even fewer
chances.

**The five-utterance floor is therefore not mainly about duration.** Earlier
text here estimated "roughly fifteen to twenty seconds of speech" to reach five
utterances, and a separate 14-second clip did reach seven. The variable is not
length, it is **how the speaker phrases**. Two recordings of the same duration
can yield two segments or seven, so no recording-length rule can predict
whether the analysis will run.

That makes the transcript boundaries in the section above stop being an
optimization. **They are what makes the floor reachable at the lengths people
actually record** — notes here run 5 to 15 seconds, not 37. The cost is real
and is recorded with the design of the new cut: `speech_ratio` measures pause
structure *inside* a unit, so a unit cut at a clause boundary has nothing left
to measure and continuity — 0.35 of the arousal weight — degenerates. A cut
that keeps continuity meaningful has to either confirm punctuation against a
pause, as the section above suggests, or compute continuity over a window
rather than over the unit.

### The cut has to be versioned

Change the pause threshold and every stored number changes, because every stored
number is per segment. Rows cut by two different segmenters are not comparable,
and the cross-day work described below — if it is ever built — would compare
them without noticing. The segmenter's parameters belong stored beside the
features, so a later version can re-cut from the audio where it still exists and
leave the old rows out where it does not.

## Within the recording, against itself

**This is the part that is validated, and it is deliberately the smallest
claim.** Score each utterance, cut as above, as a deviation from *that
recording's own* distribution of pitch, continuity and loudness.

Microphone distance, recording gain and the speaker's own pitch range are all
constant within a take, so they cancel. No personal history is needed. It works
on the first recording anyone ever makes.

**Measured** on a 37.4-second monologue, eleven utterances:

| # | at | arousal | what he is saying |
|---|---|---|---|
| 0 | 0.1 s | **+1.40** highest | "Today is my sixty-third birthday." |
| 5 | 15.9 s | **−0.85** lowest | "What hurts is, I thought of my mother — the mother who bore me and raised me." |
| 6 | 21.7 s | −0.34, lowest F0 of the whole take | "They say a birthday is the day your mother suffered." |
| 9 | 32.0 s | +0.80 | "From now on let us live happily." |

**The arousal minimum lands exactly on the sentence where he turns to his
mother**, and the maximum on the opening announcement. That is not a statistical
wobble; it is an acoustic turn at a content turn, found without reading a word
of the content.

Pitch carries most of it. Loudness is the least trustworthy feature and is
weighted down: across a recording it tracks microphone distance as much as
effort, and on a coarser cut of this same clip it ranked the segments in the
*opposite* order to everything else.

### It is in the core now

`src/voice/prosody.cpp`, needing neither a model nor ggml — which is the whole
point of taking arousal from the signal. `bodyclock-prosody-test` prints the
table above from a WAV.

**Measured**: against the tool that produced the original result, on the same
16 kHz samples, it agrees on **every column of all eleven rows** — the same
utterance boundaries, F0 to a tenth of a hertz, all three deviations and the
arousal to two decimals, the same baseline of 123.5 Hz and sd 11.1. The columns
that matter are scale-invariant, so the two agree exactly or they do not.

It runs in **0.07 s**, against 5.4 s for the Python: 535× real time, and 77×
what the prototype did. The earlier note here that 5.4 s was "already 7× real
time" undersold the headroom by two orders of magnitude.

Two things are deliberately *not* in it. **Voice quality** — jitter, shimmer,
HNR — is not ported, because it does not work yet (see below). And the
autocorrelation normalizes by the whole frame's energy rather than by the
overlapping part, which leans slightly toward reporting a pitch an octave high;
that is a known flaw, kept on purpose, because it is what the validated result
was produced with and changing it would leave the numbers with nothing to be
checked against.

### The cross-day version is much harder, and is not being built yet

Normalizing within a recording means every recording averages to zero by
construction. It can say "this sentence was the low point of this note"; it
cannot say "today was lower than last week".

That second question needs a baseline accumulated over days, and the obvious
baselines do not survive contact with reality:

- **A single personal average is meaningless.** A voice on waking — swollen
  folds, low, creaky — and a voice after an afternoon outdoors are not
  comparable, and lumping them produces a number that mostly reports which of
  those happened.
- **Conditioning on the declared state is worse than not conditioning.** It is
  tempting: `voice_features` hangs off `entry_id`, so the state comes free from
  a join. But the state log is **user-declared data with known decay** — this
  repo has `end_at` precisely because people stop working at 17:00 and notice at
  23:00, and the day view has gaps because people forget. Comparing an
  after-nap voice against a "working" baseline because the label was stale
  produces a confident, systematic error — the same shape as every other
  failure recorded here: a wrong answer that looks right.

If it is built later, **condition on time of day, not on state**. Time of day is
observed rather than declared, is always correct, and captures the circadian
part of voice variation, which is the largest confound. Require a minimum sample
count per bucket and say so in the interface — "three more notes before this
means anything" — rather than computing a confident number from four samples.

**The state log is weak evidence, and that applies beyond voice.** The design's
own insight lines — "on days when your nap ran past 25 minutes, your working
time averaged 47 minutes more" — rest on nap durations being accurate. They are
self-reported. Anything built on them should say so.

## The dimensions past the three

The literature carries more axes than three, and they are not all the same kind
of thing. One is an axis already here under an older name, one has no path from
a microphone, and two fall out of the numbers above for almost nothing. Sorting
them is worth doing once, because the failure mode is adding an axis because it
exists rather than because it changes an answer.

Where each of them would read from is in the table at the top of this file. The
question here is the other one — whether it is a separate axis at all:

| | a distinct axis? |
|---|---|
| **potency** | no — dominance in an older vocabulary |
| **unpredictability** | yes, though from appraisal theory rather than PAD |
| **intensity** | yes — magnitude rather than direction |
| **approach–avoidance** | yes — anger is unpleasant *and* approach |

**Potency is dominance renamed.** Osgood's semantic differential factored
judgments into evaluation, potency and activity; PAD is those three under other
names — pleasure for evaluation, arousal for activity, dominance for potency.
There is a shade between them, potency reading as the strength of the thing
being judged and dominance as the speaker's own felt control, but no feature set
or corpus separates them. It is written down here only so that someone arriving
from the other tradition does not add a fourth axis that is the third.

**Unpredictability is one pass away from being computed.** Its home is
appraisal theory rather than PAD — the study that argued explicitly that the
emotion space is not two-dimensional landed on four, adding unpredictability to
pleasantness, potency-control and activation. What makes it cheap here is
mechanical: the arousal profile produces a per-utterance z-score series, and
unpredictability is the **second-order** statistic of that series — how much it
jumps, not where it sits. A note that walks steadily and a note that lurches can
have the same mean arousal. It is one pass over numbers already in memory.

One trap, and it is the same one this document keeps recording: two different
things share the word. Whether the *day* was surprising is semantic; whether the
*delivery* lurches is acoustic. Measuring the second and reporting the first is
how this becomes the fourth withdrawn conclusion.

**Intensity is the magnitude, not the level.** Arousal asks which direction;
intensity asks how far from neutral in any direction. They come apart on exactly
the states this app exists for: grief is low arousal and high intensity, and so
is genuine exhaustion, which is why one "how activated" scale reads both as
nothing much happening. Mechanically it is the norm of the same z-vector that
arousal takes a weighted sum of — identical inputs, magnitude instead of sign.

Its limit is one already recorded above: within-recording normalization sets
every take's average to zero, so this can say which utterance in a note was the
most marked and never that the whole note was heavy. Global intensity needs the
cross-day baseline that is not being built. That constraint belongs to the
pipeline, not to the axis, and it applies the same way to all of them.

Its acoustic side also sits close to the **vocal effort** row below — how hard
the voice is working, independent of activation. If effort is implemented, check
the two against each other before treating them as separate signals.

**Approach–avoidance has the only outside check, and the weakest signal.** It is
genuinely not valence: anger is unpleasant and approach-motivated, which is the
observation that established the axis in the first place. But it is measured in
the literature with frontal EEG asymmetry and behavioral tasks, not with
microphones, and its acoustic correlates are weak and tangled with arousal and
dominance. From the transcript it is partly readable — "I'm going to," "let's"
against "I've been putting it off," "I didn't want to deal with it."

What keeps it in view is that it is the only dimension here that predicts
something outside the audio: avoidance predicts an **absence** in the log.
Someone who says three times in a week that they should go running, with no
exercise entry all week, is saying something the sound cannot. But see the
section above — the absence might be a forgotten tap. That is evidence across
many days, never on one.

A word this document now uses twice: **agency**. Above it means who is the
subject of the verbs, which is dominance. In the motivation literature it is
often paired with approach–avoidance instead. Where this file says agency, it
means the first.

**Neither of the two worth building adds a stored column.** Both are functions
of the per-utterance measurements in the next section, which is the same rule
the axes already follow.

**And the rule for adding any of them:** each axis multiplies the ways to be
confidently wrong, and this file already records three conclusions withdrawn for
being read past what they could support. An axis earns its place by changing an
answer the app would otherwise give — not by appearing in a paper. Like
dominance, the two being built stay out of anything shown to a person until they
have their own version of the arousal table.

## Store measurements, not conclusions

Per utterance: `f0_mean`, `f0_p10`, `f0_p90`, `f0_end_slope`, `rate_syl`,
`pause_ratio`, `pause_count`, `rms_rel`, and the voice-quality measures once they
are correct. Hung off `entry_id`, with the utterance's offsets into the
recording and the `seg_version` that cut it.

`f0_end_slope` is the terminal contour, and it is in the list for dominance
rather than arousal — one extra regression over frames already computed.
Disfluencies are not a separate measurement at all: they are counted off the
transcript against those same offsets.

The axes are computed from these at read time, not stored. The reason is the
one the timeline schema already uses for dates: **the derived thing will change
and the measured thing will not.** Today's weighting of pitch against continuity
is a guess; re-deriving it must not require keeping the audio, because keeping
the audio is a privacy cost and a storage cost that features are not.

`rate_syl` looked close to free — the transcript gives the character count, the
energy envelope gives the duration, so speaking rate is a division rather than an
estimate. **It is free only with inverse text normalization turned off.** With it
on, which is what the GGUF asks for, the same clip comes back as `约567，大卡`
instead of `约五百六十七大卡`: three characters where four syllables were spoken.
Rate is then wrong at every number, and wrong in one direction, so an utterance
that happens to contain a figure reads as slower than its neighbours for a reason
that has nothing to do with the speaker. Within-recording comparison is exactly
where that does damage.

`SenseVoice::set_inverse_text_normalization(false)` turns it off. The catch is
measured and awkward: **the same switch also removes all punctuation**, and the
segmentation section wants punctuation as a boundary source. So the two uses pull
opposite ways — analysis wants it off, boundaries and display want it on — and
the choice is either two passes at 20x real time each or a syllable count that
expands digits back out per language. Not yet decided; what is decided is that
counting characters off an ITN transcript is not an option.

## What else the voice carries

Past the three emotional axes are the dimensions that are not emotion at all,
and for an app about the body they are the more interesting half:

| | what it is | cost |
|---|---|---|
| **vocal effort / strain** | how hard speaking is — independent of arousal; one can be low-energy and high-effort | needs spectral work |
| **voice quality** | breathy, creaky, tense — jitter, shimmer, HNR | needs proper glottal pulse marking |
| **fluency** | filled pauses, restarts, prolongations — cognitive load and disturbance both raise it; also the acoustic half of dominance | shares the features already computed |

These are **falsifiable in a way emotion is not**. "Your voice is hoarser than
usual" is something the speaker can check against their own throat. "You sound
sad" is something they can only accept or resent.

A caution on the literature: voice as a health biomarker is an active field with
mixed results. The near signals — respiratory illness, vocal fatigue — hold up
much better than the distant ones. Build the near ones; leave the screening
claims alone.

**Voice quality is implemented and wrong.** A first pass produced jitter around
6.5% and HNR around 4 dB, where healthy voices sit under 1% and above 20 dB.
The per-cycle peak picking is not stable enough on real recordings. It needs the
proper pulse-marking approach before any of those numbers are used.

## Choosing the recognition model

### SenseVoice-Small, and why an old model is still the answer

SenseVoice-Small has not been updated in about two years, and its transcription
has been overtaken: **Qwen3-ASR** (0.6B and 1.7B) covers thirty languages, holds
up better in difficult acoustics, and unifies streaming with offline inference.
On transcription alone it wins.

But nothing has replaced the combination — transcription, language ID, emotion
and audio events in one small model. The two quiet years mean the niche is
uncontested, not that the model was surpassed in it.

**Its emotion labels are not what this app needs, and that is a label-space
problem rather than a perception one.** The vocabulary is `HAPPY SAD ANGRY
NEUTRAL FEARFUL DISGUSTED SURPRISED` — categories, and all of them high-arousal
except `SAD`. There is no word in it for tired, depleted, calm or content. The
whole low-arousal band collapses into `NEUTRAL`.

So when it returned `NEUTRAL` for all four segments of the birthday monologue —
including the one where a listener clearly heard grief — that is what the model
is able to say, not necessarily what it heard. **Measured**: the `f32` reference
returns `NEUTRAL` on all four too, so it is not a quantization artifact.

### Its transcription is the weak link, and the text axes eat the errors

**Measured** on the 37.4 s monologue, same 16 kHz file through all three, asked
for a verbatim transcript:

| | SenseVoice `q8` | Qwen2.5-Omni-3B | Qwen3-Omni-30B |
|---|---|---|---|
| 又是易拉宝又是… | 横**福** | 横**幅** | 横**幅** |
| 我是…高兴又难受 | **极**高兴 | **既**高兴 | **既**高兴 |
| 生活还算… | **忧郁** | **优越** | **优越** |
| 我多想告诉… | 告诉**他** | 告诉**她** | 告诉**她** |

Both omni models get all four right. **The third row is not a typo, it is a sign
flip**: the speaker said his life is *comfortable* (优越) in a sentence listing
what he is happy about, and the transcript says *melancholy* (忧郁). The sentence
now argues the opposite of what was said — and that sentence is what the valence
axis reads.

So the table at the top of this file is too generous. Valence is marked *free,
the model reads the transcript anyway*. **The transcript is free; its errors are
not.** A wrong word here does not look like a gap, it looks like a sentiment, and
it is fed to an axis that has no way to doubt it. This is the same failure shape
recorded throughout this file, arriving through a component that had been treated
as settled.

Worth being precise about what this does and does not say:

- **It is one clip.** Four errors is a case, not a rate. Whether this is
  occasional and tolerable or frequent enough to need a check is unmeasured.
- **Size is not the explanation.** The 3B gets it right too, and it is a dozen
  times SenseVoice, not a hundred. "It lost to a much bigger model" is not
  available as an excuse.
- **It separates two jobs cleanly, and that supports the design.**
  Transcription is easy for both omni sizes — 3B and 30B agree here. What
  separates them is *hearing tone*: on the three tape speeds below, the 30B
  tracks pitch and pace while the 3B says "moderate pace, low pitch, calm" to
  all three. Transcription does not need a large model; hearing tone does; and
  the one that hears tone does not fit on a phone. Which is why tone comes from
  signal processing — the same conclusion, reached from the transcript side.

Its transcription is what we want it for. Arousal comes from the signal
processing above.

### The four tags, and what each one is actually worth

The model answers four queries alongside the transcript. They are not equally
useful, and treating them as one feature would be a mistake in both directions —
ignoring the one that matters and trusting the one that does not.

| tag | verdict |
|---|---|
| **language** | **Use it.** `nospeech` is a gate, and the language decides the reply language and the TTS voice |
| **itn** | **Use it**, and control it — see the note on `rate_syl` above |
| **emotion** | Store it. Never show it. Worth one thing: disagreement |
| **event** | A hint that may only lower confidence, never gate |

**`language` is the one being wasted today.** `nospeech` means the pipeline
should decline rather than hand an empty or invented transcript to the language
model — and this file has already recorded that a wrong transcription does not
look like silence, it looks like text. The rest of the label decides which voice
speaks the answer back, and, in an app with eleven interface languages, what
language the answer should be in when someone speaks one the interface is not
set to.

**`itn` is an input as much as an output.** *Inverse text normalization*: a
speech synthesizer needs the written form turned into the spoken one, `567` into
"five hundred and sixty-seven", and that is text normalization; recognition needs
the opposite, hence *inverse*. `withitn` and `woitn` are with it and without it.

The four query tokens are prepended to the features, and the fourth selects
which; the answer just reports what was asked. **Measured**: the GGUF ships
`[0 1 2 14]` — automatic language detection, and normalization on.

**`emotion` stays out of sight, and has exactly one job.** Its label space has no
word for tired, depleted or calm, so a `NEUTRAL` carries almost nothing. But a
*non*-neutral label is a positive claim about a high-arousal state, and that can
be compared against the arousal computed from the signal. Agreement means
nothing; **disagreement is a cheap, per-recording version of the offline check
this file elsewhere proposes running an omni model for**. Log the disagreement
rate; never put the label on a screen.

**`event` is the weakest.** `BGM` would genuinely matter — background music
corrupts the F0 tracking and the energy envelope that arousal rests on — but this
file already records a clip where the tag read `Speech` throughout and was wrong.
So it can lower confidence in a measurement and must never be the thing that
decides whether one is taken.

**The replacement path, if transcription turns out not to be good enough:** move
recognition to Qwen3-ASR, which llama.cpp supports natively
(`PROJECTOR_TYPE_QWEN3A`) with no second runtime. Pay for it when the need is
demonstrated.

### `q8`, and not `f16` — the opposite of the obvious guess

Three builds are published: `f32` at 936 MB (the reference), `f16` at 470 MB,
`q8` at 254 MB. **Measured** here against `f32` on five clips — two Windows TTS
lines, a nutrition explainer, a monologue and a synthetic narration:

| | result |
|---|---|
| `f32` | punctuation, correct tags |
| **`f16`** | **the text-normalization head flips**: it answers `woitn` where `f32` answers `withitn`; on two clips the emotion and event tags vanish as well |
| `q8` | reproduces `f32` on every clip |

The missing punctuation and capitalization first recorded here as a second
symptom are **not** a second symptom. `woitn` is what removes them: the same
switch that writes `五百六十七` instead of `567` also drops the punctuation, as
the note on `rate_syl` above measures directly. One head flips, and everything
that looked like a second fault follows from that one.

`q8` is both the smaller file and the accurate one — likely because `q8_0` is
block-quantized with per-block scales, which suits this model's numeric range
better than a flat 16-bit mantissa.

What `f16` breaks is narrow and worth naming precisely: the transcribed **words
stay correct**. One of the four query heads answers wrongly, reproducibly, in one
direction — and it is the head, not the question. The build is still fed query
token 14; it answers as though it had been given 15, so
`set_inverse_text_normalization(true)` cannot straighten it out. **Anyone
verifying "is the transcription correct" would pass this build.** Upstream marks
`f16` and `q8` both recommended and reports `q8` at CER 7.99 against `f16`'s
identical figure — measuring transcription only, which is exactly the
measurement that cannot see this.

**Not a user-facing setting.** "Which quantization would you like" is not a
question anyone can answer about themselves, and choosing wrong is invisible —
demonstrably so. Per-*language* downloads are a different matter: the user knows
what language they speak, and the design already calls for exactly that, with
sizes shown in settings.

### A known failure: invented letters on some audio

On one 38-second synthetic narration, the CTC output interleaved isolated
capital letters — `S` eleven times, `E` and `N` six each — assembling into
`SESE`, `SNB`, `NCT` at sentence boundaries. **Measured**: all three precisions
do it, including `f32`, so it is the model and not the quantization. Adding the
VAD does not help; the energy envelope shows continuous speech with no silence
to cut. The cause is not established — two plausible explanations were checked
and both were wrong.

The three genuine human recordings were clean. But it means **the transcript
cannot go straight into the language model** — a wrong transcription here does
not look like silence, it looks like text. A filter for runs of isolated capital
letters is a few lines and belongs on the C++ side, shared by both platforms.

Also: the event tag was `<|Speech|>` throughout that clip, so the event field is
not dependable on anything unusual.

## How recognition gets built

FunASR ships its own llama.cpp runtime for SenseVoice, and it is far smaller
than that name suggests. **Measured** by building it here:

- It is **not a fork of llama.cpp** and does not patch it. Each binary links
  `ggml` only — not `llama` — and the CMake takes
  `-DFETCHCONTENT_SOURCE_DIR_LLAMA=<path>` to build against a checkout already
  present. It built against this repo's llama.cpp **unmodified**.
- The whole implementation is **401 lines**: `funasr-sensevoice.cpp` (191),
  `funasr_vad.h` (161), `funasr_audio.h` (49). The 191 carry the mel filterbank
  with its own FFT, the SAN-M attention and layer graphs, positional encoding,
  CTC decode and tag parsing. ggml graph code loops, so seventy encoder blocks
  are written once.
- The only heavy dependency, `miniaudio.h`, belongs to `funasr_audio.h` and
  decodes files from disk. On a device the microphone hands over PCM already, so
  it does not travel. (It also cannot open non-ASCII paths on Windows, which
  costs a desktop test nothing and would have cost a user everything.)
- **MIT for the code, a separate agreement for the weights.** The runtime and
  the rest of that repository are MIT. The official SenseVoiceSmall weights are
  not: they carry FunASR's model open-source agreement v1.1, which does permit
  commercial use but attaches attribution and model-name requirements, and which
  covers the official weights only — a third-party GGUF conversion has to be
  checked on its own terms. Bundling the model in the app is therefore a
  licensing step as well as a build step: the attribution has to land somewhere
  the user can reach, and Settings is where the other model information already
  is.

There is no second ggml, no engine to adopt, and nothing to keep in sync
upstream — recognition becomes a few source files inside this core, linked
against the ggml already built with this project's tuned `-march` flags. SAN-M
is attention plus depthwise convolution, which is what `i8mm` and `dotprod` are
for.

A separate VAD model (`fsmn-vad`, also GGUF) chunks long recordings, same shape,
one source file against the same ggml — **for recognition only**. Its boundaries
are not the analysis boundaries; see the segmentation section.

### It is built, and it matches the reference

`src/voice/` now carries the port: `fbank.*` for the front end and `sensevoice.*`
for the encoder, behind `-DBODYCLOCK_ASR=ON`, linking the ggml archives and not
libllama. `build.cmd asr` (or `./build.sh asr`) assembles a desktop ggml SDK and
turns the lane on; `bodyclock-sensevoice-test` exercises it.

The front end is a separate file for a reason: it is arithmetic with one right
answer, it needs no model, and getting it subtly wrong is the worst failure
available here — the encoder does not degrade gracefully on features that are
merely close, it produces confident text that is wrong. So it is checked exactly
and without a model: the filter a 1 kHz tone must land in, the frame count for
one second of audio, and byte comparisons that pin the low-frame-rate padding
and hop.

**Measured**, against the reference binary on three clips resampled to 16 kHz
beforehand so that both read the same file: the transcripts are
**character-for-character identical**, including punctuation and all four tags.
An earlier run differed by one character, which turned out to be the test
harness's own linear resampler rather than anything in the port — the kind of
difference that would have been easy to read as a porting error.

Two things fall out of writing it rather than calling it:

- **Timestamps.** Each token keeps the encoder frame that emitted it, so the
  transcript carries 60 ms alignment. See the segmentation section.
- **The stray-capital filter works.** On the clip where the model interleaves
  isolated capitals, the reference emits `SESE`, `NCT`, `SNB` and `ST`; the port
  removes exactly those and leaves the rest of the text identical. It runs on
  tokens, not on the finished string, so it cannot take "OK" or an initial with
  it.

**Measured on the desktop CPU, at `q8`:** 0.41 s for an 8.8-second clip, 1.79 s
for 32 seconds, 2.02 s for 38 — about **20× real time** on four threads, with
the reference at 1.50 s for the same 32 seconds on eight.

An earlier note here claimed 32× to 53×. That was wrong, and the reason is worth
keeping: the clips are 24 kHz, their durations had been computed as though they
were 16 kHz, and every one came out 1.5× too long. The times were real; the
speed-ups derived from them were not.

### On the device: one call, and a page to read it with

The C ABI is `bodyclock_voice_*`, and `bodyclock_voice_analyze` returns **one JSON
object carrying both halves** — transcript, the four tags, per-token times, and
the prosody profile with each utterance's measurements and its deviations.

The halves are independent in the ABI the way they are in the argument.
`prosody` comes back on a build with no recognition engine at all, and on a
device where the user has never picked a model. `transcript` is the optional
one. That ordering is not a detail: the half that needs no model is the half the
emotional reading comes from.

Each utterance also carries the words spoken inside it, matched by the token
timestamps. That is the table from the top of this file, produced by one call —
two independent measurements of the same seconds, side by side, with nothing
having read the content to place them.

**The bridge, and the two things in it that are not obvious.** `nativeVoiceLoad`
and `nativeVoiceAnalyze` run off the ArkTS thread, because 254 MB off disk and a
second of encoder are both long enough to drop frames. And the samples are
**copied on the ArkTS thread before the worker starts**: the `ArrayBuffer` they
arrive in belongs to the JS heap, and nothing stops it being collected or
detached while a worker reads it.

**Capture is opened at 16 kHz mono**, which is what the analysis wants, so
nothing resamples. The core refuses any other rate rather than converting. That
is deliberate and this file has already paid for the lesson: a resampler that
differs from the one a measurement was taken with changes the answer quietly.

**`/asr` in the composer opens the debug page.** Typed, not discoverable — it is
a developer page, and someone who needs it knows its name. It shows the prosody
profile first, the transcript second, and the model's own emotion tag last,
labelled as not trusted with the reason. Below five utterances it shows the
reason it is showing nothing.

**It runs on the device now.** The signing profile that blocked this — issued
for the bundle name that predates `8dcabe5 rename: YoAI becomes Body Clock` —
has been reissued, and the page has been driven through recordings on a Kirin
9020.

**Encoder speed, measured**, seconds of compute per second of clip, swept with
`bodyclock_voice_set_threads` without reloading the weights:

| threads | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| s per s of clip | 0.392 | 0.259 | **0.173** | 0.163 | 0.161 |
| × real time | 2.55 | 3.87 | **5.77** | 6.14 | 6.24 |
| against one thread | 1.0 | 1.51 | 2.27 | 2.40 | 2.43 |

**5.8× real time at four threads**, and four is where it stops paying: eight
buys 7%, which is 0.07 s on a four-second clip, against two more cores taken
from the UI. The default stays at 4.

**So recognition is not the term to attack.** A ten-second note costs 1.7 s.
That matters because the chain's own comment had it at "1.6× real time, already
known" — unmeasured, wrong by 3.6×, and wrong in the direction that would have
aimed the first optimisation at the cheapest stage.

Microphone gain and room noise are still unmeasured.

### The whole chain, measured: 1.4 seconds of silence

Recognition, an on-device answer, and the platform voice — end to end on the
Kirin. **The only number is the silence**: from the moment the person stops
talking to the first sound coming back. Everything after that first sound is
heard rather than waited through, because the platform voice speaks at 1× while
the model is still writing the next sentence.

| | first exchange, cold | second in a row | first exchange, warmed |
| --- | --- | --- | --- |
| model load | 3.42 s | 0 | 0 |
| recognition, per clip | 0.62 s (3.78 s of audio) | 0.51 s (3.10 s) | 0.55 s (3.30 s) |
| TTS engine start | 0.39 s | **0.00 s** — kept | **0.00 s** |
| prefill | 2.43 s, 0 of 119 reused | **0.29 s, 102 of 115** | **0.35 s, 102 of 116** |
| decode to the first sentence | 0.59 s (8 tok) | 0.62 s (9 tok) | 1.04 s (15 tok) |
| **silence** | **7.57 s** | **1.37 s** | **1.53 s** |

**The gap between the first two columns was the whole product decision, and the
third column closes it.** A session's first exchange cost five and a half times
its steady state, all of it in three costs that do not depend on what the person
said: the model load, an empty prefix cache, and creating the TTS engine.

**Warming on intent, not on entry.** They are paid when the chain is switched
on — about 5.5 s from the toggle to ready, spent while the person is still
deciding what to say — rather than when the page opens, because reading 1.28 GB
for somebody who came to look at a prosody profile is the page charging for a
feature they did not ask for. The prefix cache is filled by asking the model
something and discarding the answer; there is no prefill-only entry point.

With that, **the first recording of a session behaves like the steady state**:
1.53 s against 1.37 s, and the difference is a longer reply (15 decoded tokens
against 9), not a colder engine.

**In the steady state there is no dominant term left**: decode to the first
sentence 0.62 s (45%), recognition 0.51 s (37%), prefill 0.29 s (21%). That is
the signal to stop. Any single fix from here is worth two or three hundred
milliseconds.

It is also a caution about reading proportions. Recognition was 17% of the cold
exchange and the conclusion drawn from that was "not the term to attack" — true
of that exchange, and it became 37% the moment prefill collapsed. A share is a
statement about the other terms as much as its own.

**Against the omni lane, for the same job**: 1.37 s of silence here against
14.3 s before omni's first block of audio, and this one then speaks at 1× while
omni streams at 3.7×. Three engines that each do one thing beat one that does
all three, by an order of magnitude, on the only number a person experiences.

## The platforms do not offer the same thing

| | Recognition | Reliable? |
|---|---|---|
| **iOS** | `SFSpeechRecognizer` | Yes. On-device since iOS 13, broad language coverage, stable API |
| **HarmonyOS** | HMS Core Speech Kit | Likely. `@hms.ai.speechRecognizer` exposes an online/offline flag; every HarmonyOS device is Huawei hardware |
| **Android** | `SpeechRecognizer` | **No** |

Android is a façade over whatever recognition service is installed, normally
Google's — so it is **absent without Google Play Services**, which is most
phones in China. `createOnDeviceSpeechRecognizer()` arrived only in API 31 and
whether an on-device model exists is up to the vendor. Behaviour varies by OEM,
which is the kind of dependency that works on a developer's desk and fails on a
user's. Verify on target hardware; the specifics move.

**Hence bundled recognition as the baseline**, identical on all three platforms.
System recognition stays an optional fast path where it is genuinely better —
iOS especially, where it is free and covers more languages.

**Synthesis is the easier half.** All three platforms have usable offline
text-to-speech, and a wrong reading costs far less than a wrong transcription.
Use the system's.

## Why not an omni model

The obvious alternative was one model taking audio in and producing text with
tone included. It was tested at length. Two things rule it out; only the first
is settled.

### Throughput, which is the settled part

Measured elsewhere on the real target: a Kirin 9020 at 16 GB running Qwen3.5-2B
`Q4_K_M` (1.28 GB) gets **~43 tok/s prefill and 11.2 tok/s decode** against a
read bandwidth of ~~**19.5 GB/s that a single core already saturates**~~ — both
halves of that clause were later measured false, and the corrections are two
paragraphs down. The device reads **24.4 GB/s**, and it takes four cores.

That figure is worth being able to re-derive: decode reads every weight for
every token, so a model of size S cannot exceed bandwidth/S tokens per second —
15.2 for a 1.28 GB model, against 11.2 observed. No other compute unit moves
that wall; measured the same way, the Vulkan GPU came out **3.5× slower** than
CPU and the NPU's ceiling sat below it. Prefill is the exception, being
compute-bound.

**Confirmed at two sizes, through this app's own lane**, on the same prompt
(「您好，请问几点钟了？」, 19 tokens):

| | Size | Decode | Weights read | Of the 19.5 | Of the measured 24.4 | Load |
| --- | --- | --- | --- | --- | --- | --- |
| Qwen3.5-2B `Q4_K_M` | 1.281 GB | 14.0 tok/s | 17.9 GB/s | 92% | 73% | 5.9 s |
| Qwen3.5-4B `Q4_K_M` | 2.741 GB | 6.70 tok/s | 18.4 GB/s | 94% | 75% | 14.5 s |

**Decimal GB throughout**, because that is the unit the 19.5 is in and mixing
the two is a silent 7% error. A first version of this table used GiB under a GB
label and reported 85–88% against the 19.5 instead of 92–94%.

Two points, and the 4B's rate was predicted as ~6.5 before it was run. **The law
is what is settled**: doubling the model halves the tokens per second, because
decode reads every weight for every token. Which constant it runs into is a
separate question, and the last column is the one that turned out to matter.

**Two corrections that arrived later, both further down.** "A single core
already saturates it" is false — a thread sweep puts saturation at four, the
performance-core count. And **19.5 GB/s is not the chip's ceiling**: measured
with a read loop and no model at all, the device does at least 24.4 GB/s, of
which llama.cpp's decode uses 73–75%.

The inverse-with-size law holds and the two-point table above is still the way
to predict a model's decode rate **for this engine**. What does not hold is
reading that constant as physics. Anything below that argues "nothing can be
faster because the bus is full" needs re-deriving against 24.4, and the
headroom is real: MNN's thinker already decodes above what llama.cpp reaches.

Prefill came out 41.3 and 16.9 tok/s, but on a 19-token prompt that is mostly
fixed overhead and should not be quoted.

Audio is expensive to feed in: `mtmd` encodes roughly **25 audio tokens per
second** of clip (derived from Whisper's 50 Hz frames, pooled by two), so a
minute of speech is ~1500 tokens of prefill. Scaling to Qwen2.5-Omni-3B —
derived — gives ~26 tok/s prefill, so **60 seconds of audio costs about 58
seconds** before the answer starts. It scales linearly, so short clips do not
rescue it.

Recognition first changes that by an order of magnitude, because what reaches
the model is smaller: a minute of speech is ~200 text tokens, about 5 s of
prefill. Memory goes the same way — thinker `Q4_K_M` plus its mmproj is 4.7 GB
and the mmproj is not optional, against 1.53 GB for SenseVoice `q8` plus a 2B
text model, of which the recognition half is loaded only while transcribing.

**Measured** here for comparison: Qwen2.5-Omni took 36 s on the 48-second clip
that SenseVoice handled in 1.5 s. Twenty-four times — **but that pair is not
like for like**, and reading it as "recognition is 24× slower" is the mistake it
invites. The 36 s is a whole exchange, encoder and prefill and generation; the
1.5 s is transcription with no language model in it at all.

**The encoders are close. It is what they emit that differs.** Both measured on
the Kirin, per second of clip:

| | encoder | × real time |
| --- | --- | --- |
| SenseVoice, 4 threads | 0.173 s | 5.8 |
| Qwen2.5-Omni's audio encoder in MNN | ~0.31 s | ~3.2 |

Under two times apart. And the omni thinker's 9.3 s splits **audio 1.57 s,
prefill 5.68 s, decode 2.01 s** — the encoder is 17% of it and the prefill is
61%.

That is the whole argument, arriving as a measurement rather than a derivation:
an audio encoder hands the language model ~25 tokens per second of clip, and
every one is prefilled. A transcript of the same speech is nearer 4 tokens per
second. **Recognition first is not a faster encoder, it is a sixfold shorter
prompt** — and on this device that came to 5.68 s against about 1.5 s, of which
2.6× is the token count and 1.5× is 3B against 2B.

(The ~0.31 is inferred: that run logged its token count but not its clip length,
which is the bug the logging fix in `src/voice/omni.cpp` was for. The next run
prints the length directly.)

**The derived ~26 tok/s was close.** Measured on the phone through MNN, with the
prefill separated out: **182 tokens in 5.68 s, 32.0 tok/s**. So a minute of
speech — ~1500 audio tokens plus the template — is about **49 seconds** before
the answer starts, against the 58 s this paragraph derived. The derivation was
pessimistic by a fifth and right in every way that mattered.

**That extrapolation assumes prefill is linear in the token count, and it was
checked.** Two phone runs looked like it might not be: 182 tokens took 5.64 s
and 150 took 5.50 s, which fits a large fixed cost far better than a rate. A
large fixed cost would make long clips much cheaper than the line predicts, so
it mattered. Two desktop runs on the same clip with deliberately different
prompt lengths:

| prompt tokens | prefill | average |
| --- | --- | --- |
| 264 | 3.09 s | 85.4 tok/s |
| 465 | 5.75 s | 80.9 tok/s |

13.2 ms per token with an intercept of **−0.4 s** — no fixed cost, and the
average creeping up with length is attention's quadratic term showing through.
So the line holds and the phone pair is not structural. Most likely thermal:
those two runs were three minutes apart and the second followed a 38-second
Talker run on eight threads.

### Whether it hears tone at all — and three conclusions withdrawn

The original test was a two-way control. **Control 1** held the voice fixed and
changed the words: two clips from one Windows TTS voice, same flat delivery.
The verdict flipped from "a sense of exhaustion" to "not tired… calm and
collected". **Control 2** held the words fixed and changed the sound, by
re-rendering one real recording at three tape speeds. A 1.9× tempo ratio and a
large pitch shift left no trace.

That was reproduced here independently, and extended to seven clips across seven
speakers, where the model's verdict matched the emotional lean of the *words*
every time.

**And then it failed to hold, on better material.** Cutting one real recording
into four segments — same speaker, same take, so the only variable is what is
being said — the model called the *cheerful* opening "slightly tired", crossing
the content's lean. Asked the same question about the transcript with the audio
replaced by silence, it answered "happy" for that segment. **Same model, same
prompt, audio is the only difference, and the answer changes.** Qwen2.5-Omni
does use the audio.

Three things written here earlier are therefore withdrawn:

- **"The model reads the words, not the voice."** Overstated. The seven-clip
  table that supported it drew on seven different speakers, so acoustic and
  semantic differences were confounded; that table cannot separate the two
  explanations, and it was read as though it could.
- **"SenseVoice's emotion head is systematically broken."** It has no label for
  the low-arousal band at all. Missing vocabulary, not missing perception.
- **"Prosody features cannot find it, so that path is closed."** They were being
  asked to find *sadness* — a valence judgment — and at ten-second granularity.
  Asked for arousal at utterance granularity, they locate the emotional turn
  precisely, as the table above shows.

The pattern across all three: a measurement that could not distinguish two
explanations, read as though it had.

**None of this revives the omni path on-device.** The throughput wall is
independent of whether the model hears.

### The larger model was run, and it hears clearly

Two things written above are out of date. **llama.cpp does run Qwen3-Omni** —
`ggml-org/Qwen3-Omni-30B-A3B-Instruct-GGUF` ships `Q4_K_M` plus an mmproj, and
it loads and answers through `llama-mtmd-cli`. And the **base Instruct model was
enough**; the Captioner variant was not needed to get a clear result.

**Measured**, `Q4_K_M` plus a bf16 mmproj on an RTX 3090, 19.3 GB of 24, one
6.5-second clip of real Mandarin put through control 2 — the same words at three
tape speeds:

| | sound | what the model said |
|---|---|---|
| slow, 9.0 s | pitch down | "**mid-to-low pitched**… **moderate, conversational pace**… **His** delivery" |
| original, 6.5 s | — | "**fast-paced**… pitch consistently **high**… **Her** pitch" |
| fast, 4.8 s | pitch up | "**fast-paced**… **high-pitched**, animated" |

Pitch and pace both track the manipulation, and **the perceived speaker's sex
flips with the pitch shift** — which is what a pitch shift does to a listener,
and is not available from the words, because the words are identical in all
three. The semantic read stayed put across all three ("exasperated",
"animated"), which is the correct behaviour: the content-driven part should not
move when only the sound does.

The 3B, given the same treatment, said "moderate pace, low pitch, calm" for all
three.

So the question the two controls were built to answer has an answer. **It is a
capacity problem, not an impossibility.** A model of this class hears tone; the
one small enough to put on a phone does not.

This does not change the on-device plan — 19.3 GB is a server — but it changes
what the on-device plan *is*. Signal processing is not a fallback chosen because
the model idea failed. It is the only thing that works at the size that fits,
and it is the part that is validated.

Caveats: one speaker, one clip, one sample per condition. A tape-speed change is
not real emotional prosody; this shows the model is sensitive to acoustics, not
that it judges tiredness correctly. That needs one person recording one sentence
in several real states.

**Half the mmproj is the vision tower, and it is dead weight here.** "mmproj" is
the *multimodal* projector: for an omni model it carries an audio encoder and a
vision encoder side by side, and `clip_init` loads **every** tower the file
declares. There is no runtime switch for audio-only — the single skip in that
function is a hardcoded Gemma-3N special case — so a process that will never be
handed an image pays for the vision weights anyway.

[strip-mmproj-vision.py](strip-mmproj-vision.py) drops them: the `v.*` tensors
and the `mm.0.*` / `mm.2.*` projector MLP go, the `a.*` encoder and `mm.a.*` head
stay, every `clip.vision.*` key goes and `clip.has_vision_encoder` is set false —
that last flag being the one `clip.cpp` actually reads. It is a metadata rewrite,
not a requantization: each kept tensor is copied byte for byte at its original
type, for the reason the `q8`/`f16` section gives at length.

**Measured**, both models, and verified by loading rather than by parsing:

| | before | after | dropped |
|---|---|---|---|
| Qwen2.5-Omni-3B `f16` | 2.44 GiB | 1.20 GiB | 1.25 GiB |
| Qwen3-Omni-30B `bf16` | 2.06 GiB | 1.22 GiB | 0.84 GiB |

On the same clip with greedy decoding, stripped and unstripped give the **same
answer word for word** — on both models. (Qwen3-Omni names its projectors per
modality, `clip.audio.projector_type = qwen3a` beside
`clip.vision.projector_type = qwen3vl_merger`, where 2.5 had one combined
`qwen2.5o`. The script needs no change for either.)

**It does not make generation faster, and cannot.** The mmproj runs once at
prefill; every token after that reads the language model's weights. Measured on
the 3B, alternating runs: 27/31 s unstripped against 30/30 s stripped — inside
the run-to-run noise. What it buys is load time and resident memory, which turns
into tok/s only where VRAM was the binding constraint and layers were being left
on the CPU.

**Practical note for any batch use**: a run is about 40 s, of which roughly 38 s
is loading 17 GB from disk — audio encoding is **198 ms** and inference is
seconds. Anything comparing more than a handful of clips wants `llama-server`
with the model resident, not `llama-mtmd-cli` per clip.

There is also a use for Qwen2.5-Omni that does not need it to be fast: an
**offline check on the signal processing**. It runs on one clip occasionally in
the background; a long-run divergence between the two says the feature weights
have drifted. **Measured**: 17 s for a ten-second clip asked for a single word,
of which 5.6 s is audio encoding and most of the rest is model load, which a
resident model removes.

Its cost advantage over the 30B is smaller than the sizes suggest, and the
smaller model is the slower one where it counts. **Measured** on one CPU
machine, same 8.8 s clip, so the two numbers are comparable in a way the figures
above are not: audio encoding takes **4465 ms on Qwen2.5-Omni-3B** and **477 ms
on Qwen3-Omni-30B**. The 3B is cheaper to keep resident, which is the whole
argument for it here; it is not cheaper per clip.

## The engine question, and why it stopped being one

The first version of this problem was posed as a choice between two engines:
**modify llama.cpp to support omni speech output, or move to Alibaba's MNN**.
MNN is a reasonable thing to have asked about — it is Alibaba's own on-device
runtime, so the Qwen family is first-party there in a way it is not upstream,
and the gap it would have closed is real. The section below documents that gap
precisely: there is no Talker checkpoint in GGUF, and llama.cpp's audio
generation is a TTS stack driven by its own backbone rather than by a chat
model's hidden states. On end-to-end omni speech out, llama.cpp cannot do it.

**The choice was never made, because the requirement went away.** Once
recognition and synthesis moved outside the language model — platform TTS for
output, SenseVoice on ggml for input — nothing was left that llama.cpp lacks.
There was no capability to switch engines *for*. That is a better outcome than
picking the right engine, and it is the reason this file spends its length on
where the axes get their numbers rather than on runtimes.

**If it does come back, this is the shape it takes.** Worked out rather than
left to be re-derived:

- **Prosody runs on every recording, on every lane.** It is what goes in the
  store, and the store is longitudinal. A field computed one way on Tuesday and
  another way on Wednesday is worse than one not computed at all, so the
  measurement path cannot depend on which engine answered.
- **SenseVoice runs too**, even with an omni model present. The text axes read a
  transcript, and a transcript from an omni model is generated rather than
  decoded: no frame alignment, where CTC hands over 60 ms timestamps that the C
  ABI already uses to attach words to utterances. And it costs a full audio
  prefill against one encoder pass.
- **So the omni lane's genuine addition is exactly one thing: the voice on the
  way out.** Not a parallel pipeline — an optional output stage.

That makes the engineering small and the product rule obvious: **its absence is
the normal case, so the interface must never promise it.** It is tied to
whichever model the engine supports, and it disappears the day the user moves to
a better thinker.

**What would put it back on the table**, so this is not re-argued from memory:

- Wanting the model's own voice out — a real omni Talker, the speaker's
  characteristics carried through into the reply — rather than the platform
  reading text aloud. That is the original motivation, and it is still
  unavailable upstream.
- Finding that ggml is a poor fit for some encoder worth having. So far the
  opposite: SenseVoice took 400 lines against the ggml this repo already ships.

### MNN was built and measured

**The capability is real, and it works.** MNN carries what llama.cpp does not: a
`Talker` with `setWavformCallback` streaming float PCM, behind a
`predit → dit → bigvgan` chain. `taobao-mnn/Qwen2.5-Omni-3B-MNN` ships all of it
— thinker, audio encoder, talker, DiT, vocoder, speaker dictionary — 3.8 GB.
Built with `MNN_BUILD_LLM_OMNI=ON`, clean.

**Measured**: an 8.8 s clip in, a one-sentence answer about how the speaker
sounds, and **4.0 s of genuine speech out**, in one call. That is the thing this
whole engine question was ever about.

**Install the waveform callback AFTER `load()`, or it produces silence.** One
line, and the whole shape of the call hangs off it:

```cpp
llm->load();                                 // this is what CREATES the Talker
llm->setWavformCallback(sink);               // so this has to come after it
llm->response(prompt, &os, "", 512);         // then one ordinary call
llm->generateWavform();
```

`Omni::setWavformCallback` forwards to the Talker and guards with
`if (mTalker)`. Called first, it is a **silent no-op**: no error, no warning,
and the Talker's own `doGenerate()` is `mWavformCallback != nullptr`, so it then
skips collecting hidden states and skips generating. The result is a clean run
that produces no audio.

The `""` is `end_with`, a terminator MNN appends to the output stream when
generation stops. It is not a token the model emits. Passing `"<eop>"` puts that
text at the end of every reply; passing `NULL` gets a newline, because MNN reads
NULL as "use the default".

**An earlier version of this section prescribed something else**, and it was
wrong:

```cpp
llm->response(prompt, &os, "<eop>", 0);   // prefill ONLY
while (...) { llm->generate(1); }         // step the decode by hand
```

That was reverse-engineered from MNN's Android app while the callback bug was
still in place, on the theory that a completed `response()` marks the session
finished before the Talker can read it. It does not — the Talker collects an
embed on every forward pass of an ordinary call. The hand-stepping only put MNN
into a state its own code never produces, and on the device that crashed: the
Talker's prefill concatenates `mTalkerEmbeds[0]` and `[1]` with no bounds check,
so one embed short is an out-of-bounds read on a vector of one. MNN's own
interleaved path guards the same spot with `hasEmbeds()`.

**So: the thinker must produce at least two tokens before the Talker is asked to
speak**, and the graph must publish `talker_embeds` at all —
`getOutputIndex("talker_embeds")` says so at load, which is much better than
finding out after a recording.

**One nice thing** found on the way: where the GGUF mmproj needed a tensor-level
rewrite to drop the vision tower, MNN reads `is_visual`, `is_audio` and
`has_talker` as plain booleans out of `llm_config.json`. Skipping the 353 MB
vision encoder is one edited word.

#### The real defects, and one that is not

**`use_mmap` breaks the thinker on ARM.** **Measured on the device**: with
`use_mmap: true` the thinker's prefill dies in shape computation, before a token
exists —

```
MNNJNI: Compute Shape Error for /Unsqueeze_output_0
MNNJNI: onForward returned no outputs. seqLen=182, inDecode=0
```

— and the same build, the same model and the same call sequence answer
correctly with the flag off. The desktop cannot see it: MNN's mmap path fails
outright on Windows (`MNNMmapFile failed ... size=1073741824`, repeatedly) and
falls back, so x86 runs the non-mmap path either way.

Worse than the failure is its **stickiness**: MNN sets `INTERNAL_ERROR` and
every entry point checks the status first, so every later call returns in
milliseconds having done nothing. Without a status check of its own the caller
sees an empty answer rather than a broken engine, forever, until the model is
loaded again.

**The control** was a desktop program driving the engine exactly as
`src/voice/omni.cpp` does — an in-memory waveform through `MultimodalPrompt`,
one `response()` call — rather than MNN's own demo. It answered correctly,
**263 prompt tokens to 35 generated**, which is what narrowed the difference
between the two machines down to the overrides this file had added.

**The mechanism is not understood**, and the pairing is odd enough to say so:
shape computation runs before a kernel is chosen and should not care how weights
were loaded. What is established is the correlation, from a controlled
comparison with one variable.

**What the flag was buying, measured**: load took 2.3–3.1 s with it and
7.9–8.3 s without — so roughly five seconds, once per session. Resident memory
was not measured, which is the half that mattered for adding it: with mmap on
the process held 3.5 GB, and nothing says what it holds now.

**Not a defect, and worth having before the next knob is turned: the two config
knobs are measured.** `precision` and `memory`, all five combinations that load,
on the desktop at 209 prompt tokens:

| precision | memory | prefill | decode |
| --- | --- | --- | --- |
| low | low | 80.7 | **19.8 tok/s** ← the model's `config.json` |
| normal | low | 81.7 | 20.2 |
| high | low | 80.8 | 20.6 |
| low | normal | 58.0 | 5.6 |
| high | normal | 56.0 | 5.0 ← `config_high.json` |

**`memory` is the whole cost and `precision` is free.** Low memory keeps the
weights 4-bit and uses the quantized GEMM; normal dequantizes and loses it, for
a 4× decode penalty and nothing in return. The precision column measures nothing
on x86, which reports `fp16:0` — all three compute in fp32 there. On the phone
it would be a real choice, and `precision: high` was tried on the device against
the shape error above and changed nothing.

**Intermittent segfault in text-in/speech-out.** One crash in two runs on the
desktop. The audio is written first and the crash comes during teardown, so the
product is delivered — but a crash is a crash. Not re-tested since the callback
ordering was fixed.

**Audio over ~30 s segfaults.** **Measured**: 8.8 s and 28 s fine, 31 s, 32 s,
37 s and 38 s all segfault, within ten seconds — during the audio front end,
before a token is generated. The *limit* is not the defect: 30 s is the window
every Whisper-family audio encoder has, llama.cpp included, which is why this
file already plans `fsmn-vad` chunking. **Segfaulting instead of saying so is
the defect.**

**Not a defect: the cost.** **Measured**: about 43 s of compute for 14.4 s of
speech — roughly **3× slower than real time** on a desktop CPU.

**That desktop ratio has not been re-derived with a wall clock**, and it is
written as "seconds of compute" — which is what `clock()` returns, summed across
threads. The phone's first figure was inflated five times for exactly that
reason. If this one carries the same defect it reads **high**, the desktop is
faster than 3×, and the phone is not "barely worse" but several times worse.
Every comparison below that leans on 3.0 against 3.7 rests on this.

#### On the phone: 3.7× real time

This is the number the file had been missing, and the one the decision below was
explicitly waiting on. **Measured** on the target device, Qwen2.5-Omni-3B in
MNN, four threads, the model's own `config.json`:

| | |
| --- | --- |
| Load, from app storage | 7.9 s |
| Thinker, 194 prompt tokens → 16 generated | 17.2 s |
| Talker, 10.1 s of speech | 37.5 s — **3.70× real time** |
| Whole exchange, for 4.8 s of recorded audio | 46.2 s |

**The ratio covers the Talker alone**, which is the half that was ever in
question — `generateWavform()`, from codec tokens through DiT to the vocoder.
The thinker is the rest of the exchange and is billed separately above.

A third run, with the thinker broken down and nine blocks of audio instead of
two — MNN's own counters, read off the public `LlmContext`:

| | |
| --- | --- |
| Thinker, 182 prompt tokens → 32 generated | 9.3 s |
| — audio encoder | 1.57 s |
| — prefill, 182 tokens | 5.68 s (**32.0 tok/s**) |
| — decode, 32 tokens | 2.01 s (**15.9 tok/s**) |
| Talker, 10.34 s of speech | 38.5 s — **3.7×** |
| First block of PCM, from the start | 14.7 s |
| Blocks | 9 |
| Whole exchange | 47.7 s |

**Prefill is 61% of the thinker**, which is the structural claim two sections up
arriving as a measurement: audio reaches the model as ~25 tokens per second of
clip, and every one is prefilled. Recognition first is cheaper because it hands
over a tenth as many tokens, not because the model is smaller.

Thinker time varies more than the token counts explain — 194 tokens took 17.2 s
against 182 in 9.3 s. The slow one was the first call after a load, so some of
that gap is cold cache rather than the model.

**The Talker ratio is stable across every run and the block count is not.** Four
exchanges gave 3.5× to 3.7× with no attempt to control for anything, which is
what makes it worth deciding on. Blocks were logged on two of them: 2 and 9, for
replies of 2.26 s and 10.34 s — so MNN's chunking tracks the reply's length
rather than being a fixed cadence, and the short run's two blocks made streaming
look more useless than it is. It is the nine-block run that says what streaming
actually does.

**Against the desktop**, on the same engine and model: `llm_demo` there gets
80.7 tok/s prefill and 19.8 decode, against 32.0 and 15.9 on the phone. Prefill
is 2.5× slower on the device, decode only 1.2×. The split is the expected shape
— prefill is compute-bound and decode is memory-bound — and it says which half
to attack.

**One number here did not fit**, and chasing it found two wrong sentences
elsewhere. The thinker decodes at 15.9 tok/s against a 1.74 GB weight file,
which is **27.6 GB/s** of reads — half again the 19.5 GB/s the section above
calls a wall that a single core already saturates. The number turned out to be
real; both the "single core" claim and the first attempt to explain the number
away did not.

#### The thread sweep, and what it disproves

**Measured** on the device through the `/asr` bench, Qwen3.5-2B `Q4_K_M`
(1.28 GB) in llama.cpp, two runs per setting, the model resident throughout —
`bodyclock_local_set_threads` changes the count without dropping the weights,
so no run pays for a cold page cache:

| threads | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| decode, tok/s | 5.8 | 9.0 | **12.6** | 12.7 | 12.9 |
| implied reads, GB/s | 7.4 | 11.5 | **16.1** | 16.2 | 16.5 |
| prefill, tok/s | 22.7 | 29.0 | **48.4** | 46.3 | 50.1 |

**"A single core already saturates it" is false.** One core reaches 7.4 GB/s,
45% of what the chip gives up at saturation and 38% of the 19.5. It takes four.

**Both phases stop at four**, which is this SoC's performance-core count — one
big, three mid, four little — so the little cores contribute nothing to either.
The project's default of six threads was one cluster too many and is now four,
for the same throughput and two more cores left to the UI.

What survives is narrower than it first appeared. llama.cpp's own ceiling of
15.2 tok/s for a 1.28 GB model holds — 12.9 measured is 85% of it — so within
this engine, threads are not the lever.

**But the plateau is not the bus.** It arrives at the performance-core count,
which is as consistent with running out of useful cores as with filling the
memory controller, and a sweep of one engine cannot separate those. Measured
directly two sections down, the chip reads at 24.4 GB/s and llama.cpp's decode
uses 73–75% of it. The threads stop helping before the memory does.

#### The 27.6 GB/s survives the check, and the wall is llama.cpp's

**The denominator was checked and it is right.** A previous version of this
section explained the 27.6 away as a divisor error — 1.74 GB read as the whole
bundle, the thinker's own weights guessed at 1.05 GB, which lands on 16.7 GB/s
and agrees with the sweep. It does not survive the numbers it asked for:

- **`llm.mnn.weight` is the thinker alone**, 1,737,291,202 bytes (1.618 GiB).
  The bundle is 3.77 GiB across 24 files, and the audio encoder, Talker, DiT and
  vocoder are separate files beside it. The per-file listing was already in the
  device log that produced the 27.6.
- **The graph accounts for every byte of it.** `llm.mnn.json` has 327 ops
  carrying external blobs, and they sum to exactly 1,737,291,202 —
  1655.7 MiB in `Convolution`, which is how MNN stores a Linear, plus 1.2 MiB of
  `LayerNorm`. The model is dense: no routing, no conditional layers, so decode
  reads all of it for every token.

**And the inference undercut itself.** "llama.cpp cannot exceed ~16.5 GB/s on
this chip however it is threaded" treats the sweep's plateau as a property of
the bus — but the sweep explains that plateau two paragraphs earlier by the
performance-core count, *the little cores contribute nothing*. That is a compute
explanation. A curve that flattens at the number of usable cores cannot by
itself tell you the memory bus is full; it is equally consistent with having run
out of cores worth using.

#### Measured without a model: the chip does at least 24.4 GB/s

`bodyclock_memory_bandwidth` reads a 256 MiB buffer with no engine involved —
threads claiming 4 MiB chunks from a shared counter, four accumulators to keep
loads in flight, the buffer written before the clock starts. Swept the same way
as the decode bench, on the same device:

| threads | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| GB/s | 6.5 | 10.6 | 20.9 | 21.9 | **24.4** |

**19.5 GB/s is not a hardware wall.** A few dozen lines of ordinary C beat it by
a quarter, and llama.cpp's decode implies 17.9–18.4 GB/s — 73–75% of what the
chip gives up to a read loop. So the constant this file has been dividing by was
one engine's throughput all along, and "no other compute unit moves that wall"
was only ever tested against engines with llama.cpp's access pattern.

**24.4 is a lower bound, not a peak.** Each fix to the benchmark raised it, which
is the sign of a measurement not yet limited by the thing it is measuring:

| | 1 | 4 | 6 | 8 |
|---|---|---|---|---|
| equal static slices | 6.0 | 18.2 | 16.0 | 21.4 |
| chunks claimed from a counter | 5.5 | 18.2 | 22.7 | 23.0 |
| plus a 2 MiB-aligned buffer | 6.5 | 20.9 | 21.9 | **24.4** |

The first row contains its own refutation: **six threads slower than four**, five
runs each. Equal slices on one big, three mid and four little cores means the
timer waits for a little core carrying a sixth of the buffer while the fast ones
idle. That is not the machine's bandwidth, it is the bandwidth of an unlucky
split — and it inverted the very ordering the table exists to show.

**Huge pages were tried and are not available.** 256 MiB of 4 KiB pages is 65,536
translations against a TLB holding a few thousand, so a page walk every 512 loads
was a plausible reason the loop might understate. `madvise(MADV_HUGEPAGE)` was
refused by this kernel. It is worth recording as closed rather than untried,
because it also means MNN does not get huge pages for its weights either: page
size cannot explain any gap between the two.

**What is left unexplained is 13%**, and it is left there. MNN's thinker implies
27.6 GB/s against a measured 24.4, which is inside the range that "the read loop
uses scalar 8-byte loads and has not been tuned" covers — every previous fix
moved it further than that. Chasing it further would not change anything below:
the wall is higher than 19.5 either way, and by enough that the arithmetic in
this file should be re-derived rather than patched.

**MNN's side was cross-checked and holds.** The 15.9 tok/s comes from MNN's own
counter, so it was verified against wall-clock differences between runs instead:
184 prompt tokens to 8 generated took 7.5 s, and to 25 generated took 8.6 s —
17 tokens in 1.1 s, **15.5 tok/s**, arrived at without touching MNN's timers.

**The desktop extrapolation was right in direction**, and how far is not
settled. "On a phone it can only be worse" is how the paragraph above ends, and
3.0× became 3.7× — but the 3.0 may be a `clock()` figure, in which case the gap
is wider than it looks. What the thinker measurements do say cleanly, both taken
with a wall clock: prefill is 2.5× slower on the device and decode only 1.2×.

If the phone really is close to a desktop CPU, the reason is that it is not
running the same arithmetic — the device reports `fp16:1` and `i8mm:1`, ARM
kernels x86 has no equivalent of, and the model's `config.json` asks for the
precision that uses them.

Either way the decision below is unaffected, because it never turned on the
desktop comparison. **Ten seconds of spoken reply costs thirty-seven seconds of
the phone's whole attention**, measured on the phone, and the blocks say the
person waits through nearly all of it — see the streaming paragraph in the
decision.

**A caution about how this number is obtained.** The first phone run reported
**17.0×**, and that figure was wrong by a factor of five. The timings used
`clock()`, which on POSIX is processor time summed across threads; the engine
runs four. The error was caught by comparing the printed durations against the
log's own timestamps — 87.9 s printed, 18.2 s elapsed. Anything multithreaded
measured with `clock()` reports several seconds per second, and a ratio built on
it decides the wrong way. The same mistake was in the recognition timings on the
`/asr` path.

#### What this correction replaced, and why it is kept

An earlier version of this section listed five defects and concluded "not
adopted". **Four of the five were not defects.** They were one API misuse — a
callback installed one line too early — plus a config file this project had
broken itself, plus a standard window limit read as a capability gap. The
conclusion built on them had no support.

**The correction was itself half wrong, and that is part of the lesson.** It
named the misuse as driving generation the wrong way, and prescribed the
hand-stepped decode above. Reading the shipping app did find *a* working call
shape; it did not find *why* ours was silent, and the difference went unnoticed
because the recipe appeared to work. The real cause surfaced only later, from
MNN's source: `setWavformCallback` guards on a Talker that `load()` has not
created yet.

The mistake is worth more than the measurements. Faced with output that did not
work, the response was to turn configuration knobs — precision, the `mllm`
block, sync against async — four rounds of it, and then to promote "I could not
make it work" into "the engine is defective", in a written decision.

**The first question was never asked: has anyone already got this working?**
They had. MNN ships an Android app that does audio in and speech out, and ten
lines of its session code hold the answer. Turning knobs feels like work in a
way that reading a working caller does not.

This is the same shape this file records three times elsewhere — a measurement
that cannot distinguish two explanations, read as though it had. "Silence" was
equally consistent with a broken engine and with a wrong call, no experiment
separated them, and the reading chosen was the one that allowed the
investigation to stop.

### Whether to adopt it

**The case for, and the half this file never priced**: it is the only way to get
the model's own voice out, and that works today.

**Its voice is reported to sound more natural than the platform's.** That is an
observation from listening to both, not a measurement, and it is recorded
because everything else here prices only the cost side. A decision that weighs
3.7× against "one capability" has quietly assumed the capability is
interchangeable with the free one — and if it is not, the trade is a product
judgement rather than arithmetic.

**The obvious objection was checked: the platform is not simply misconfigured.**
An app cannot ask for a nicer voice than the one it is getting.

- `@hms.ai.textToSpeech` lists **three** voices on this handset, and only person
  13 (聆小珊) is INSTALLED. Every one of them has style
  `interaction-broadcast`, and **style cannot be requested** — it appears in
  those typings only as a field on `VoiceInfo`, never in `VoiceQuery` or
  `CreateEngineParams`. `person` is the whole of the choice.
- **The online engine is not a door.** `online: 0` is refused with
  `401 online type error`. An earlier note here said offline was the only engine
  HarmonyOS offers and was demoted to "an assumption"; it was right, and now it
  is checked.
- `@hms.ai.textReader` is a second surface with a richer model on paper —
  `Person { tone, style }`, style as an *input*, an online mode — and driven
  with its defaults it speaks **the same voice**. The panel says 小艺朗读 and
  what comes out is the broadcast timbre.

So the platform's ceiling for a third-party app is that timbre. The voices in
the assistant's own settings menu belong to the assistant and are not reachable
from either API. **The comparison against omni is therefore a fair one**, which
is the opposite of what checking it was expected to show.

One thread is unpulled: `textReader`'s `person` was never passed, and
`interaction-broadcast` is obviously one name in a scheme. The typings enumerate
nothing, so that is guessing string constants, and a wrong guess speaks in the
default and looks like a success.

**The obvious middle path is closed for this model.** Driving the Talker from
text — the cheap chain writes, omni's voice speaks — would skip the thinker's
9 s entirely. MNN has the entry point, `generateTTS`, but it refuses anything
whose `talker_type` is not `qwen3_tts`, and Qwen2.5-Omni's Talker consumes the
thinker's hidden states rather than text. It is not a wiring problem.

**A standalone TTS in the same family would be the route**, and it changes the
size argument though not the speed one. The parts that make sound are talker
391 MB, DiT 348 MB, vocoder 116 MB, predit 17 MB and the talker embeddings
34 MB — about **0.9 GB**, against 3.8 for the bundle. The 3B thinker is the
freight, and it is exactly the part a text-driven TTS does not need. But the
3.7× was measured on `generateWavform()` alone, so removing the thinker does not
move it.

**What the ratio actually rules out is conversation, not playback.** Everything
above assumes somebody is waiting to be answered. An app reading back something
it prepared — a day's recap — spends 3.7× in the background and nothing in
front of the person. That is a different product, and this file has never
costed it.

Recognition through it is also plausibly better than SenseVoice's
— Qwen2.5-Omni-3B fixed four transcription errors including a polarity flip, in
the table above — though that was measured through llama.cpp and has not been
reproduced in MNN's own quantization, because the reference clip is longer than
the 30 s window.

**The case against, and it is about arithmetic, not defects.** Recognition is
already served by SenseVoice, which is cheap and uniquely gives the 60 ms
alignment the C ABI already uses. Text is already served by llama.cpp,
integrated, carrying the GBNF grammar the text axes decode against. Prosody is
signal processing and needs no engine at all. So a second engine buys exactly
one thing — the voice on the way out — and charges a second model format, a
second quantization story, a second tuning surface, and 3.8 GB.

**The 3.8 GB charges something the size figure hides**: it has to reach the
phone. Twenty-four files whose layout MNN resolves relative to `config.json`,
which nobody is going to copy by hand, so this repo now carries a bundle format
and an extractor to make it one file. That is real code, and it exists only
because of this engine.

**A second engine also means a second set of headers in one binary.** MNN
vendors rapidjson 1.1.0; this repo already vendored a much newer one. Both are
header-only templates emitting the same weak symbols into one `.so`, the linker
keeps one definition, and the newer `MemoryPoolAllocator` holds a `SharedData*`
where 1.1.0 holds `chunkHead_`. The result was a segfault at 0x10 on MNN's first
JSON parse. Not a defect in either library — a cost of carrying two.

**And the one thing it buys costs 3.7× real time on the phone.** Measured, in
the table above: ten seconds of spoken reply for thirty-seven seconds of the
device's whole attention. That is the number to argue with, and it is not an
implementation detail that improves with familiarity.

**So: not yet — and now on a measurement rather than an extrapolation.** The
engine works on the device, end to end, audio in and the model's own voice out.
What the phone figure says is that a spoken reply cannot be waited for. Below
about 1× it would be a feature; at 3.7× it is a demo.

**What would change the answer**: Talker throughput near real time, and nothing
else. The capability is proven and the remaining crashes are ours or fixable, so
the ratio is the whole question — and the obvious way around it does not work.

**Streaming is not the rescue, and that is arithmetic rather than an
implementation detail.** The Talker hands PCM over in blocks, so a reply could
in principle start playing before it is finished. **Measured**, on the run with
nine blocks: 10.34 s of speech, 38.5 s of Talker, first block at **14.7 s** from
the start of the exchange against 47.7 s for the whole exchange.

Bringing the first word forward by thirty-three seconds sounds like the answer,
and it is not, because of what arrives after it. Nine blocks over 38.5 s is a
block every **4.3 s** carrying **1.15 s** of audio. Playback consumes a second
per second, so it drains each block in 1.15 s and then waits 3.2 s for the next:
**27% speech, 73% silence**. That is not a reply being spoken, it is a reply
being spelled out.

The duty cycle *is* the ratio — 1 ÷ 3.7 — and that is the general form. **Above
1× the listener always outruns the generator**, so streaming can only ever move
the first word earlier; it can never close the gaps. Below 1× it would close
them for free.

So chunking, `n_window`, and MNN's interleaved mode are all the same idea and
all bounded by the same number. They are worth building **after** the ratio goes
below 1, not instead of getting it there. What is left that could move it: the
NPU backend, a smaller Talker, or accepting that the platform reads text aloud.

**What was NOT established**, so it is not quietly assumed later: whether MNN's
quantization transcribes as well as the GGUF build does. The reference clip is
the one that crashes. The finding that Qwen2.5-Omni-3B beats SenseVoice on four
errors including a polarity flip stands — but it was measured through
llama.cpp, and it does not transfer to another runtime for free.

## What llama.cpp can do now

An earlier note here said llama.cpp implements only the Thinker and audio output
is an open question. **Out of date.** `tools/mtmd/` carries a three-stage
generation pipeline — backbone to hidden state, hidden state to audio codes,
codes to PCM — exposed as `mtmd_gen_audio_process()`, with
`MTMD_GEN_AUDIO_TYPE_QWEN3TTS` and `MTMD_GEN_AUDIO_TYPE_POCKETTTS` registered.

**It is still not the omni Talker.** For Qwen3-TTS the stage-1 backbone is
`talker.model`, the TTS model's own — hidden states flow inside the TTS stack,
not from a chat model that just understood something. Excellent text-to-speech,
not end-to-end speech. There is also **no Talker checkpoint in GGUF**: published
conversions are thinker plus mmproj only, so implementing it would mean writing
the conversion as well as the inference.

Audio **input** for Qwen2.5-Omni is fully supported (`PROJECTOR_TYPE_QWEN25O`),
and the registered audio encoders now go well beyond it — Qwen2A, Qwen3A,
Parakeet, Granite Speech, Whisper, Conformer.

## The port exists; multimodal is not in it

This repo already cross-compiles llama.cpp for HarmonyOS —
[harmony/build.cmd](../harmony/build.cmd) `llama`, assembling
`harmony/prebuilt/llama-sdk/<abi>/`. It defaults to
`-march=armv8.6-a+i8mm+bf16+dotprod+fp16`, which is not a guess: a cross build
with no `-march` lands on baseline `armv8-a`, because CMake's feature probes
compile **and run** a test binary and cannot run one for the phone. That costs
2.9× prefill and 2.2× decode, measured.

**Audio is not in that build.** It sets `-DLLAMA_BUILD_TOOLS=OFF`, and `libmtmd`
lives under `tools/mtmd/`. The assembled SDK has `libllama.a` and the three ggml
libraries and nothing else.

**The decision at the top means none of that has to change.** Recognition links
ggml directly; the signal processing needs nothing at all.

## What this must not become

**Never show a person a flat claim about their own emotional state.** The omni
result is why. The model did not say it could not tell — it produced a
confident, specific, plausible reading, and in an app about health that failure
is invisible: it looks right every time.

Everything here surfaces as **deviation from a baseline**, never as a label.
"You spoke thirty percent slower than earlier in this recording" is something
the speaker can check. "You sound tired" is something they can only accept or
resent. The same rule is why the falsifiable dimensions — hoarseness, effort,
fluency — are worth more here than the emotional ones, even though they sound
less impressive.

Use the signal where being wrong is survivable: within one recording, or
aggregated across many, or as an input to how the model phrases its own replies.
Never as a displayed fact.
