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

namespace bodyclock { namespace llm {

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

// Per-TURN options, as opposed to the per-client ones each concrete client
// carries in its own Options struct.
//
// The distinction is load-bearing: the on-device client is cached per provider
// and reused for every turn against that model (platform/c_api.cpp), so a
// setting that belongs to one request cannot live on the client without leaking
// into the next conversation.
struct RequestOptions {
    // GBNF grammar constraining the reply. Empty means unconstrained, which is
    // every ordinary chat turn.
    //
    // This is a grammar, NOT a JSON schema: converting one to the other lives in
    // llama.cpp's `common` library, which this project does not link, and the
    // grammars that constrained output actually needs here are short enough to
    // write out. A caller wanting JSON writes the JSON's grammar.
    //
    // Honored by the on-device client. The remote client ignores it and says so
    // in the log rather than silently returning free text -- an unconstrained
    // reply that the caller will try to parse is a failure worth hearing about.
    //
    // A NON-EMPTY GRAMMAR ALSO MAKES THE TURN DETERMINISTIC: greedy decoding,
    // no repetition penalty. This is not a convenience, it is the fix for a bug
    // that cost an evening.
    //
    // A grammar means the reply is a value, not prose, and a value must not
    // change between runs on the same input. Measured: a 2B asked to score one
    // cheerful sentence, constrained to five levels, at the conversational
    // defaults (temp 0.7, top_k 20, top_p 0.8), answered -1 four times out of
    // six and 0 twice -- never the same number twice running, never correct.
    // Greedy, it answered 0 every time. The sampling was the whole difference,
    // and the failure looked exactly like a model that could not read.
    //
    // Constraining the SHAPE of deliberately varied output is the case this
    // rules out. Nothing in this project wants that; when something does, this
    // is where the opt-out goes.
    std::string grammar;
};

}
}
