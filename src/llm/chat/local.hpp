#pragma once

// On-device LLM client: llama.cpp + a GGUF model, no network.
//
// A local Chat (llm/chat/chat.hpp): constructing one starts llama.cpp before
// anything here runs (see Backend's constructor). yoai.cpp runs a
// turn the same way whether an id resolves to this or to OpenAIChatClient;
// nothing above the client needs to know a turn ran locally.
//
// Built only when configured with -DYOAI_ONDEVICE_LLM=ON and pointed at a
// llama.cpp SDK via -DLLAMA_CPP_DIR=<dir with include/llama.h + lib/>.
// Otherwise a stub compiles in: available() is false and ainvoke() reports a
// single Error event. That is the supported off state, not a degraded one --
// the default desktop build has no llama.cpp and still links.
//
// Threading: ainvoke() BLOCKS the calling thread for the whole turn (a decode
// loop, seconds to minutes), exactly as OpenAIChatClient blocks on its HTTP
// transfer. The caller owns threading; on HarmonyOS that is the NAPI worker.
//
// Lifetime: the model is loaded once, lazily on the first ainvoke(), and kept
// for the object's life -- a multi-GB reload per turn would be unusable. Each
// turn gets a fresh context, so turns are stateless and history is replayed
// from `messages`, matching how the remote client behaves.

