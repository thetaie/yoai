// A message's annotations turned into the text a model reads.
//
// A host attaches Annotations to its messages as data -- the time on every one,
// arousal when prosody had something to say -- and keeps them that way for its
// own screens. Every chat client calls with_annotation_text() on the transcript
// before it renders the prompt, so the model sees them as one bracketed line in
// front of a user message:
//
//     [Time: Saturday, October 10, 2026, 2:32 PM UTC+8 Asia/Shanghai; Tone: a little more animated than usual (measured from the voice)]
//     今天又加班到十点……
//
// The wording of each part comes from the prompt table (context_time,
// context_arousal), so it is listed and replaceable like any other prompt.
//
// **The time is not written on every message.** It goes on a user message when
// it is the first one with a time, when the local date has changed since the
// last time written, when the offset or zone has changed, or when 30 minutes or
// more have passed. In between, the model reads "just now" and "tonight" off the
// last one. So a whole conversation usually carries one or two.
//
// **The same transcript always gives the same text.** That is what keeps a local
// model's KV cache, and a remote provider's prefix cache, valid from one turn to
// the next: nothing here reads the clock or keeps state between calls -- the
// decision for each message follows from the messages before it in the list it
// is given.

#pragma once

#include "llm/chat/types.hpp"

#include <vector>

namespace yoai {
namespace llm {

// `messages` with each user message's annotations written into the front of its
// content, as above. Assistant and system messages pass through unchanged, and
// so does every message's `annotations` member.
std::vector<ChatMessage> with_annotation_text(const std::vector<ChatMessage>& messages);

}  // namespace llm
}  // namespace yoai
