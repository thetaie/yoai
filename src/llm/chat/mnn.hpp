#pragma once

// On-device chat on MNN: an MNN LLM export, no network. A Chat
// (llm/chat/chat.hpp) with Engine::Mnn, so yoai.cpp runs a turn on it
// exactly as it does on LocalClient or OpenAIChatClient.
//
// The model is a DIRECTORY, not a file: MNN's llmexport writes config.json,
// llm.mnn, its weights and a tokenizer side by side, and resolves every other
// file relative to config.json.
//
// What it shares with LocalClient, and what it does not:
//
//   - Same events: Reply, Thinking for `<think>` spans (the same ThinkSplitter),
//     and one CostStatistics when a turn completes.
//   - Same prefix reuse across turns, done by MNN's own prompt_cache: a turn
//     prefills only what differs from the last one's transcript.
//   - NO grammar. MNN has no constrained sampling, so RequestOptions::grammar
//     is logged and ignored -- the same answer the remote client gives. A caller
//     that needs a constrained reply needs llama.cpp.
//   - Threads apply at the next load, because MNN builds its runtime with a
//     thread count when it loads.
//
// Text only. An omni export (Qwen2.5-Omni) loads and chats, but its Talker is
// switched off here; speech output is voice/omni's job.
//
// Built only with -DYOAI_MNN=ON and an MNN SDK at -DMNN_DIR. Otherwise a stub
// compiles in: available() is false, load() fails and ainvoke() sends a single
// Error, the same off state LocalClient has.
//
// Threading: ainvoke() blocks for the whole turn, and turns are serialized per
// object. The caller owns threading.

#include "llm/chat/chat.hpp"

#include <memory>
#include <string>
#include <vector>

namespace yoai { namespace llm {

struct MnnOptions {
    std::string model_dir;   // the directory holding config.json

    // Applied at load; see set_threads(). 4 for the reason in
    // LocalOptions::n_threads: on a phone the performance cores run out there.
    int n_threads = 4;

    // Whether the model reasons before answering: 1 on, 0 off, -1 leave it to
    // the model. Passed to the chat template as enable_thinking, which is what
    // Qwen3's template reads.
    int thinking = 0;

    // Ceiling on tokens per reply, as a backstop against a loop rather than a
    // policy. MNN's own default is 512, which cuts real answers short.
    int n_predict = 8192;
};

class MnnClient : public Chat {
public:
    /// True when compiled against MNN (YOAI_MNN).
    static bool available();

    explicit MnnClient(MnnOptions opt);
    ~MnnClient() override;

    MnnClient(const MnnClient&)            = delete;
    MnnClient& operator=(const MnnClient&) = delete;

    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string&              system_prompt,
                 const EventHandler&             on_event,
                 const UserContext&              user = UserContext(),
                 const RequestOptions&           req  = RequestOptions()) override;

    /// Read the weights now. Idempotent.
    bool load(std::string& err) override;

    bool loaded() const override;

    /// Stored for the next load: MNN fixes its thread count when it loads, and
    /// this never reloads. Logged, so a sweep can see it did not apply yet.
    void set_threads(int n) override;

    /// Drop the KV cache and the prompt cache. The weights stay resident.
    void forget() override;

    const MnnOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
