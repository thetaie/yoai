#include "llm/chat/openai.hpp"

#include "llm/chat/annotations.hpp"
#include "llm/chat/sse_parser.hpp"
#include "utils/http.hpp"
#include "utils/log.hpp"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <map>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace yoai { namespace llm {

namespace {

//------------------------------------------------------------------------------
// Stream state
//------------------------------------------------------------------------------

// Streaming tool calls arrive in fragments indexed by `tool_calls[i].index`.
// We coalesce id/name/arguments per index, then flush once when the stream
// completes — matching how the OpenAI SDK rebuilds them on the client side.
struct PendingToolCall {
    std::string id;
    std::string name;
    std::string arguments;
    bool title_emitted = false;
};

struct StreamContext {
    SseParser parser;
    std::string text_buffer;
    std::string thinking_buffer;    // reasoning_content deltas, coalesced like text
    std::string full_text;          // the turn's complete assistant text (for the tool loop)
    std::size_t min_chunk_size = 30;

    std::int64_t input_tokens   = 0;
    std::int64_t output_tokens  = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t total_tokens   = 0;

    std::map<int, PendingToolCall> tool_calls;   // by index

    bool aborted   = false;
    bool done_seen = false;

    const EventHandler* on_event = nullptr;
};

bool dispatch(StreamContext& ctx, const Event& e) {
    if (ctx.aborted) return false;
    if (!(*ctx.on_event)(e)) {
        ctx.aborted = true;
        return false;
    }
    return true;
}

bool flush_text(StreamContext& ctx) {
    if (ctx.text_buffer.empty()) return true;
    Event e;
    e.type    = EventType::Reply;
    e.content = std::move(ctx.text_buffer);
    ctx.text_buffer.clear();
    return dispatch(ctx, e);
}

bool flush_thinking(StreamContext& ctx) {
    if (ctx.thinking_buffer.empty()) return true;
    Event e;
    e.type    = EventType::Thinking;
    e.content = std::move(ctx.thinking_buffer);
    ctx.thinking_buffer.clear();
    return dispatch(ctx, e);
}

//------------------------------------------------------------------------------
// JSON helpers
//------------------------------------------------------------------------------

std::string get_string(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return {};
    const auto& m = v[key];
    if (m.IsString()) return std::string{m.GetString(), m.GetStringLength()};
    return {};
}

std::int64_t get_int64(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    const auto& m = v[key];
    if (m.IsInt64())  return m.GetInt64();
    if (m.IsInt())    return m.GetInt();
    if (m.IsUint64()) return static_cast<std::int64_t>(m.GetUint64());
    if (m.IsUint())   return static_cast<std::int64_t>(m.GetUint());
    return 0;
}

//------------------------------------------------------------------------------
// Tool-call assembly & SSE dispatch
//------------------------------------------------------------------------------

void process_tool_call_fragments(StreamContext& ctx,
                                 const rapidjson::Value& tool_calls_arr) {
    if (!tool_calls_arr.IsArray()) return;
    for (const auto& tc : tool_calls_arr.GetArray()) {
        if (!tc.IsObject()) continue;
        int idx = 0;
        if (tc.HasMember("index")) {
            if (tc["index"].IsInt())     idx = tc["index"].GetInt();
            else if (tc["index"].IsInt64()) idx = static_cast<int>(tc["index"].GetInt64());
        }

        auto& slot = ctx.tool_calls[idx];

        if (slot.id.empty()) {
            slot.id = get_string(tc, "id");
        }
        if (tc.HasMember("function") && tc["function"].IsObject()) {
            const auto& fn = tc["function"];
            if (slot.name.empty()) slot.name = get_string(fn, "name");
            if (fn.HasMember("arguments") && fn["arguments"].IsString()) {
                slot.arguments.append(fn["arguments"].GetString(),
                                      fn["arguments"].GetStringLength());
            }
        }

        // Emit a QueryTitle as soon as we know the name + id; arguments are
        // flushed once at the end so the consumer sees a complete JSON blob.
        if (!slot.title_emitted && !slot.name.empty() && !slot.id.empty()) {
            Event e;
            e.type    = EventType::QueryTitle;
            e.content = slot.name;
            e.tool_id = slot.id;
            slot.title_emitted = true;
            if (!dispatch(ctx, e)) return;
        }
    }
}

void flush_tool_calls(StreamContext& ctx) {
    for (auto it = ctx.tool_calls.begin(); it != ctx.tool_calls.end(); ++it) {
        auto& slot = it->second;
        if (!slot.title_emitted && !slot.name.empty()) {
            Event title;
            title.type    = EventType::QueryTitle;
            title.content = slot.name;
            title.tool_id = slot.id;
            slot.title_emitted = true;
            if (!dispatch(ctx, title)) return;
        }
        if (!slot.arguments.empty()) {
            Event args;
            args.type    = EventType::QueryArguments;
            args.content = slot.arguments;
            args.tool_id = slot.id;
            if (!dispatch(ctx, args)) return;
        }
    }
}

void handle_data_event(StreamContext& ctx, const char* payload) {
    if (std::strcmp(payload, "[DONE]") == 0) {
        ctx.done_seen = true;
        return;
    }

    rapidjson::Document doc;
    doc.Parse(payload);
    if (doc.HasParseError() || !doc.IsObject()) {
        yoai::utils::log_warn("openai-chat: bad SSE JSON: %.200s", payload);
        return;
    }

    // Usage normally arrives on a final chunk that has empty `choices`.
    if (doc.HasMember("usage") && doc["usage"].IsObject()) {
        const auto& u = doc["usage"];
        ctx.input_tokens  += get_int64(u, "prompt_tokens");
        ctx.output_tokens += get_int64(u, "completion_tokens");
        ctx.total_tokens  += get_int64(u, "total_tokens");
        if (u.HasMember("completion_tokens_details") && u["completion_tokens_details"].IsObject()) {
            ctx.thought_tokens += get_int64(u["completion_tokens_details"], "reasoning_tokens");
        }
    }

    if (!doc.HasMember("choices") || !doc["choices"].IsArray()) return;
    const auto& choices = doc["choices"].GetArray();
    if (choices.Empty()) return;
    const auto& choice0 = choices[0];
    if (!choice0.IsObject() || !choice0.HasMember("delta")) return;
    const auto& delta = choice0["delta"];
    if (!delta.IsObject()) return;

    // Answer and chain-of-thought deltas are both coalesced up to min_chunk_size
    // before an event goes out -- reasoning models tokenize the thought stream
    // just as finely as the answer, and emitting one event per token would flood
    // every consumer downstream (SSE writes; on mobile, one thread-safe-function
    // hop plus a UI re-render each). Crossing from one stream to the other
    // flushes the other buffer first, so events are delivered in arrival order.

    if (delta.HasMember("content") && delta["content"].IsString()) {
        std::string txt(delta["content"].GetString(), delta["content"].GetStringLength());
        if (!txt.empty()) {
            if (!flush_thinking(ctx)) return;   // thought stream ended: emit its tail first
            ctx.text_buffer.append(txt.data(), txt.size());
            ctx.full_text.append(txt.data(), txt.size());
            if (ctx.text_buffer.size() >= ctx.min_chunk_size) {
                flush_text(ctx);
            }
        }
    }

    // Some providers (e.g. DeepSeek-R1 derivatives via vLLM) emit a separate
    // `reasoning_content` field for the model's chain-of-thought.
    if (delta.HasMember("reasoning_content") && delta["reasoning_content"].IsString()) {
        std::string txt(delta["reasoning_content"].GetString(),
                             delta["reasoning_content"].GetStringLength());
        if (!txt.empty()) {
            if (!flush_text(ctx)) return;       // answer stream paused: keep ordering
            ctx.thinking_buffer.append(txt.data(), txt.size());
            if (ctx.thinking_buffer.size() >= ctx.min_chunk_size) {
                flush_thinking(ctx);
            }
        }
    }

    if (delta.HasMember("tool_calls") && delta["tool_calls"].IsArray()) {
        process_tool_call_fragments(ctx, delta["tool_calls"]);
    }
}

//------------------------------------------------------------------------------
// Message construction (raw JSON objects, appended across tool-loop rounds)
//------------------------------------------------------------------------------

typedef rapidjson::Writer<rapidjson::StringBuffer> JsonWriter;

std::string finish(rapidjson::StringBuffer& buf) {
    return std::string{buf.GetString(), buf.GetSize()};
}

// {"role": <role>, "content": <content>}
std::string make_message(const std::string& role, const std::string& content) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("role");    w.String(role.data(),    static_cast<rapidjson::SizeType>(role.size()));
    w.Key("content"); w.String(content.data(), static_cast<rapidjson::SizeType>(content.size()));
    w.EndObject();
    return finish(buf);
}

