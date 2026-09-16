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

**Status: it compiles, and it has not run on a device.** The ArkTS builds clean
and the recognition lane is in the built `libbodyclock.so` for `arm64-v8a`. The
HAP is not signed on this machine: the debug signing profile predates
`8dcabe5 rename: YoAI becomes Body Clock` and was issued for the old bundle
name, so DevEco has to reissue it for `ai.bodyclock.app` before anything can be
installed. Every number this page is meant to settle — microphone gain, room
noise, load time, encoder speed on a Kirin — is still unmeasured.

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
read bandwidth of **19.5 GB/s that a single core already saturates**.

That figure is worth being able to re-derive: decode reads every weight for
every token, so a model of size S cannot exceed bandwidth/S tokens per second —
15.2 for a 1.28 GB model, against 11.2 observed. No other compute unit moves
that wall; measured the same way, the Vulkan GPU came out **3.5× slower** than
CPU and the NPU's ceiling sat below it. Prefill is the exception, being
compute-bound.

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
that SenseVoice handled in 1.5 s. Twenty-four times.

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
