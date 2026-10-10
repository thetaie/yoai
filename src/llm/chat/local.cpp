#include "llm/chat/local.hpp"

#include "llm/chat/annotations.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <mutex>

#ifdef YOAI_ONDEVICE_LLM
#include "llama.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#endif

namespace yoai { namespace llm {

namespace {

#ifdef YOAI_ONDEVICE_LLM

using clk = std::chrono::steady_clock;

double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

// What goes after the template's opening of the reply to say whether the
// model reasons first: "<think>\n" to let it, the block already closed to not,
// and nothing when `thinking` < 0 or the model's own template does not speak
// this protocol -- appending <think> to anything else would be prompt noise.
std::string think_suffix(llama_model* model, int thinking) {
    const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr);
    if (thinking < 0 || tmpl == nullptr || std::strstr(tmpl, "<think>") == nullptr) {
        return std::string();
    }
    return thinking > 0 ? "<think>\n" : "<think>\n\n</think>\n\n";
}

/**
 * Build the prompt for one turn.
 *
 * Prefers the model's own chat template. Gemma is the known exception:
 * llama_chat_apply_template only maps a fixed list of built-in templates (it has no
 * Jinja engine) and has no entry for the newer Gemma architectures, so it returns
 * empty -- which silently degrades to a 1-token prompt and a model that ignores the
 * question. Detect that and hand-build the Gemma turn markers instead.
 */
std::string build_prompt(llama_model*                    model,
                         const std::vector<ChatMessage>& messages,
                         const std::string&              system_prompt,
                         int                             thinking) {
    // Gemma has no system role: the system prompt is folded into the first user
    // turn. Doing it here rather than per-template keeps one prompt shape.
    //
    // Annotations go in first, so the system prompt lands in front of the first
    // message's annotation line, not inside it -- and warm()'s front, which ends
    // where the first message's text begins, is still the front of this prompt.
    const std::vector<ChatMessage> annotated = with_annotation_text(messages);
    std::vector<ChatMessage> msgs;
    msgs.reserve(annotated.size());
    for (std::size_t i = 0; i < annotated.size(); ++i) {
        const ChatMessage& m = annotated[i];
        if (m.role == "user" || m.role == "assistant") {
            msgs.push_back(m);
        }
    }
    if (msgs.empty()) {
        return std::string();
    }
    if (!system_prompt.empty()) {
        msgs.front().content = system_prompt + "\n\n" + msgs.front().content;
    }

    const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr);

    if (tmpl != nullptr) {
        std::vector<llama_chat_message> lc;
        lc.reserve(msgs.size());
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            llama_chat_message one;
            one.role    = msgs[i].role.c_str();
            one.content = msgs[i].content.c_str();
            lc.push_back(one);
        }

        int need = llama_chat_apply_template(tmpl, lc.data(), lc.size(),
                                             /*add_ass=*/true, nullptr, 0);
        if (need > 0) {
            std::vector<char> buf(static_cast<std::size_t>(need) + 1);
            need = llama_chat_apply_template(tmpl, lc.data(), lc.size(), true,
                                             buf.data(), static_cast<int>(buf.size()));
            if (need > 0) {
                std::string out(buf.data(), static_cast<std::size_t>(need));
                // The built-in CHATML form emits neither branch of the template's
                // `enable_thinking` conditional, so state the choice ourselves.
                return out + think_suffix(model, thinking);
            }
        }
    }

    // Fallback: Gemma turn markers. "model" is the assistant role name here.
    std::string out;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        out += "<start_of_turn>";
        out += (msgs[i].role == "assistant" ? "model" : "user");
        out += "\n" + msgs[i].content + "<end_of_turn>\n";
    }
    out += "<start_of_turn>model\n";
    return out;
}

/** `text` as the model's tokens, special markers parsed. False on failure. */
bool tokenize_prompt(const llama_vocab* vocab, const std::string& text,
                     std::vector<llama_token>* out) {
    const int n = -llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                                  nullptr, 0, /*add_special=*/true, /*parse_special=*/true);
    if (n <= 0) {
        return false;
    }
    out->resize(static_cast<std::size_t>(n));
    return llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                          out->data(), n, true, true) >= 0;
}

// The same without a BOS or any other token the vocabulary adds at the front:
// for a piece of a prompt, whose tokens are compared against the whole's tail.
bool tokenize_prompt_raw(const llama_vocab* vocab, const std::string& text,
                         std::vector<llama_token>* out) {
    const int n = -llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                                  nullptr, 0, /*add_special=*/false, /*parse_special=*/true);
    if (n <= 0) {
        return false;
    }
    out->resize(static_cast<std::size_t>(n));
    return llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                          out->data(), n, false, true) >= 0;
}