// The assistant turn that issued the tool calls, replayed back verbatim so the
// follow-up request has the call/response pairing OpenAI requires.
std::string make_assistant_tool_call_message(const std::string& text,
                                             const std::map<int, PendingToolCall>& calls) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("role"); w.String("assistant");
    w.Key("content");
    if (text.empty()) w.Null();
    else              w.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
    w.Key("tool_calls");
    w.StartArray();
    for (auto it = calls.begin(); it != calls.end(); ++it) {
        const PendingToolCall& slot = it->second;
        const std::string args = slot.arguments.empty() ? std::string{"{}"} : slot.arguments;
        w.StartObject();
        w.Key("id");   w.String(slot.id.data(),   static_cast<rapidjson::SizeType>(slot.id.size()));
        w.Key("type"); w.String("function");
        w.Key("function");
        w.StartObject();
        w.Key("name");      w.String(slot.name.data(), static_cast<rapidjson::SizeType>(slot.name.size()));
        // OpenAI expects `arguments` as a JSON-encoded *string*.
        w.Key("arguments"); w.String(args.data(),      static_cast<rapidjson::SizeType>(args.size()));
        w.EndObject();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return finish(buf);
}

// {"role": "tool", "tool_call_id": <id>, "content": <result>}
std::string make_tool_result_message(const std::string& tool_id, const std::string& result) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("role");         w.String("tool");
    w.Key("tool_call_id"); w.String(tool_id.data(), static_cast<rapidjson::SizeType>(tool_id.size()));
    w.Key("content");      w.String(result.data(),  static_cast<rapidjson::SizeType>(result.size()));
    w.EndObject();
    return finish(buf);
}

