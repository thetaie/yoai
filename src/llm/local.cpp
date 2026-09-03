#include "llm/local.hpp"

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

/**
 * One-time llama.cpp startup, safe to call from anywhere.
 *
 * The log hook is the point: ggml reports every tensor at INFO, which on a device
 * buries everything else in the log window. Errors still go to stderr, where
 * platform/log.cpp's own sink is not appropriate -- these are llama.cpp's words,
 * not ours.
 */
void ensure_backend() {
    static std::once_flag once;
    std::call_once(once, []() {
        llama_log_set([](ggml_log_level level, const char* text, void*) {
            if (level >= GGML_LOG_LEVEL_ERROR && text != nullptr) {
                std::fputs(text, stderr);
            }
        }, nullptr);
        llama_backend_init();
    });
}

using clk = std::chrono::steady_clock;

double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
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
    std::vector<ChatMessage> msgs;
    msgs.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const ChatMessage& m = messages[i];
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
                // Only for a model whose own template speaks this protocol --
                // appending <think> to anything else would just be prompt noise.
                if (thinking >= 0 && std::strstr(tmpl, "<think>") != nullptr) {
                    out += (thinking > 0) ? "<think>\n" : "<think>\n\n</think>\n\n";
                }
                return out;
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

#endif   // YOAI_ONDEVICE_LLM

bool emit_event(const EventHandler& on_event, EventType type, const std::string& content) {
    Event e;
    e.type    = type;
    e.content = content;
    return on_event ? on_event(e) : true;
}

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

    explicit Impl(LocalOptions o) : opt(std::move(o)) { ensure_backend(); }

    ~Impl() {
        if (model != nullptr) {
            llama_model_free(model);
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
};

bool LocalClient::available() { return true; }

LocalClient::LocalClient(LocalOptions opt) : impl_(new Impl(std::move(opt))) {}
LocalClient::~LocalClient() = default;

void LocalClient::setThreads(int n) {
    std::lock_guard<std::mutex> lock(impl_->turn);   // never mid-turn
    impl_->opt.n_threads = n;
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
                          const UserContext&              /*user*/) {
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

    int n_threads = impl_->opt.n_threads;
    if (n_threads <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        n_threads = hw > 0 ? static_cast<int>(hw) : 4;
    }

    /**
     * Take the largest context that both the model supports and the device can
     * actually give us, walking down from n_ctx_max.
     *
     * The rungs halve, so this costs at most a few failed allocations once per turn,
     * and llama_init_from_model returning null is the only trustworthy signal that a
     * size does not fit (see LocalOptions::n_ctx_max for why arithmetic is not an
     * option).
     */
    llama_context_params cp = llama_context_default_params();
    cp.n_ubatch        = 512;   // physical batch: what sizes the compute buffer
    cp.n_threads       = n_threads;
    cp.n_threads_batch = n_threads;
    impl_->stats.n_threads = n_threads;

    const int trained = llama_model_n_ctx_train(model);
    int want = impl_->opt.n_ctx_max;
    if (trained > 0 && trained < want) {
        want = trained;                  // never ask for more than it was trained on
    }

    // Fresh context per turn: history is replayed from `messages`, so there is no KV
    // state worth carrying, and a stale one would silently grow unbounded.
    //
    // n_batch tracks n_ctx because the prefill below hands the WHOLE prompt to one
    // llama_decode, and the prompt is fitted to `n_ctx - n_reply_reserve`. n_batch is
    // the logical batch -- the most tokens a single decode may take -- and llama.cpp
    // enforces it with an assert that aborts the PROCESS rather than returning an
    // error the caller could report. A fixed 512 there means any conversation whose
    // prompt passes 512 tokens takes the app down on a real device. The physical
    // batch (n_ubatch, above) is what actually costs memory; llama.cpp splits the
    // logical batch into n_ubatch chunks itself, so this stays free.
    llama_context* ctx = nullptr;
    for (int c = want; c >= 1024; c /= 2) {
        cp.n_ctx   = static_cast<uint32_t>(c);
        cp.n_batch = static_cast<uint32_t>(c);
        ctx = llama_init_from_model(model, cp);
        if (ctx != nullptr) {
            break;
        }
    }
    if (ctx == nullptr) {
        emit_event(on_event, EventType::Error, "could not create a context for this model");
        return false;
    }
    const int n_ctx = static_cast<int>(llama_n_ctx(ctx));
    impl_->stats.n_ctx = n_ctx;

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
            llama_free(ctx);
            emit_event(on_event, EventType::Error, "no user message to answer");
            return false;
        }

        n_prompt = -llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                                   nullptr, 0, /*add_special=*/true, /*parse_special=*/true);
        if (n_prompt <= 0) {
            llama_free(ctx);
            emit_event(on_event, EventType::Error, "could not tokenize the prompt");
            return false;
        }
        tokens.resize(static_cast<std::size_t>(n_prompt));
        if (llama_tokenize(vocab, text.c_str(), static_cast<int>(text.size()),
                           tokens.data(), n_prompt, true, true) < 0) {
            llama_free(ctx);
            emit_event(on_event, EventType::Error, "could not tokenize the prompt");
            return false;
        }

        if (n_prompt <= fit_to) {
            break;                      // fits, with room reserved for the answer
        }
        if (drop + 1 >= messages.size()) {
            // Nothing left to shed: the current question alone is too long. Trimming
            // it would answer a different question, so refuse instead.
            llama_free(ctx);
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
        llama_free(ctx);
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
    if (impl_->opt.repeat_penalty > 1.0f && impl_->opt.repeat_last_n != 0) {
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab),
            impl_->opt.repeat_last_n, impl_->opt.repeat_penalty,
            /*penalty_freq=*/0.0f, /*penalty_present=*/0.0f));
    }
    if (impl_->opt.temperature <= 0.0f) {
        // Deterministic on request -- useful for benchmarking, unsafe as a default.
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

    clk::time_point t0 = clk::now();
    llama_batch batch = llama_batch_get_one(tokens.data(), static_cast<int>(tokens.size()));
    if (llama_decode(ctx, batch) != 0) {
        llama_sampler_free(smpl);
        llama_free(ctx);
        emit_event(on_event, EventType::Error, "prefill failed");
        return false;
    }
    impl_->stats.prefill_ms = ms_since(t0);

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
        ++impl_->stats.decoded_tokens;

        batch = llama_batch_get_one(const_cast<llama_token*>(&tok), 1);
        if (llama_decode(ctx, batch) != 0) {
            impl_->stats.decode_ms = ms_since(t0);
            llama_sampler_free(smpl);
            llama_free(ctx);
            emit_event(on_event, EventType::Error, "decode failed mid-stream");
            return false;
        }
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
    llama_free(ctx);
    return !aborted;
}

#else   // ---------------------------------------------------------------- stub

struct LocalClient::Impl {
    LocalOptions opt;
    LocalStats   stats;
    explicit Impl(LocalOptions o) : opt(std::move(o)) {}
};

bool LocalClient::available() { return false; }

LocalClient::LocalClient(LocalOptions opt) : impl_(new Impl(std::move(opt))) {}
LocalClient::~LocalClient() = default;

void LocalClient::setThreads(int) {}
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
                          const UserContext&) {
    emit_event(on_event, EventType::Error,
               "this build has no on-device engine "
               "(configure with -DYOAI_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=...)");
    return false;
}

#endif  // YOAI_ONDEVICE_LLM

}
}
