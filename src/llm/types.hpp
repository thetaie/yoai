#pragma once

// The vocabulary every LLM client in this directory shares: what a turn is made
// of going in (ChatMessage), who is asking (UserContext), and how partial
// results come back out (EventHandler over llm::Event).
//
// Kept in its own header, separate from any one client, because Client (the
// abstract interface) and each concrete client all need these types and none of
// them should have to include another client to get them.

#include "llm/event.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace yoai { namespace llm {

struct ChatMessage {
    std::string role;       // "user" / "assistant" / "system" / "tool"
    std::string content;
};

// Returning false from the handler aborts the in-flight HTTP transfer, which is
// how a cancelled UI stops a stream that would otherwise run for minutes.
using EventHandler = std::function<bool(const Event&)>;

// The caller's identity for a turn, threaded through ainvoke so a client that
// runs tools locally can pass it to its tool executor. A non-positive user_id
// means the device owner / unauthenticated -- which on a personal client is the
// normal case, not an error.
struct UserContext {
    std::int64_t user_id = 0;
    std::string  session_id;
};

}
}