//------------------------------------------------------------------------------
// Request building
//------------------------------------------------------------------------------

// Does `tools_json` describe at least one tool? Distinguishes "no tools" ("[]",
// or blank) from a populated array without paying for a full parse.
bool has_any_tool(const std::string& tools_json) {
    for (std::size_t i = 0; i < tools_json.size(); ++i) {
        const char c = tools_json[i];
        if (c == '{') return true;                       // first member object
        if (c == ']') return false;                      // closed before any member
        if (c != '[' && !std::isspace(static_cast<unsigned char>(c))) return true;
    }
    return false;                                        // empty / unterminated
}

std::string build_request_body(const OpenAIChatOptions& opt,
                               const std::vector<std::string>& message_objs,
                               bool tools_enabled) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();

    w.Key("model");
    w.String(opt.model.data(), static_cast<rapidjson::SizeType>(opt.model.size()));

    w.Key("stream"); w.Bool(true);

    // include_usage forces the server to emit a final chunk carrying usage
    // counters even when streaming — required for the cost summary.
    w.Key("stream_options");
    w.StartObject();
    w.Key("include_usage"); w.Bool(true);
    w.EndObject();

    w.Key("messages");
    w.StartArray();
    for (const auto& m : message_objs) {
        w.RawValue(m.data(), m.size(), rapidjson::kObjectType);
    }
    w.EndArray();

    if (tools_enabled) {
        w.Key("tools");
        w.RawValue(opt.tools_json.data(), opt.tools_json.size(), rapidjson::kArrayType);
        w.Key("tool_choice"); w.String("auto");
    }

    // Provider-specific root fields (see OpenAIChatOptions::extra_body_json).
    // Spliced member-by-member rather than as one blob so the writer keeps its
    // own bookkeeping, and so a malformed value degrades to a warning instead of
    // corrupting the body.
    if (!opt.extra_body_json.empty()) {
        rapidjson::Document extra;
        if (extra.Parse(opt.extra_body_json.c_str()).HasParseError() || !extra.IsObject()) {
            yoai::utils::log_warn(
                "openai-chat: ignoring extra_body_json, not a JSON object: %.120s",
                opt.extra_body_json.c_str());
        } else {
            for (rapidjson::Value::ConstMemberIterator it = extra.MemberBegin();
                 it != extra.MemberEnd(); ++it) {
                rapidjson::StringBuffer vb;
                rapidjson::Writer<rapidjson::StringBuffer> vw(vb);
                it->value.Accept(vw);
                w.Key(it->name.GetString(), it->name.GetStringLength());
                w.RawValue(vb.GetString(), vb.GetSize(), it->value.GetType());
            }
        }
    }

    w.EndObject();
    return finish(buf);
}

//------------------------------------------------------------------------------
// libcurl callbacks
//------------------------------------------------------------------------------