#include "llm/backend.hpp"
#include "llm/chat/chat.hpp"
#include "llm/chat/event.hpp"
#include "llm/chat/types.hpp"   // ChatMessage, EventHandler, UserContext

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace yoai { namespace llm {

struct LocalOptions {
    std::string model_path;      // absolute path to a .gguf

    /**
     * Ceiling for the context window. The turn takes the largest size that is both
     * within what the model was TRAINED for and what actually allocates, walking a
     * descending ladder from here.
     *
     * Chosen by allocation rather than arithmetic because the arithmetic is not
     * available: head_dim is not derivable from the metadata every model publishes,
     * and the layer discount a hybrid attention/SSM model earns has no portable key.
     * An estimate would be wrong in both directions; whether the buffer allocates is
     * a fact. It also adapts to the device for free -- a 2B takes the top rung where
     * a 4B on a small phone quietly settles lower.
     */
    int n_ctx_max = 32768;

    /**
     * Worker threads. NOT hardware_concurrency(): on a three-cluster phone SoC the
     * all-cores default is routinely the WORST count, because the batch waits on the
     * little cores. <= 0 falls back to hardware_concurrency().
     *
     * **Measured** on the target device (Kirin 9020, Qwen3.5-2B Q4_K_M, two runs
     * per setting through the /asr bench), decode in tokens per second:
     *
     *     threads   1     2     4     6     8
     *     decode    5.8   9.0   12.6  12.7  12.9
     *     prefill   22.7  29.0  48.4  46.3  50.1
     *
     * **Both phases stop improving at four.** That is the core count, not a
     * coincidence: this SoC is one big plus three mid plus four little, so four
     * threads is exactly its performance cores and the little ones contribute
     * nothing to either phase.
     *
     * 4 rather than 6, then — the same throughput while leaving two more cores
     * to the UI and the battery.
     *
     * The decode row also disproves a claim this project carried for a while,
     * that one core already saturates the memory controller: one core reaches
     * 7.4 GB/s of weight reads and four reach about 16.5.
     *
     * **It says nothing about where the bus is**, and an earlier version of this
     * note claimed it did — that four cores take ~85% of the chip's bandwidth,
     * leaving nothing for another compute unit. That was the same measurement
     * read twice: the plateau is explained two paragraphs up by running out of
     * performance cores, which is a COMPUTE limit, and a curve that flattens at
     * the usable-core count cannot also be evidence the bus is full.
     *
     * A read loop with no model in it measures **24.4 GB/s** on this device at
     * eight threads, and that is a lower bound — every fix to the benchmark
     * raised it. So llama.cpp's decode sits at 73-75% of what the chip gives up,
     * the headroom is real, and design/voice.md has the sweep and the caveats.
     */
    int n_threads = 4;

    /**
     * Layers to offload to a GPU backend. 0 because this repo builds llama.cpp
     * CPU-only for HarmonyOS (see harmony/build.cmd for the measurements that
     * decided it), so asking for offload would only be a claim the binary cannot
     * honour. 999 means "all" if a GPU-capable SDK is ever linked.
     */
    int n_gpu_layers = 0;

    /**
     * Optional extra ceiling on tokens per reply; <= 0 means "only the context
     * limits it".
     *
     * NOT a policy cap -- nothing is billed here and cancelling takes effect within
     * one token. It is a backstop: a runaway loop at ~11 tok/s is otherwise close to
     * an hour of a phone heating up in someone's pocket. 8192 is more than any
     * genuine answer, so it never truncates real output but does end a loop.
     */
    int n_predict = 8192;

    /**
     * Tokens always kept free for the answer. History is trimmed until the prompt
     * fits in `n_ctx - n_reply_reserve`, so a long conversation can never squeeze
     * the reply down to nothing.
     *
     * This is what makes multi-turn work at all: every turn replays the whole
     * conversation, so without trimming the prompt grows until it fills the window.
     */
    int n_reply_reserve = 1024;

    /**
     * Whether the model should reason before answering: 1 on, 0 off, -1 leave it to
     * the model.
     *
     * Explicit because otherwise it is an ACCIDENT. The model's Jinja template
     * decides this with an `enable_thinking` conditional, but
     * llama_chat_apply_template has no Jinja engine -- it maps to the built-in
     * CHATML form, which emits neither branch, and the model then falls back to
     * whatever its training made default. Reasoning also costs real time: the
     * thinking pass decodes at the same tok/s as the answer.
     */
    int thinking = 0;

    //-- sampling ---------------------------------------------------------------

    /**
     * Greedy decoding (temperature 0) is the textbook way to get a repetition loop.
     * temperature <= 0 selects it anyway, kept so a benchmark can ask for
     * determinism.
     */
    float temperature = 0.7f;
    float top_p       = 0.8f;
    int   top_k       = 20;

    /// 1.0 disables. Penalizing recent tokens is what actually breaks a loop.
    float repeat_penalty = 1.1f;
    int   repeat_last_n  = 256;
};

/// What the last turn cost. Zeroed at the start of every ainvoke(); a turn that
/// completes turns it into the same CostStatistics event a remote turn ends
/// with, so the host's token counter works on both lanes.
struct LocalStats {
    double load_ms        = 0;   // model load; non-zero only on the turn that paid
    double prefill_ms     = 0;
    double decode_ms      = 0;
    /// Time to the FIRST decoded token, inside decode_ms rather than beside it.
    ///
    /// Separated because a short reply cannot be read without it. Two turns of
    /// the same model measured 14.0 tok/s over 106 tokens and 8.2 over 11 — not
    /// a slowdown, a per-turn cost the short reply had nowhere to hide. Solving
    /// the pair put it near half a second, which is most of a chain's wait
    /// before the first spoken word, so it is worth a number of its own instead
    /// of an inference across mismatched runs.
    double first_token_ms = 0;
    int    prompt_tokens  = 0;
    int    decoded_tokens = 0;
    int    n_threads      = 0;   // what the turn actually ran with
    /// Prompt tokens the KV already held, so prefill skipped them. High on a
    /// second turn with the same system prompt; zero after anything that
    /// invalidated the cache.
    int    reused_tokens  = 0;
    int    n_ctx          = 0;   // context this turn got (see n_ctx_max)
    /// Oldest messages shed to make the prompt fit; > 0 means the window slid.
    int    dropped_msgs   = 0;
};

/**
 * Length of the longest prefix of `bytes` containing only COMPLETE UTF-8 sequences.
 *
 * A token is a byte sequence, not a character: Qwen and friends use byte-level BPE,
 * so one CJK character (3 bytes in UTF-8) is routinely split across two tokens.
 * Forwarding a token's bytes straight on therefore ships half a character, which
 * renders as a replacement glyph -- and once the host has substituted U+FFFD, the
 * arrival of the remaining bytes cannot repair it. The caller keeps the incomplete
 * tail and prepends it to the next token.
 *
 * A free function, and compiled unconditionally, so it is testable without
 * llama.cpp.
 */
std::size_t utf8_complete_len(const std::string& bytes);

/**
 * Splits a raw on-device token stream into answer text and `<think>` reasoning.
 *
 * The remote client receives reasoning as its own event because the wire protocol
 * separates it (`reasoning_content`). llama.cpp hands back only raw text, so a
 * reasoning model's markers arrive inline -- Qwen3 opens every reply with
 * `<think>...</think>`, and even in non-thinking mode emits the empty pair -- and
 * without this they render literally in the bubble.
 *
 * Incremental on purpose: a tag is NOT delivered atomically. `<think>` can arrive
 * as `<` + `think` + `>` or `<th` + `ink>`, so anything that scans a whole string
 * would miss it. feed() holds back only the longest suffix that could still become
 * a tag and emits the rest immediately, which keeps streaming responsive.
 *
 * Compiled unconditionally (no llama.cpp dependency).
 */
class ThinkSplitter {
public:
    /// Called per piece; `thinking` says which stream it belongs to. Return false
    /// to abort, which feed()/flush() propagate.
    using Emit = std::function<bool(bool thinking, const std::string& text)>;

    /// Consume one chunk of raw model text.
    bool feed(const std::string& chunk, const Emit& emit);

    /// Release anything still held at end of stream. A partial tag never completed,
    /// so it was content after all.
    bool flush(const Emit& emit);

    /// True while inside an unterminated `<think>`.
    bool thinking() const { return in_think_; }

private:
    std::string buf_;
    bool in_think_ = false;
};

//------------------------------------------------------------------------------

class LocalClient : public Chat {
public:
    /// True when compiled against llama.cpp (YOAI_ONDEVICE_LLM).
    static bool available();

    explicit LocalClient(LocalOptions opt);
    ~LocalClient() override;

    LocalClient(const LocalClient&)            = delete;
    LocalClient& operator=(const LocalClient&) = delete;

    /// Stream a reply. Emits Reply deltas, Thinking for `<think>` spans, and a
    /// CostStatistics built from stats() when the turn completes; on failure a
    /// single Error. Returns true when the turn completed; false on error or when
    /// `on_event` returned false (the handler aborting is not an error -- it is
    /// how the host cancels, exactly as on the remote lane).
    /// `user` is accepted for interface parity and ignored: a local turn runs no
    /// tools, so there is no identity to forward.
    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string&              system_prompt,
                 const EventHandler&             on_event,
                 const UserContext&              user = UserContext(),
                 const RequestOptions&           req  = RequestOptions()) override;

    /// Threads for subsequent turns; <= 0 means hardware_concurrency(). Takes
    /// effect on the next turn without reloading the model, because the count is
    /// applied when a turn builds its context, not when the weights are read.
    void set_threads(int n) override;

    /// Forget the conversation: drop the KV cache and the token sequence the
    /// prefix reuse is computed against. The weights stay resident.
    ///
    /// **This is what starting a new conversation should call**, and it is not
    /// the same as sending a shorter prompt. Turns are stateless -- the whole
    /// transcript is replayed -- so a caller may reasonably expect a one-message
    /// prompt to mean a fresh context. It does not by itself: what is left over
    /// is removed by rewinding the cache to the point where the old and new
    /// prompts diverge, and a model whose memory cannot rewind that far refuses,
    /// which costs the reuse and, before that refusal was checked, the
    /// conversation's privacy from the next one. Saying so explicitly is cheaper
    /// and cannot be refused: removing a whole sequence always succeeds.
    void forget() override;

    /// True once the model is resident (i.e. after a successful first turn).
    bool loaded() const override;

    /**
     * Read the weights now, so the first turn does not pay for them.
     *
     * The load is the dominant cost of an on-device turn (seconds, a few GB off
     * disk) and it cannot be made cheaper -- only moved somewhere nobody is
     * waiting. A UI that knows which model the user picked can call this while they
     * are still typing.
     *
     * Idempotent; returns false with `err` set if the file will not load. A no-op
     * returning false in the stub build.
     */
    bool load(std::string& err) override;

    LocalStats stats() const;

    const LocalOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