#endif   // YOAI_ONDEVICE_LLM

bool emit_event(const EventHandler& on_event, EventType type, const std::string& content) {
    Event e;
    e.type    = type;
    e.content = content;
    return on_event ? on_event(e) : true;
}

#ifdef YOAI_ONDEVICE_LLM

/**
 * The end of a turn that completed: the usage event, and one log line.
 *
 * The engine has no wire protocol to carry token counts, so it reports them as
 * the same CostStatistics event the remote lane ends with. That is what lets a
 * host show one token counter instead of two. Not sent for a turn that failed
 * or was cancelled: a partial count presented as a total is worse than none.
 */
void report_turn(const LocalOptions& opt, const LocalStats& st, const EventHandler& on_event) {
    // The file name, not the path and not the host's id: "Qwen3-4B-Q4_K_M.gguf"
    // is what the user recognizes, and it is what the remote lane reports here
    // too -- the model, not the entry.
    const std::size_t slash = opt.model_path.find_last_of("/\\");

    Event e;
    e.type               = EventType::CostStatistics;
    e.cost.model         = slash == std::string::npos ? opt.model_path
                                                      : opt.model_path.substr(slash + 1);
    e.cost.input_tokens  = st.prompt_tokens;
    e.cost.output_tokens = st.decoded_tokens;
    e.cost.total_tokens  = st.prompt_tokens + st.decoded_tokens;
    e.cost.total_cost    = 0.0;   // nobody was billed; the tokens still matter
    if (on_event) {
        on_event(e);
    }

    // threads and the two rates are on this line rather than derived by whoever
    // reads the log: a sweep is compared across many of these, and pairing each
    // one with a separate "threads ->" line breaks the moment another turn
    // interleaves. st.n_threads is what the turn ACTUALLY ran with, after <= 0
    // has been resolved to a core count.
    yoai::utils::log_info(
        "on-device turn: %d prompt (%d reused) + %d decoded tokens, ctx %d, "
        "threads %d, load %.0fms prefill %.0fms (%.1f tok/s) "
        "decode %.0fms (first %.0fms, then %.1f tok/s), %d msgs dropped",
        st.prompt_tokens, st.reused_tokens, st.decoded_tokens, st.n_ctx,
        st.n_threads, st.load_ms,
        st.prefill_ms,
        st.prefill_ms > 0
            ? (st.prompt_tokens - st.reused_tokens) * 1000.0 / st.prefill_ms : 0.0,
        st.decode_ms, st.first_token_ms,
        // The rate AFTER the first token, which is the one that predicts a
        // longer reply. Dividing the whole span by the whole count mixes in a
        // per-turn cost that does not repeat, and on an eleven-token reply that
        // cost is most of the span.
        (st.decode_ms - st.first_token_ms) > 0 && st.decoded_tokens > 1
            ? (st.decoded_tokens - 1) * 1000.0 / (st.decode_ms - st.first_token_ms)
            : 0.0,
        st.dropped_msgs);
}

#endif   // YOAI_ONDEVICE_LLM

const char* const kThinkOpen  = "<think>";
const char* const kThinkClose = "</think>";

/**
 * How much of `s` may be emitted without risking splitting `tag` across two feeds:
 * everything except the longest suffix of `s` that is also a prefix of `tag`.
 */
std::size_t emittable(const std::string& s, const std::string& tag) {
    const std::size_t max_hold = std::min(s.size(), tag.size() - 1);
    for (std::size_t hold = max_hold; hold > 0; --hold) {
        if (s.compare(s.size() - hold, hold, tag, 0, hold) == 0) {
            return s.size() - hold;
        }
    }
    return s.size();
}

}   // namespace

//------------------------------------------------------------------------------
// Stream shaping -- compiled in every build, llama.cpp or not
//------------------------------------------------------------------------------

std::size_t utf8_complete_len(const std::string& bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        std::size_t need;
        if      ((c & 0x80) == 0x00) { need = 1; }   // ASCII
        else if ((c & 0xE0) == 0xC0) { need = 2; }
        else if ((c & 0xF0) == 0xE0) { need = 3; }   // most CJK lands here
        else if ((c & 0xF8) == 0xF0) { need = 4; }
        else                         { need = 1; }   // stray continuation or invalid
                                                     // lead: pass it through rather
                                                     // than stall the stream forever
        if (i + need > bytes.size()) {
            return i;                                // the incomplete tail starts here
        }
        i += need;
    }
    return bytes.size();
}