struct CurlContext {
    StreamContext* stream = nullptr;
    std::string error_buf;
    bool error_mode = false;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* cc = static_cast<CurlContext*>(userdata);
    const size_t n = size * nmemb;
    if (cc->error_mode) { cc->error_buf.append(ptr, n); return n; }
    if (cc->stream->aborted) return 0;
    cc->stream->parser.feed(ptr, n, [&](const void* payload, std::size_t /*length*/) {
        if (cc->stream->aborted) return;
        handle_data_event(*cc->stream, static_cast<const char*>(payload));
    });
    return cc->stream->aborted ? 0 : n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* cc = static_cast<CurlContext*>(userdata);
    const size_t len = size * nitems;
    if (len >= 12 && std::memcmp(buffer, "HTTP/", 5) == 0) {
        const char*  sp  = static_cast<const char*>(std::memchr(buffer, ' ', len));
        const size_t pos = sp ? static_cast<size_t>(sp - buffer) : len;
        if (pos + 4 <= len) {
            int  code = 0;
            bool ok   = true;
            for (size_t i = pos + 1; i < pos + 4; ++i) {
                if (buffer[i] < '0' || buffer[i] > '9') { ok = false; break; }
                code = code * 10 + (buffer[i] - '0');
            }
            if (ok) cc->error_mode = (code >= 400);
        }
    }
    return len;
}

//------------------------------------------------------------------------------
// One streaming request
//------------------------------------------------------------------------------

struct RequestOutcome {
    bool        init_failed = false;
    CURLcode    rc          = CURLE_OK;
    long        http_code   = 0;
    bool        http_error  = false;
    std::string http_error_body;
};

// Perform one `/chat/completions` streaming request, feeding chunks into
// `stream`. Does not emit errors itself — the caller maps the outcome onto an
// Error event so the multi-round loop can stop cleanly.
RequestOutcome run_request(const OpenAIChatOptions& opt,
                           const std::string& url,
                           const std::string& auth,
                           const std::string& body,
                           StreamContext& stream) {
    RequestOutcome out;

    CURL* curl = curl_easy_init();
    if (!curl) { out.init_failed = true; return out; }

    CurlContext cc;
    cc.stream = &stream;

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    hdrs = curl_slist_append(hdrs, auth.c_str());

    yoai::utils::configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cc);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cc);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(opt.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,        static_cast<long>(opt.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    out.rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    out.http_error      = cc.error_mode;
    out.http_error_body = std::move(cc.error_buf);

    // Drain any trailing payload the parser is still holding (unless aborted).
    if (!stream.aborted && !out.http_error && out.rc == CURLE_OK) {
        stream.parser.finish([&](const void* payload, std::size_t /*length*/) {
            if (stream.aborted) return;
            handle_data_event(stream, static_cast<const char*>(payload));
        });
    }
    return out;
}

}

//------------------------------------------------------------------------------
// OpenAIChatClient
//------------------------------------------------------------------------------

OpenAIChatClient::OpenAIChatClient(OpenAIChatOptions opt)
    : Chat(Engine::Remote), opt_(std::move(opt)) {}

//------------------------------------------------------------------------------

