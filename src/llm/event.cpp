#include "llm/event.hpp"

namespace yoai { namespace llm {

const char* to_string(EventType t) {
    switch (t) {
        case EventType::Reply:          return "reply";
        case EventType::Thinking:       return "thinking";
        case EventType::QueryTitle:     return "queryTitle";
        case EventType::QueryArguments: return "queryArguments";
        case EventType::QueryDetail:    return "queryDetail";
        case EventType::CostStatistics: return "costStatistics";
        case EventType::Error:          return "error";
    }
    return "";
}

}
}