bool ThinkSplitter::feed(const std::string& chunk, const Emit& emit_fn) {
    buf_ += chunk;
    while (true) {
        const std::string tag = in_think_ ? kThinkClose : kThinkOpen;
        const std::size_t at = buf_.find(tag);
        if (at != std::string::npos) {
            // Text before the marker belongs to whichever stream we were in.
            if (at > 0 && !emit_fn(in_think_, buf_.substr(0, at))) {
                return false;
            }
            buf_.erase(0, at + tag.size());   // the marker itself is never emitted
            in_think_ = !in_think_;
            continue;                         // a reply can hold several blocks
        }
        const std::size_t n = emittable(buf_, tag);
        if (n > 0) {
            if (!emit_fn(in_think_, buf_.substr(0, n))) {
                return false;
            }
            buf_.erase(0, n);
        }
        return true;
    }
}

bool ThinkSplitter::flush(const Emit& emit_fn) {
    if (buf_.empty()) {
        return true;
    }
    const std::string out = buf_;
    buf_.clear();
    return emit_fn(in_think_, out);
}

//------------------------------------------------------------------------------

#ifdef YOAI_ONDEVICE_LLM

struct LocalClient::Impl {
    LocalOptions opt;
    llama_model* model = nullptr;
    LocalStats   stats;
    // ainvoke() is documented as caller-threaded, but the model is shared state:
    // serialize turns so a second caller cannot decode against a half-built context.
    std::mutex turn;

    /**
     * The context outlives the turn, and `cached` is what its KV holds.
     *
     * Turns used to build a context each time, on the reasoning that history is
     * replayed from `messages` so there is no state worth keeping. That is true
     * of the CONVERSATION and false of the arithmetic: a prompt's leading tokens
     * are identical from one turn to the next -- always the system prompt, and
     * in a conversation the whole of the history before the new message -- and
     * prefilling them again costs time strictly proportional to how many there
     * are. Measured on the target device at 47 tok/s of prefill, so a 300-token
     * system prompt is six seconds per turn, every turn, for a string that did
     * not change.
     *
     * `cached` is the exact token sequence sitting at positions [0, size). A
     * turn keeps their common prefix, drops the KV from there on, and prefills
     * only what is new. It must be cleared whenever the KV is disturbed by
     * anything else, because a `cached` that disagrees with the memory produces
     * a reply conditioned on text nobody wrote -- fluent, plausible, and wrong.
     */
    llama_context*           ctx = nullptr;
    std::vector<llama_token> cached;

    /**
     * A snapshot of the sequence at the end of the last turn's prompt, before
     * its think suffix, and the tokens it holds.
     *
     * For a memory that cannot be rewound (Qwen3.5 is hybrid). The empty think
     * block that turns reasoning off sits in the KV ahead of each reply, and
     * the template renders a past reply without it, so the next prompt parts
     * from `cached` exactly there -- and a memory that cannot drop the tail
     * used to prefill the whole conversation again every turn: 3.2 s for 229
     * tokens. Restoring this puts it back at that point instead, so only the
     * reply and the new message are prefilled.
     *
     * Writing the block into past replies was tried first, and makes the next
     * prompt match the cache exactly -- and Qwen3.5-2B, shown a history no
     * template produces, answered with a stray </think> in the middle.
     */
    std::vector<llama_token> ckpt_tokens;
    std::vector<uint8_t>     ckpt_state;

    explicit Impl(LocalOptions o) : opt(std::move(o)) {}

    ~Impl() {
        if (ctx != nullptr) {
            llama_free(ctx);
        }
        if (model != nullptr) {
            llama_model_free(model);
        }
    }

    /** Forget the prefix. Cheap, and the correct response to any doubt. */
    void drop_cache() {
        cached.clear();
        ckpt_tokens.clear();
        ckpt_state.clear();
        if (ctx != nullptr) {
            llama_memory_seq_rm(llama_get_memory(ctx), 0, 0, -1);
        }
    }

    // Loads on first use; a no-op afterwards. `err` is set only on failure.
    bool ensure_model(std::string& err) {
        if (model != nullptr) {
            return true;
        }
        const clk::time_point t0 = clk::now();
        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = opt.n_gpu_layers;
        model = llama_model_load_from_file(opt.model_path.c_str(), mp);
        if (model == nullptr) {
            err = "could not load model: " + opt.model_path;
            return false;
        }
        stats.load_ms = ms_since(t0);
        return true;
    }

