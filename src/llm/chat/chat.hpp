#pragma once

// A model that answers a conversation, streamed.
//
// Implemented by LocalClient (llama.cpp, llm/chat/local.hpp) and OpenAIChatClient
// (any OpenAI-compatible endpoint, llm/chat/openai.hpp). yoai.cpp
// keeps one per registered id behind this type and runs every turn the same
// way, whichever of the two it is.

#include "llm/backend.hpp"
#include "llm/chat/types.hpp"   // ChatMessage, EventHandler, UserContext, RequestOptions

#include <string>
#include <vector>

namespace yoai { namespace llm {

class Chat : public Backend {
public:
    // Stream a reply to `messages`, firing `on_event` on the calling thread.
    // Blocks for the whole turn.
    //
    // A turn that completes ends with exactly one CostStatistics event. A turn
    // that fails sends one Error event instead, and returns false. Returning
    // false from `on_event` cancels the turn: ainvoke then returns false, and
    // that is not an error -- it is how a host stops a reply.
    //
    // Turns are stateless: the whole transcript comes in every time. `user` is
    // for a client that runs tools and forwards the caller's identity to them;
    // `req` carries per-turn options such as a grammar.
    virtual bool ainvoke(const std::vector<ChatMessage>& messages,
                         const std::string&              system_prompt,
                         const EventHandler&             on_event,
                         const UserContext&              user = UserContext(),
                         const RequestOptions&           req  = RequestOptions()) = 0;

    // Start a new conversation: drop anything kept from the last one. A remote
    // model keeps nothing between turns, so there it does nothing. An on-device
    // one keeps its KV cache, and a new conversation must call this -- see
    // LocalClient::forget for why sending a shorter transcript is not enough.
    virtual void forget() = 0;

protected:
    explicit Chat(Engine engine) : Backend(engine) {}
};

}
}
