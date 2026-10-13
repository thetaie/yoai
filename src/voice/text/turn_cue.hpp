// What a piece of the user's speech, cut where they paused, does to a voice
// conversation: the two judgments of docs/slides.md, "Barge-in: a second
// sentence mid-reply", made on the words, by rule.
//
// Judgment 1, with nothing being answered: can this be answered on its own, or
// has the user not yet said what they want? Judgment 2, with a reply under
// way: does this add to the request, or take it back?
//
// **Rules, because the chat model alone was not enough.** The plan was to ask
// the chat model in the turn itself (kLiveTurnText in llm/prompt/prompts.cpp),
// and that stays as a second chance. Measured on eight messages with nothing
// yet to answer, Qwen3.5-2B answered <wait> to one -- the example the prompt
// gives -- and the 4B to three. A reworded prompt took the 2B to none, and had
// the 4B answer <wait> to "帮我订去北京的票". With the first wording neither ever
// waited on a message that could be answered, so that wording is kept, and
// these rules catch what it misses.
//
// Chinese first, with a little English, like the rest of this directory. The
// rules see the words only: "明天" alone, a topic with no question yet, is not
// caught, and is left to the model. Nor is a correction that does not say it is
// one: "杭州" said over a reply about Shanghai reads as an addition, which is
// the cheaper mistake -- a few seconds of the wrong city, which "停" ends.

#pragma once

#include <string>

namespace yoai {
namespace voice {

enum class TurnCue {
    Answer,      // something to answer: a new request, or one added to the last
    Unfinished,  // nothing to answer yet, as in "我想问一下"; the rest is coming
    Correct,     // takes back the request being answered and says what instead,
                 // as in "不对，是杭州"
    Stop,        // takes it back and says nothing else, as in "算了" or "停"
};

// One utterance's cue. A host asks only the judgment that applies: Unfinished
// when nothing is being answered, Correct or Stop when a reply is under way.
// "不对" with nothing being answered is something to answer, like any other.
TurnCue turn_cue(const std::string& text);

}  // namespace voice
}  // namespace yoai