    // The context, built on first need and kept, with this turn's thread count
    // applied. Needs the model. Fills stats.n_threads and stats.n_ctx.
    bool ensure_context(std::string& err) {
        int n_threads = opt.n_threads;
        if (n_threads <= 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            n_threads = hw > 0 ? static_cast<int>(hw) : 4;
        }

        /**
         * Take the largest context that both the model supports and the device
         * can actually give us, walking down from n_ctx_max.
         *
         * The rungs halve, so this costs at most a few failed allocations, and
         * llama_init_from_model returning null is the only trustworthy signal that
         * a size does not fit (see LocalOptions::n_ctx_max for why arithmetic is
         * not an option).
         */
        llama_context_params cp = llama_context_default_params();
        cp.n_ubatch        = 512;   // physical batch: what sizes the compute buffer
        cp.n_threads       = n_threads;
        cp.n_threads_batch = n_threads;
        stats.n_threads    = n_threads;

        const int trained = llama_model_n_ctx_train(model);
        int want = opt.n_ctx_max;
        if (trained > 0 && trained < want) {
            want = trained;                  // never ask for more than it was trained on
        }

        // n_batch tracks n_ctx because a prefill hands the WHOLE prompt to one
        // llama_decode, and the prompt is fitted to `n_ctx - n_reply_reserve`.
        // n_batch is the logical batch -- the most tokens a single decode may take
        // -- and llama.cpp enforces it with an assert that aborts the PROCESS
        // rather than returning an error the caller could report. A fixed 512
        // there means any conversation whose prompt passes 512 tokens takes the
        // app down on a real device. The physical batch (n_ubatch, above) is what
        // actually costs memory; llama.cpp splits the logical batch into n_ubatch
        // chunks itself, so this stays free.
        //
        // Built once and kept. The ladder therefore runs once too, so its failed
        // allocations are a startup cost rather than a per-turn one, and n_ctx
        // stops varying between turns -- which it could before, if memory
        // pressure changed, silently moving the point at which history gets shed.
        if (ctx == nullptr) {
            for (int c = want; c >= 1024; c /= 2) {
                cp.n_ctx   = static_cast<uint32_t>(c);
                cp.n_batch = static_cast<uint32_t>(c);
                ctx = llama_init_from_model(model, cp);
                if (ctx != nullptr) {
                    break;
                }
            }
            if (ctx == nullptr) {
                err = "could not create a context for this model";
                return false;
            }
        }
        // Applied per call rather than at creation, because set_threads() may have
        // changed the count since, and a kept context would otherwise ignore it.
        llama_set_n_threads(ctx, n_threads, n_threads);
        stats.n_ctx = static_cast<int>(llama_n_ctx(ctx));
        return true;
    }
};

bool LocalClient::available() { return true; }

LocalClient::LocalClient(LocalOptions opt)
    : Chat(Engine::LlamaCpp), impl_(new Impl(std::move(opt))) {}
LocalClient::~LocalClient() = default;

void LocalClient::set_threads(int n) {
    std::lock_guard<std::mutex> lock(impl_->turn);   // never mid-turn
    impl_->opt.n_threads = n;
}

void LocalClient::forget() {
    std::lock_guard<std::mutex> lock(impl_->turn);   // never mid-turn
    impl_->drop_cache();
}

bool LocalClient::warm(const std::string& system_prompt, std::string& err) {
    // Under the turn lock, so a turn that arrives meanwhile waits for this and
    // then reuses it, rather than racing it for the same KV.
    std::lock_guard<std::mutex> lock(impl_->turn);
    if (!impl_->ensure_model(err) || !impl_->ensure_context(err)) {
        return false;
    }
    llama_model*       model = impl_->model;
    const llama_vocab* vocab = llama_model_get_vocab(model);

    // The system prompt rides in the first user message (see build_prompt), so
    // what every turn shares is the rendering up to where that message's own
    // text starts. Two different messages find exactly that point, in tokens --
    // a boundary token that merged with the message would simply not be shared.
    std::vector<ChatMessage> one(1), two(1);
    one[0].role = "user";
    one[0].content = "\xE7\x94\xB2";   // 甲
    two[0].role = "user";
    two[0].content = "\xE4\xB9\x99";   // 乙
    std::vector<llama_token> a, b;
    if (!tokenize_prompt(vocab, build_prompt(model, one, system_prompt, impl_->opt.thinking), &a) ||
        !tokenize_prompt(vocab, build_prompt(model, two, system_prompt, impl_->opt.thinking), &b)) {
        err = "could not tokenize the system prompt";
        return false;
    }
    std::size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) {
        ++n;
    }
    if (n == 0) {
        return true;   // nothing in common to compute
    }

    // Whatever of it the cache already holds -- the last turn's prompt began the
    // same way -- stays, and only the rest is computed. The same rewind as a
    // turn's, with the same fallback when the memory refuses it.
    std::size_t reuse = 0;
    const std::size_t max_reuse = std::min(impl_->cached.size(), n);
    while (reuse < max_reuse && impl_->cached[reuse] == a[reuse]) {
        ++reuse;
    }
    if (reuse == n) {
        return true;
    }
    llama_context* ctx = impl_->ctx;
    if (reuse > 0 &&
        !llama_memory_seq_rm(llama_get_memory(ctx), 0, static_cast<llama_pos>(reuse), -1)) {
        reuse = 0;
    }
    if (reuse == 0) {
        llama_memory_seq_rm(llama_get_memory(ctx), 0, -1, -1);
    }

    const clk::time_point t0 = clk::now();
    llama_batch batch = llama_batch_get_one(a.data() + reuse, static_cast<int>(n - reuse));
    if (llama_decode(ctx, batch) != 0) {
        impl_->drop_cache();
        err = "prefilling the system prompt failed";
        return false;
    }
    impl_->cached.assign(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(n));
    utils::log_info("local: warmed %zu prompt token(s) (%zu already held) in %.0f ms",
                    n, reuse, ms_since(t0));
    return true;
}

