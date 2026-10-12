#pragma once

#include "llm/chat/chat.hpp"
#include "llm/chat/event.hpp"
#include "llm/chat/types.hpp"   // for ChatMessage + EventHandler

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace yoai { namespace llm {

// OpenAI-compatible Chat Completions client.
//
// Targets any endpoint that speaks the `/chat/completions` SSE protocol.
// Two endpoint flavors share the same wire format; only the URL and auth
// header differ:
//
//   Direct: https://api.openai.com/v1/chat/completions
//           Authorization: Bearer {api_key}     (OpenAI proper, OpenRouter,
//                                                Nebula, DashScope, Ark,
//                                                vLLM, llama.cpp's server,
//                                                etc. - flip `base_url` to
//                                                point at the right host)
//
//   Azure : {azure_endpoint}/openai/deployments/{azure_deployment}
//           /chat/completions?api-version={azure_api_version}
//           api-key: {api_key}                  (Azure OpenAI resource;
//                                                deployment name in URL
//                                                wins, body `model` is
//                                                ignored by the server)
//
// Mirrors `OpenAIChatClient.ainvoke` in pub/agents/base/clients.py, including
// the tool-execution loop when tools are supplied (see OpenAIChatOptions::
// tools_json / tool_executor). With no tools configured it behaves as a plain
// streaming client; a stray tool call then surfaces as QueryTitle /
// QueryArguments events and the turn ends without running anything.

enum class OpenAIMode {
    Direct,     // api.openai.com or any OpenAI-compatible host
    Azure,      // {resource}.openai.azure.com with deployment + api-version
};

struct OpenAIChatOptions {
    OpenAIMode mode = OpenAIMode::Direct;

    std::string api_key;

    // Direct mode: base_url + "/chat/completions" is hit.
    std::string base_url    = "https://api.openai.com/v1";
    std::string model       = "gpt-5-nano";

    // Azure mode: the full URL is composed from these three fields.
    // `model` above is still sent in the body for compatibility but Azure
    // ignores it (the deployment in the URL wins).
    std::string azure_endpoint;                          // https://my.openai.azure.com
    std::string azure_deployment;                        // gpt-4o-mini-prod
    std::string azure_api_version = "2024-10-21";        // current GA at writing

    // Per-request prices in $/M tokens.
    double input_price  = 0.0;
    double output_price = 0.0;

    int         connect_timeout_ms = 10000;
    int         request_timeout_ms = 600000;
    std::size_t min_chunk_size     = 30;

    // Tool use (function calling). When `tools_json` is non-empty AND
    // `tool_executor` is set, `tools_json` is sent verbatim as the request's
    // "tools" array (the OpenAI `[{type:function, function:{...}}]` shape, e.g.
    // mcp::registry().functions_json("openai")). Each tool call the model makes
    // is then run through `tool_executor(name, arguments_json, user)` — `user`
    // being the identity passed to ainvoke — its returned string fed back as a
    // tool message, and the model re-queried, up to `max_tool_iterations`
    // rounds. Leaving either field unset disables tool use.
    std::string tools_json;
    std::function<std::string(const std::string& name,
                              const std::string& arguments_json,
                              const UserContext& user)> tool_executor;
    int max_tool_iterations = 8;

    // Provider-specific request fields, as a JSON OBJECT whose members are
    // spliced into the request body's root (e.g. {"chat_template_kwargs":
    // {"thinking":false}}). "OpenAI-compatible" endpoints keep inventing
    // non-standard knobs; this is the escape hatch so a per-model quirk lives in
    // the provider table (res/agents/*.cpp) instead of here. Empty by default,
    // and a value that is not a parseable object is warned about and ignored --
    // never fatal.
    //
    // Known user: NVIDIA NIM's DeepSeek V4 (flash and pro) are reasoning models
    // that stream ONLY `reasoning_content` unless {"chat_template_kwargs":
    // {"thinking":false}} is present, leaving `content` empty for the whole turn.
    std::string extra_body_json;
};

//------------------------------------------------------------------------------
// OpenAIChatClient
//------------------------------------------------------------------------------

// A remote Chat (llm/chat/chat.hpp). It holds nothing but its options, so one
// instance can serve concurrent turns, and the Backend half has nothing to do:
// no weights to load, no threads to set and no conversation state to forget.
class OpenAIChatClient : public Chat {
public:
    explicit OpenAIChatClient(OpenAIChatOptions opt);

    OpenAIChatClient(const OpenAIChatClient&)            = delete;
    OpenAIChatClient& operator=(const OpenAIChatClient&) = delete;

    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string& system_prompt,
                 const EventHandler& on_event,
                 const UserContext& user = UserContext(),
                 const RequestOptions& req = RequestOptions()) override;

    bool load(std::string&) override { return true; }
    bool loaded() const override { return true; }
    void set_threads(int) override {}
    void forget() override {}

    const OpenAIChatOptions& options() const { return opt_; }

private:
    OpenAIChatOptions opt_;
};

}
}