bool OpenAIChatClient::ainvoke(const std::vector<ChatMessage>& messages,
                               const std::string& system_prompt,
                               const EventHandler& on_event,
                               const UserContext& user,
                               const RequestOptions& req) {
    if (!req.grammar.empty()) {
        // Said out loud rather than dropped. A caller that asked for a grammar
        // is about to parse the reply, and free text arriving where constrained
        // text was requested fails later and further away. GBNF is a llama.cpp
        // concept with no OpenAI-protocol equivalent; a provider-specific
        // `response_format` would go through OpenAIChatOptions::extra_json.
        yoai::utils::log_warn(
            "remote provider ignores the requested grammar; reply is unconstrained");
    }
    auto emit_error = [&](std::string msg) {
        Event e; e.type = EventType::Error; e.content = std::move(msg);
        on_event(e);
    };

    if (opt_.api_key.empty()) {
        emit_error("API key is required for OpenAIChatClient functionality.");
        return false;
    }
    if (messages.empty()) {
        emit_error("Empty message.");
        return false;
    }

    std::string url;
    std::string auth;
    if (opt_.mode == OpenAIMode::Azure) {
        if (opt_.azure_endpoint.empty()) {
            emit_error("OpenAIChatClient: Azure mode requires azure_endpoint.");
            return false;
        }
        if (opt_.azure_deployment.empty()) {
            emit_error("OpenAIChatClient: Azure mode requires azure_deployment.");
            return false;
        }
        std::string ep = opt_.azure_endpoint;
        while (!ep.empty() && ep.back() == '/') ep.pop_back();
        url  = ep + "/openai/deployments/" + opt_.azure_deployment
             + "/chat/completions?api-version=" + opt_.azure_api_version;
        auth = "api-key: " + opt_.api_key;
    } else {
        url  = opt_.base_url + "/chat/completions";
        auth = "Authorization: Bearer " + opt_.api_key;
    }

    // An EMPTY tools array counts as disabled, not just an empty string: with no
    // MCP tools registered, functions_json() yields "[]", which is a non-empty
    // std::string. Emitting `"tools":[], "tool_choice":"auto"` makes strict
    // endpoints reject the whole request -- NVIDIA NIM answers 400 "When using
    // `tool_choice`, `tools` must be set" -- while laxer ones silently accept it.
    const bool tools_enabled = has_any_tool(opt_.tools_json) &&
                               static_cast<bool>(opt_.tool_executor);

    // The running conversation as raw JSON message objects; the tool loop
    // appends the assistant tool-call turn and the tool results between rounds.
    std::vector<std::string> message_objs;
    if (!system_prompt.empty()) message_objs.push_back(make_message("system", system_prompt));
    for (const auto& m : with_annotation_text(messages)) message_objs.push_back(make_message(m.role, m.content));

    // Token counters accumulate across every round; the cost summary is emitted
    // once at the very end.
    std::int64_t in_tok = 0, out_tok = 0, thought_tok = 0, total_tok = 0;

    for (int iter = 0; ; ++iter) {
        const std::string body = build_request_body(opt_, message_objs, tools_enabled);

        StreamContext stream;
        stream.min_chunk_size = opt_.min_chunk_size;
        stream.on_event       = &on_event;

        const RequestOutcome o = run_request(opt_, url, auth, body, stream);

        if (o.init_failed)   { emit_error("curl_easy_init failed"); return false; }
        if (stream.aborted)  return false;
        if (o.rc != CURLE_OK) {
            emit_error(std::string{"transport error: "} + curl_easy_strerror(o.rc));
            return false;
        }
        if (o.http_error) {
            emit_error(o.http_error_body.empty()
                ? std::string{"HTTP "} + std::to_string(o.http_code)
                : o.http_error_body);
            return false;
        }

        // Thinking first: if its buffer still holds anything here, the answer
        // stream never started (e.g. the turn hit the token limit mid-thought),
        // so the thought tail precedes any buffered text chronologically.
        flush_thinking(stream);
        if (stream.aborted) return false;
        flush_text(stream);
        if (stream.aborted) return false;
        flush_tool_calls(stream);
        if (stream.aborted) return false;

        in_tok      += stream.input_tokens;
        out_tok     += stream.output_tokens;
        thought_tok += stream.thought_tokens;
        total_tok   += stream.total_tokens;

        // No tools, no calls, or the safety cap reached → the turn is complete.
        if (!tools_enabled || stream.tool_calls.empty() || iter >= opt_.max_tool_iterations) {
            break;
        }

        // Replay the assistant's tool-call turn, then run each tool and append
        // its result so the next round can use it.
        message_objs.push_back(make_assistant_tool_call_message(stream.full_text, stream.tool_calls));
        for (auto it = stream.tool_calls.begin(); it != stream.tool_calls.end(); ++it) {
            const PendingToolCall& slot = it->second;
            const std::string args   = slot.arguments.empty() ? std::string{"{}"} : slot.arguments;
            const std::string result = opt_.tool_executor(slot.name, args, user);

            Event det;
            det.type    = EventType::QueryDetail;
            det.content = result;
            det.tool_id = slot.id;
            if (!on_event(det)) return false;   // client gone

            message_objs.push_back(make_tool_result_message(slot.id, result));
        }
    }

    Event cs;
    cs.type                = EventType::CostStatistics;
    cs.cost.model          = opt_.model;
    cs.cost.input_tokens   = in_tok;
    cs.cost.output_tokens  = out_tok;
    cs.cost.thought_tokens = thought_tok;
    cs.cost.total_tokens   = total_tok;
    cs.cost.total_cost =
        (static_cast<double>(in_tok) * opt_.input_price +
         static_cast<double>(thought_tok + out_tok) * opt_.output_price)
        / 1e6;
    on_event(cs);

    return true;
}

}
}