bool LocalClient::loaded() const { return impl_->model != nullptr; }

bool LocalClient::load(std::string& err) {
    // Under the turn lock: ensure_model() is exactly what a turn does first, and two
    // threads racing to read the same multi-GB file would load it twice.
    std::lock_guard<std::mutex> lock(impl_->turn);
    return impl_->ensure_model(err);
}

LocalStats LocalClient::stats() const { return impl_->stats; }

const LocalOptions& LocalClient::options() const { return impl_->opt; }

bool LocalClient::ainvoke(const std::vector<ChatMessage>& messages,
                          const std::string&              system_prompt,
                          const EventHandler&             on_event,
                          const UserContext&              /*user*/,
                          const RequestOptions&           req) {
    std::lock_guard<std::mutex> lock(impl_->turn);

    // Zeroed every turn, so load_ms is non-zero only on the turn that paid for it.
    impl_->stats = LocalStats();

    std::string err;
    if (!impl_->ensure_model(err)) {
        emit_event(on_event, EventType::Error, err);
        return false;
    }

    llama_model* model = impl_->model;
    const llama_vocab* vocab = llama_model_get_vocab(model);

    if (!impl_->ensure_context(err)) {
        emit_event(on_event, EventType::Error, err);
        return false;
    }
    llama_context* ctx = impl_->ctx;
    const int n_ctx = impl_->stats.n_ctx;

    /**
     * Fit the conversation into the window, shedding the OLDEST turns until the
     * prompt leaves n_reply_reserve tokens free for the answer.
     *
     * This has to live here rather than in the host because only this layer holds the
     * tokenizer -- no caller can count tokens, and character counts are not a usable
     * proxy across CJK and code. Turns are stateless (the whole history is replayed
     * every time), so without this the prompt grows every turn until it fills the
     * context; raising n_predict cannot help, it only moves where the wall is.
     *
     * Re-tokenizing per drop is O(messages^2) worst case, which is fine: the count is
     * tens, and tokenizing is microseconds against a decode measured in seconds. The
     * cheap alternative -- summing per-message counts -- is wrong, because the chat
     * template wraps the set, not each message.
     */
    const int reserve = impl_->opt.n_reply_reserve > 0 ? impl_->opt.n_reply_reserve : 1;
    const int fit_to  = n_ctx - reserve;

    std::string text;
    std::vector<llama_token> tokens;
    int n_prompt = 0;
    std::size_t drop = 0;

    while (true) {
        const std::vector<ChatMessage> window(
            messages.begin() + static_cast<std::ptrdiff_t>(drop), messages.end());
        text = build_prompt(model, window, system_prompt, impl_->opt.thinking);
        if (text.empty()) {
            impl_->drop_cache();
            emit_event(on_event, EventType::Error, "no user message to answer");
            return false;
        }

        n_prompt = -llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                                   nullptr, 0, /*add_special=*/true, /*parse_special=*/true);
        if (n_prompt <= 0) {
            impl_->drop_cache();
            emit_event(on_event, EventType::Error, "could not tokenize the prompt");
            return false;
        }
        tokens.resize(static_cast<std::size_t>(n_prompt));
        if (llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                           tokens.data(), n_prompt, true, true) < 0) {
            impl_->drop_cache();
            emit_event(on_event, EventType::Error, "could not tokenize the prompt");
            return false;
        }

        if (n_prompt <= fit_to) {
            break;                      // fits, with room reserved for the answer
        }
        if (drop + 1 >= messages.size()) {
            // Nothing left to shed: the current question alone is too long. Trimming
            // it would answer a different question, so refuse instead.
            impl_->drop_cache();
            emit_event(on_event, EventType::Error,
                       "this message is too long for the model's context window");
            return false;
        }
        ++drop;
        // Keep the window opening on a user turn: a conversation that starts with an
        // assistant reply reads as a non-sequitur to the model.
        while (drop + 1 < messages.size() && messages[drop].role != "user") {
            ++drop;
        }
    }
    impl_->stats.dropped_msgs  = static_cast<int>(drop);
    impl_->stats.prompt_tokens = n_prompt;

    // The context bound always applies -- a token past it fails llama_decode, so this
    // is physics, not policy. One token is reserved because the last generated token
    // is itself decoded (to prime the next step), which must still land inside the
    // window rather than exactly on its edge.
    //
    // n_predict only NARROWS this, and only when a caller asks.
    const int room = n_ctx - n_prompt - 1;
    const int budget = (impl_->opt.n_predict > 0 && impl_->opt.n_predict < room)
        ? impl_->opt.n_predict
        : room;
    if (budget <= 0) {
        impl_->drop_cache();
        emit_event(on_event, EventType::Error,
                   "no room left in the context window for a reply");
        return false;
    }

    /**
     * Sampler chain, in llama.cpp's prescribed order: penalties narrow the
     * distribution, then top-k/top-p truncate it, then temperature reshapes what is
     * left, then one token is drawn.
     *
     * The penalty is the part that matters. Greedy decoding always takes the most
     * likely token, so once a phrase becomes locally optimal the model emits it
     * forever; penalizing the last repeat_last_n tokens makes the second lap cheaper
     * to leave than to continue.
     *
     * REQUIRES a llama.cpp new enough that llama_sampler_init_penalties takes
     * n_vocab. Against an older checkout this is the ONE call in the file that fails
     * to compile, as "requires 5 arguments, but 4 were provided" -- which reads like
     * a bug here rather than a stale dependency, so the root CMakeLists checks the
     * header for it and says so plainly.
     */
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());

    /*
     * A grammar, when one was asked for, goes at the HEAD of the chain -- before
     * the penalties and before any truncation.
     *
     * Order is not a style choice here. The grammar works by masking every token
     * the syntax cannot accept to -INF; top-k and top-p work by keeping only the
     * best n candidates. Run top-k first and it can hand the grammar a shortlist
     * containing no legal token at all, at which point sampling has nothing to
     * choose and the reply breaks at exactly the moments the grammar was added to
     * prevent. Masking first, truncating second, always leaves a legal candidate.
     *
     * A malformed grammar returns NULL rather than aborting. That is reported and
     * the turn goes on unconstrained: the caller gets a reply it can see is wrong
     * plus a log line saying why, which beats a turn that silently never runs.
     */
    const bool structured = !req.grammar.empty();
    if (structured) {
        llama_sampler* gr =
            llama_sampler_init_grammar(vocab, req.grammar.c_str(), "root");
        if (gr != nullptr) {
            llama_sampler_chain_add(smpl, gr);
        } else {
            yoai::utils::log_warn(
                "on-device: grammar rejected by the parser, continuing unconstrained");
        }
    }

    /*
     * A structured turn is decoded GREEDILY and without a repetition penalty,
     * whatever the client's chat settings say. See RequestOptions::grammar for
     * the measurement behind this; the short version is that sampling a
     * classification at temperature 0.7 returns a different answer every run and
     * the wrong one most runs.
     *
     * The penalty has to go for a second reason. It demotes tokens seen in the
     * last repeat_last_n, and a scoring prompt necessarily SHOWS the model its
     * own scale -- "-1 very unpleasant, 0 neutral" -- so the penalty lands
     * squarely on the handful of tokens the grammar permits as the answer.
     */
    if (!structured &&
        impl_->opt.repeat_penalty > 1.0f && impl_->opt.repeat_last_n != 0) {
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab),
            impl_->opt.repeat_last_n, impl_->opt.repeat_penalty,
            /*penalty_freq=*/0.0f, /*penalty_present=*/0.0f));
    }
    if (structured || impl_->opt.temperature <= 0.0f) {
        // Deterministic on request -- useful for benchmarking, unsafe as a
        // default for prose, and mandatory for a grammar.
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        if (impl_->opt.top_k > 0) {
            llama_sampler_chain_add(smpl, llama_sampler_init_top_k(impl_->opt.top_k));
        }
        if (impl_->opt.top_p > 0.0f && impl_->opt.top_p < 1.0f) {
            llama_sampler_chain_add(smpl,
                llama_sampler_init_top_p(impl_->opt.top_p, /*min_keep=*/1));
        }
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(impl_->opt.temperature));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    }

    /*
     * Prefill only what the KV does not already hold.
     *
     * `cached` is the token sequence at positions [0, cached.size()). Whatever
     * the new prompt shares with it from the front is already computed and its
     * positions are unchanged, so the work is to drop the KV past the point
     * where they diverge and decode the remainder.
     *
     * One token is always left to decode. A decode produces logits for the
     * tokens it is given, so handing it an empty batch yields nothing to sample
     * and the turn stalls -- which is the case when a prompt is re-sent
     * verbatim, exactly the case caching is most likely to hit.
     *
     * Positions are not passed: llama_batch_get_one tracks them from the
     * sequence's current end, which after the removal below is precisely
     * `reuse`.
     *
     * **The removal can refuse, and its answer is not advisory.** llama.h:
     * "Returns false if a partial sequence cannot be removed. Removing a whole
     * sequence never fails" -- a memory that cannot be rewound to an arbitrary
     * position, a sliding-window or recurrent one, is where it says no. Dropping
     * that answer on the floor does not leave the cache alone: the old tokens
     * stay AND the new prompt is appended after them, because positions come
     * from the sequence's end. The model is then handed the previous
     * conversation followed by this one, which is exactly what "the new
     * conversation still remembers the last one" looks like from the outside --
     * the screen is clear, the prompt has one message in it, and the reply
     * answers a question nobody asked twice.
     *
     * So a refusal falls back to the removal that cannot fail, and the prompt is
     * prefilled whole. That costs the reuse and nothing else.
     */
    std::size_t reuse = 0;
    const std::size_t max_reuse =
        std::min(impl_->cached.size(), tokens.size() - 1);
    while (reuse < max_reuse && impl_->cached[reuse] == tokens[reuse]) {
        ++reuse;
    }
    if (reuse > 0 &&
        !llama_memory_seq_rm(llama_get_memory(ctx), 0,
                             static_cast<llama_pos>(reuse), -1)) {
        // The memory will not drop a tail. If the last prompt's snapshot is a
        // prefix of this one, go back to it rather than to nothing -- see
        // Impl::ckpt_state for why a conversation lands exactly there.
        const std::vector<llama_token>& ck = impl_->ckpt_tokens;
        const bool usable = !ck.empty() && ck.size() <= reuse &&
                            std::equal(ck.begin(), ck.end(), tokens.begin());
        llama_memory_seq_rm(llama_get_memory(ctx), 0, -1, -1);
        if (usable && llama_state_seq_set_data(ctx, impl_->ckpt_state.data(),
                                               impl_->ckpt_state.size(), 0) > 0) {
            utils::log_info("local: this memory will not rewind to %zu; restored the snapshot at %zu",
                            reuse, ck.size());
            reuse = ck.size();
        } else {
            utils::log_info(
                "local: this memory will not rewind to %zu, prefilling the whole prompt", reuse);
            reuse = 0;
        }
    }
    if (reuse == 0) {
        // The whole sequence, which llama.h promises never fails.
        llama_memory_seq_rm(llama_get_memory(ctx), 0, -1, -1);
    }

    // Where this prompt ends before its think suffix: the point the next turn's
    // prompt will part from the KV at. Only when the suffix is really the tail
    // of the tokens -- it is special tokens and newlines, so it tokenizes
    // cleanly on its own -- and only when there is prompt left to prefill up
    // to it.
    std::size_t ckpt_at = 0;
    {
        const std::string suffix = think_suffix(model, impl_->opt.thinking);
        std::vector<llama_token> tail;
        if (!suffix.empty() && tokenize_prompt_raw(vocab, suffix, &tail) &&
            tail.size() < tokens.size() &&
            std::equal(tail.begin(), tail.end(), tokens.end() - static_cast<std::ptrdiff_t>(tail.size()))) {
            ckpt_at = tokens.size() - tail.size();
        }
    }

    clk::time_point t0 = clk::now();
    // In two batches when a snapshot is due: up to the suffix, save, then the
    // rest. The first batch's logits go unused; only the last token's matter.
    std::size_t at = reuse;
    if (ckpt_at > reuse) {
        llama_batch head = llama_batch_get_one(tokens.data() + at, static_cast<int>(ckpt_at - at));
        if (llama_decode(ctx, head) != 0) {
            llama_sampler_free(smpl);
            impl_->drop_cache();
            emit_event(on_event, EventType::Error, "prefill failed");
            return false;
        }
        at = ckpt_at;
        const std::size_t n = llama_state_seq_get_size(ctx, 0);
        impl_->ckpt_state.resize(n);
        if (n > 0 && llama_state_seq_get_data(ctx, impl_->ckpt_state.data(), n, 0) == n) {
            impl_->ckpt_tokens.assign(tokens.begin(), tokens.begin() + static_cast<std::ptrdiff_t>(ckpt_at));
        } else {
            impl_->ckpt_tokens.clear();
            impl_->ckpt_state.clear();
        }
    }
    llama_batch batch = llama_batch_get_one(tokens.data() + at,
                                            static_cast<int>(tokens.size() - at));
    if (llama_decode(ctx, batch) != 0) {
        llama_sampler_free(smpl);
        impl_->drop_cache();
        emit_event(on_event, EventType::Error, "prefill failed");
        return false;
    }
    impl_->stats.prefill_ms = ms_since(t0);
    impl_->stats.reused_tokens = static_cast<int>(reuse);

    // The KV now holds the whole prompt. Generated tokens are appended below as
    // they are decoded, so this stays true for the next turn.
    impl_->cached = tokens;

    // Reasoning markers arrive inline in the raw text and must not reach the host as
    // a literal "<think>" -- route them to Thinking, which the app already renders as
    // a separate block for the remote lane.
    ThinkSplitter splitter;
    ThinkSplitter::Emit route =
        [&on_event](bool thinking, const std::string& piece) -> bool {
            return emit_event(on_event, thinking ? EventType::Thinking : EventType::Reply, piece);
        };

    // Bytes of a character whose remaining bytes are still in a later token. Sits
    // UPSTREAM of the splitter so it only ever sees whole characters.
    std::string utf8_carry;

    bool aborted = false;
    t0 = clk::now();
    for (int i = 0; i < budget; ++i) {
        const llama_token tok = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) {
            break;
        }

        char piece[512];
        const int n = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, true);
        if (n > 0) {
            utf8_carry.append(piece, static_cast<std::size_t>(n));
            const std::size_t whole = utf8_complete_len(utf8_carry);
            if (whole > 0) {
                if (!splitter.feed(utf8_carry.substr(0, whole), route)) {
                    aborted = true;   // the host asked to stop; not an error
                    break;
                }
                utf8_carry.erase(0, whole);
            }
        }
        // Stamped on the first token only, and before the forward that produces
        // the second. What it captures is everything between the end of prefill
        // and the first piece of text: the sample from the prefill logits, and
        // whatever llama.cpp does once when the batch size drops from the whole
        // prompt to one.
        if (impl_->stats.decoded_tokens == 0) {
            impl_->stats.first_token_ms = ms_since(t0);
        }
        ++impl_->stats.decoded_tokens;

        batch = llama_batch_get_one(const_cast<llama_token*>(&tok), 1);
        if (llama_decode(ctx, batch) != 0) {
            impl_->stats.decode_ms = ms_since(t0);
            llama_sampler_free(smpl);
            impl_->drop_cache();
            emit_event(on_event, EventType::Error, "decode failed mid-stream");
            return false;
        }
        // It is in the KV now, so it is part of what the next turn can reuse --
        // and in a conversation it will be, because the host replays the reply
        // as an assistant message and the template renders it identically.
        impl_->cached.push_back(tok);
    }
    impl_->stats.decode_ms = ms_since(t0);

    // Anything still held back was a partial tag that never completed, so it is
    // content -- dropping it would silently truncate the answer. `utf8_carry` is the
    // opposite case and is deliberately DISCARDED: a character the model never
    // finished emitting has no renderable form, and passing the stray bytes on is
    // exactly the replacement-glyph artifact this buffering exists to prevent.
    if (!aborted) {
        splitter.flush(route);
    }

    llama_sampler_free(smpl);
    // The cache is KEPT here. This is the normal end of a turn, not an error
    // path -- the KV holds the prompt and the reply, both of which the next
    // turn may share. Only the sampler was per-turn.
    if (!aborted) {
        report_turn(impl_->opt, impl_->stats, on_event);
    }
    return !aborted;
}

