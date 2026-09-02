// Implementation of the public C ABI (src/yoai.h).
//
// Thin by design: the registry is a map, and a turn is one OpenAIChatClient
// built from a copy of the matching entry. No session state, no config file, no
// background thread -- everything the core knows, the host told it.

#include "yoai.h"

#include "client/http_client.hpp"
#include "llm/openai_chat.hpp"
#include "llm/types.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

using yoai::llm::ChatMessage;
using yoai::llm::Event;
using yoai::llm::EventType;
using yoai::llm::OpenAIChatOptions;

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

// Guarded by registry_mutex(). Ordered rather than hashed so yoai_providers()
// lists ids in a stable order -- a menu that reshuffles itself when a key is
// added would be a UI bug sourced here.
std::map<std::string, OpenAIChatOptions>& registry() {
    static std::map<std::string, OpenAIChatOptions> r;
    return r;
}

// curl's global state must be initialized once before any easy handle is used.
// Left to lazy init it is not thread-safe, and every entry point below can be
// the first one a host calls.
void ensure_curl() {
    static std::once_flag once;
    std::call_once(once, []() { yoai::client::HttpClient::global_init(); });
}

// Look up `id` and hand back a COPY of its options, so the turn that follows is
// unaffected by a concurrent registry edit. Returns false when unknown.
bool lookup(const std::string& id, OpenAIChatOptions* out) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = registry().find(id);
    if (it == registry().end()) return false;
    *out = it->second;
    return true;
}

//------------------------------------------------------------------------------
// Event mapping
//------------------------------------------------------------------------------

// The C ABI's tag for one internal event type. Distinct from
// llm::to_string(EventType) on purpose: that spelling serves the internal
// logs, while these are the strings a host app switches on, and they must not
// change when the internal enum is renamed.
const char* tag_for(EventType t) {
    switch (t) {
        case EventType::Reply:          return "reply";
        case EventType::Thinking:       return "thinking";
        case EventType::QueryTitle:     return "queryTitle";
        case EventType::QueryArguments: return "queryArguments";
        case EventType::QueryDetail:    return "queryDetail";
        case EventType::CostStatistics: return "usage";
        case EventType::Error:          return "error";
    }
    return "error";
}

std::string usage_json(const yoai::llm::CostStatistics& c) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("model");
    w.String(c.model.data(), static_cast<rapidjson::SizeType>(c.model.size()));
    w.Key("inputTokens");   w.Int64(c.input_tokens);
    w.Key("outputTokens");  w.Int64(c.output_tokens);
    w.Key("thoughtTokens"); w.Int64(c.thought_tokens);
    w.Key("totalTokens");   w.Int64(c.total_tokens);
    w.Key("totalCost");     w.Double(c.total_cost);
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

//------------------------------------------------------------------------------

// Parse the host's transcript. An entry missing role or content is SKIPPED
// rather than failing the turn: one malformed row out of fifty should cost the
// user that row, not their message. Returns false only when the argument is not
// a JSON array at all.
bool parse_messages(const char* json, std::vector<ChatMessage>* out) {
    rapidjson::Document doc;
    doc.Parse(json);
    if (doc.HasParseError() || !doc.IsArray()) return false;

    for (const auto& m : doc.GetArray()) {
        if (!m.IsObject()) continue;
        if (!m.HasMember("role") || !m["role"].IsString()) continue;
        if (!m.HasMember("content") || !m["content"].IsString()) continue;
        ChatMessage msg;
        msg.role.assign(m["role"].GetString(), m["role"].GetStringLength());
        msg.content.assign(m["content"].GetString(), m["content"].GetStringLength());
        out->push_back(std::move(msg));
    }
    return true;
}

// Backing store for yoai_providers()' return value. Thread-local so two threads
// listing at once do not overwrite each other's buffer.
std::string& providers_buffer() {
    static thread_local std::string buf;
    return buf;
}

bool blank(const char* s) { return s == nullptr || *s == '\0'; }

}  // namespace

//------------------------------------------------------------------------------
// Diagnostics
//------------------------------------------------------------------------------

extern "C" const char* yoai_version(void) {
    return "0.1.0";
}

extern "C" void yoai_set_log_level(const char* level) {
    if (blank(level)) return;
    yoai::platform::set_log_level(level);
}

//------------------------------------------------------------------------------
// Provider registry
//------------------------------------------------------------------------------

extern "C" int yoai_provider_set(const char* id,
                                 const char* base_url,
                                 const char* api_key,
                                 const char* model,
                                 const char* extra_json) {
    if (blank(id) || blank(base_url) || blank(api_key) || blank(model)) {
        yoai::platform::log_warn("yoai_provider_set: id, base_url, api_key and model are all required");
        return 1;
    }

    OpenAIChatOptions opt;
    opt.base_url = base_url;
    opt.api_key  = api_key;
    opt.model    = model;
    if (!blank(extra_json)) opt.extra_body_json = extra_json;

    // Trailing slash on the base URL would produce "…//chat/completions", which
    // some gateways 404 on. Cheap to fix here, once, rather than in every host.
    while (!opt.base_url.empty() && opt.base_url.back() == '/') opt.base_url.pop_back();

    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[id] = std::move(opt);
    }
    // The key is deliberately absent from this line.
    yoai::platform::log_info("provider registered: %s -> %s (%s)", id, base_url, model);
    return 0;
}

extern "C" int yoai_provider_remove(const char* id) {
    if (blank(id)) return 0;
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry().erase(id) > 0 ? 1 : 0;
}

extern "C" void yoai_provider_clear(void) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().clear();
}

extern "C" const char* yoai_providers(void) {
    std::string& buf = providers_buffer();
    buf.clear();
    std::lock_guard<std::mutex> lock(registry_mutex());
    for (auto it = registry().begin(); it != registry().end(); ++it) {
        if (!buf.empty()) buf += '\n';
        buf += it->first;
    }
    return buf.c_str();
}

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

extern "C" int yoai_chat(const char* provider_id,
                         const char* messages_json,
                         const char* system_prompt,
                         yoai_chat_handler on_event,
                         void* user_data) {
    if (on_event == nullptr) return 1;

    // Every failure from here on is reported through the handler as well as the
    // return code: the host has a place to show text and no way to turn an int
    // into one.
    auto fail = [&](const std::string& msg) {
        yoai::platform::log_warn("yoai_chat: %s", msg.c_str());
        on_event("error", msg.c_str(), user_data);
        return 1;
    };

    if (blank(provider_id))   return fail("no provider given");
    if (blank(messages_json)) return fail("no messages given");

    OpenAIChatOptions opt;
    if (!lookup(provider_id, &opt)) {
        return fail(std::string("unknown provider: ") + provider_id);
    }

    std::vector<ChatMessage> messages;
    if (!parse_messages(messages_json, &messages)) {
        return fail("messages must be a JSON array");
    }
    if (messages.empty()) return fail("no usable messages");

    ensure_curl();

    yoai::llm::OpenAIChatClient client(opt);
    const bool ok = client.ainvoke(
        messages,
        blank(system_prompt) ? std::string() : std::string(system_prompt),
        [&](const Event& e) -> bool {
            const std::string payload = (e.type == EventType::CostStatistics)
                                      ? usage_json(e.cost)
                                      : e.content;
            return on_event(tag_for(e.type), payload.c_str(), user_data) != 0;
        });

    return ok ? 0 : 1;
}
