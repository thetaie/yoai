#pragma once

#include <cstdint>
#include <string>

namespace yoai { namespace llm {

// Mirrors the dict shapes yielded by the Python reference clients
// (pub/agents/base/clients.py): {"type": ..., "content": ..., "tool_id": ...}.
enum class EventType {
    Reply,           // user-facing answer text (coalesced to min_chunk_size)
    Thinking,        // reasoning narrative
    QueryTitle,      // tool name — start of a tool step
    QueryArguments,  // tool arguments (JSON-encoded string)
    QueryDetail,     // tool result (string; JSON-encoded when structured)
    CostStatistics,  // terminal usage summary
    Error,           // server- or transport-level error message
};

const char* to_string(EventType t);

//------------------------------------------------------------------------------
// Event payload
//------------------------------------------------------------------------------

struct CostStatistics {
    std::string model;
    std::int64_t input_tokens  = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t total_tokens  = 0;
    double total_cost = 0.0;
};

struct Event {
    EventType type;

    // Text payload. For CostStatistics this is left empty — read `cost`.
    std::string content;

    // Populated only for tool-related events (QueryTitle/Args/Detail).
    std::string tool_id;

    // Populated only when type == CostStatistics.
    CostStatistics cost;
};

}
}