#else   // ---------------------------------------------------------------- stub

struct LocalClient::Impl {
    LocalOptions opt;
    LocalStats   stats;
    explicit Impl(LocalOptions o) : opt(std::move(o)) {}
};

bool LocalClient::available() { return false; }

LocalClient::LocalClient(LocalOptions opt)
    : Chat(Engine::LlamaCpp), impl_(new Impl(std::move(opt))) {}
LocalClient::~LocalClient() = default;

void LocalClient::set_threads(int) {}
void LocalClient::forget() {}

bool LocalClient::warm(const std::string&, std::string& err) {
    err = "this build has no on-device engine";
    return false;
}

bool LocalClient::loaded() const { return false; }

bool LocalClient::load(std::string& err) {
    err = "this build has no on-device engine";
    return false;
}

LocalStats LocalClient::stats() const { return impl_->stats; }

const LocalOptions& LocalClient::options() const { return impl_->opt; }

bool LocalClient::ainvoke(const std::vector<ChatMessage>&,
                          const std::string&,
                          const EventHandler& on_event,
                          const UserContext&,
                          const RequestOptions&) {
    emit_event(on_event, EventType::Error,
               "this build has no on-device engine "
               "(configure with -DYOAI_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=...)");
    return false;
}

#endif  // YOAI_ONDEVICE_LLM

}
}
