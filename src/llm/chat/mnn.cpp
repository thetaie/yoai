#include "llm/chat/mnn.hpp"

#include "llm/chat/annotations.hpp"
#include "llm/chat/local.hpp"   // ThinkSplitter, utf8_complete_len
#include "utils/log.hpp"

#include <mutex>
#include <utility>

#ifdef YOAI_MNN
#include <llm/llm.hpp>

#include <chrono>
#include <cstdio>
#include <streambuf>
#endif

namespace yoai { namespace llm {

namespace {

bool emit(const EventHandler& on_event, EventType type, const std::string& content) {
    Event e;
    e.type    = type;
    e.content = content;
    return on_event ? on_event(e) : true;
}

}  // namespace

#ifdef YOAI_MNN

namespace {

using MNN::Transformer::Llm;
using MNN::Transformer::LlmContext;
using MNN::Transformer::LlmStatus;

using clk = std::chrono::steady_clock;

std::string with_slash(const std::string& dir) {
    if (dir.empty() || dir.back() == '/' || dir.back() == '\\') {
        return dir;
    }
    return dir + "/";
}

// A JSON string literal, for the handful of values this file puts into MNN's
// config. A path is the only one that can hold a quote or a backslash, and on
// Windows it always holds backslashes.
std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

/**
 * The ostream MNN writes the reply into, turned back into events.
 *
 * MNN streams by writing each decoded piece to a std::ostream, and a piece is a
 * token's bytes -- so, exactly as on the llama.cpp lane, half a character can
 * arrive at a time. The incomplete tail is held until the rest arrives, and
 * whole characters go through the same ThinkSplitter LocalClient uses.
 *
 * Cancelling is the one thing an ostream cannot say. MNN's decode loop checks
 * its context's status before every token, and USER_CANCEL makes it stop; so
 * when the handler returns false, on_cancel sets that status and the loop ends
 * at the next token.
 */
class EventStream : public std::streambuf {
public:
    EventStream(const EventHandler& on_event, std::function<void()> on_cancel)
        : on_event_(on_event), on_cancel_(std::move(on_cancel)) {
        route_ = [this](bool thinking, const std::string& text) {
            return emit(on_event_, thinking ? EventType::Thinking : EventType::Reply, text);
        };
    }

    bool cancelled() const { return cancelled_; }

    // Whatever the splitter still holds back was a partial tag that never
    // completed, so it is content. A partial CHARACTER is not: it has no
    // renderable form, and is dropped.
    void finish() {
        if (!cancelled_) {
            splitter_.flush(route_);
        }
    }

protected:
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            const char c = static_cast<char>(ch);
            write(&c, 1);
        }
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        write(s, static_cast<std::size_t>(n));
        return n;
    }

private:
    void write(const char* s, std::size_t n) {
        if (cancelled_) {
            return;
        }
        carry_.append(s, n);
        const std::size_t whole = utf8_complete_len(carry_);
        if (whole == 0) {
            return;
        }
        if (!splitter_.feed(carry_.substr(0, whole), route_)) {
            cancelled_ = true;
            on_cancel_();
        }
        carry_.erase(0, whole);
    }

    const EventHandler&   on_event_;
    std::function<void()> on_cancel_;
    ThinkSplitter::Emit   route_;
    ThinkSplitter         splitter_;
    std::string           carry_;
    bool                  cancelled_ = false;
};

// MNN's statuses that stop every later call: each entry point checks the status
// first and returns without doing anything.
bool stuck(LlmStatus s) {
    return s == LlmStatus::USER_CANCEL || s == LlmStatus::INTERNAL_ERROR ||
           s == LlmStatus::TIMEOUT;
}

}  // namespace

struct MnnClient::Impl {
    MnnOptions opt;
    std::unique_ptr<Llm, void (*)(Llm*)> llm{nullptr, &Llm::destroy};
    bool       is_loaded = false;
    std::mutex turn;

    explicit Impl(MnnOptions o) : opt(std::move(o)) {}

    // Under `turn`. A no-op once loaded.
    bool ensure_model(std::string& err) {
        if (is_loaded) {
            return true;
        }
        const std::string dir    = with_slash(opt.model_dir);
        const std::string config = dir + "config.json";
        const clk::time_point t0 = clk::now();

        llm.reset(Llm::createLLM(config));
        if (!llm) {
            err = "could not create an MNN engine from " + config;
            return false;
        }

        // Before load(): thread_num and has_talker are read while it builds the
        // runtime and the modules.
        //
        //   tmp_path       where MNN keeps its caches; it must be writable, and
        //                  inside the model directory the caller could read.
        //   has_talker     off: an omni export would otherwise load its Talker
        //                  -- gigabytes -- for speech this client never makes.
        //   prompt_cache   the prefix reuse described in the header.
        //   jinja.context  enable_thinking, for templates that read it.
        std::string cfg = "{";
        cfg += "\"tmp_path\":" + json_string(dir + "mnn-cache");
        cfg += ",\"thread_num\":" + std::to_string(opt.n_threads > 0 ? opt.n_threads : 4);
        cfg += ",\"max_new_tokens\":" + std::to_string(opt.n_predict);
        cfg += ",\"has_talker\":false";
        cfg += ",\"prompt_cache\":true";
        if (opt.thinking >= 0) {
            cfg += std::string(",\"jinja\":{\"context\":{\"enable_thinking\":") +
                   (opt.thinking > 0 ? "true" : "false") + "}}";
        }
        cfg += "}";
        llm->set_config(cfg);

        if (!llm->load()) {
            llm.reset();
            err = "could not load the MNN model in " + opt.model_dir;
            return false;
        }
        is_loaded = true;
        utils::log_info("mnn model loaded in %.0f ms: %s",
                           std::chrono::duration<double, std::milli>(clk::now() - t0).count(),
                           opt.model_dir.c_str());
        return true;
    }
};

bool MnnClient::available() { return true; }

MnnClient::MnnClient(MnnOptions opt)
    : Chat(Engine::Mnn), impl_(new Impl(std::move(opt))) {}
MnnClient::~MnnClient() = default;

bool MnnClient::load(std::string& err) {
    std::lock_guard<std::mutex> lock(impl_->turn);
    return impl_->ensure_model(err);
}

bool MnnClient::loaded() const { return impl_->is_loaded; }

void MnnClient::set_threads(int n) {
    std::lock_guard<std::mutex> lock(impl_->turn);   // never mid-turn
    impl_->opt.n_threads = n;
    if (impl_->is_loaded) {
        utils::log_info("mnn threads -> %d, applied at the next load", n);
    }
}

void MnnClient::forget() {
    std::lock_guard<std::mutex> lock(impl_->turn);
    if (impl_->llm) {
        impl_->llm->reset();
    }
}

const MnnOptions& MnnClient::options() const { return impl_->opt; }

bool MnnClient::ainvoke(const std::vector<ChatMessage>& messages,
                        const std::string&              system_prompt,
                        const EventHandler&             on_event,
                        const UserContext&              /*user*/,
                        const RequestOptions&           req) {
    std::lock_guard<std::mutex> lock(impl_->turn);

    if (!req.grammar.empty()) {
        // Said, not dropped: a caller that asked for a grammar is about to parse
        // the reply. See the header.
        utils::log_warn("mnn ignores the requested grammar; reply is unconstrained");
    }

    std::string err;
    if (!impl_->ensure_model(err)) {
        emit(on_event, EventType::Error, err);
        return false;
    }
    Llm* llm = impl_->llm.get();

    MNN::Transformer::ChatMessages chat;
    if (!system_prompt.empty()) {
        chat.push_back(std::make_pair(std::string("system"), system_prompt));
    }
    bool has_user = false;
    const std::vector<ChatMessage> annotated = with_annotation_text(messages);
    for (std::size_t i = 0; i < annotated.size(); ++i) {
        const ChatMessage& m = annotated[i];
        if (m.role == "user" || m.role == "assistant") {
            chat.push_back(std::make_pair(m.role, m.content));
            has_user = has_user || m.role == "user";
        }
    }
    if (!has_user) {
        emit(on_event, EventType::Error, "no user message to answer");
        return false;
    }

    // A cancelled or failed turn leaves a status that makes response() return
    // without doing anything, forever. reset() drops the KV and the prompt
    // cache, which after a cancel hold a reply nobody will replay; then
    // generate_init() is the one call that sets the status back to RUNNING.
    if (stuck(llm->getContext()->status)) {
        llm->reset();
        llm->generate_init(nullptr, nullptr);
    }

    EventStream stream(on_event, [llm]() {
        // The context is not const, only the accessor is. Writing the status
        // is how MNN's own callers stop a decode; see EventStream.
        const_cast<LlmContext*>(llm->getContext())->status = LlmStatus::USER_CANCEL;
    });
    std::ostream os(&stream);

    // "" rather than the default "\n", which MNN would append to every reply.
    llm->response(chat, &os, "", impl_->opt.n_predict);
    stream.finish();

    if (stream.cancelled()) {
        return false;   // the host asked to stop; not an error
    }
    const LlmContext* ctx = llm->getContext();
    if (ctx->status == LlmStatus::INTERNAL_ERROR || ctx->status == LlmStatus::TIMEOUT) {
        emit(on_event, EventType::Error,
             ctx->status == LlmStatus::TIMEOUT ? "the MNN engine timed out"
                                               : "the MNN engine failed mid-turn");
        return false;
    }

    // The model, not the entry: the directory name is what the user picked.
    std::string name = impl_->opt.model_dir;
    while (!name.empty() && (name.back() == '/' || name.back() == '\\')) {
        name.pop_back();
    }
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }

    Event usage;
    usage.type               = EventType::CostStatistics;
    usage.cost.model         = name;
    usage.cost.input_tokens  = ctx->prompt_len;
    usage.cost.output_tokens = ctx->gen_seq_len;
    usage.cost.total_tokens  = ctx->prompt_len + ctx->gen_seq_len;
    usage.cost.total_cost    = 0.0;   // nobody was billed; the tokens still matter
    if (on_event) {
        on_event(usage);
    }

    // prompt is what was prefilled THIS turn: with the prompt cache, the part
    // of the transcript that differs from the last turn's.
    const double prefill_ms = ctx->prefill_us / 1000.0;
    const double decode_ms  = ctx->decode_us / 1000.0;
    utils::log_info(
        "mnn turn: %d prompt + %d decoded tokens, threads %d, "
        "prefill %.0fms (%.1f tok/s) decode %.0fms (%.1f tok/s)",
        ctx->prompt_len, ctx->gen_seq_len, impl_->opt.n_threads,
        prefill_ms, prefill_ms > 0 ? ctx->prompt_len * 1000.0 / prefill_ms : 0.0,
        decode_ms, decode_ms > 0 ? ctx->gen_seq_len * 1000.0 / decode_ms : 0.0);
    return true;
}

#else   // ---------------------------------------------------------------- stub

struct MnnClient::Impl {
    MnnOptions opt;
    explicit Impl(MnnOptions o) : opt(std::move(o)) {}
};

bool MnnClient::available() { return false; }

MnnClient::MnnClient(MnnOptions opt)
    : Chat(Engine::Mnn), impl_(new Impl(std::move(opt))) {}
MnnClient::~MnnClient() = default;

bool MnnClient::load(std::string& err) {
    err = "this build has no MNN engine";
    return false;
}

bool MnnClient::loaded() const { return false; }
void MnnClient::set_threads(int) {}
void MnnClient::forget() {}

const MnnOptions& MnnClient::options() const { return impl_->opt; }

bool MnnClient::ainvoke(const std::vector<ChatMessage>&,
                        const std::string&,
                        const EventHandler& on_event,
                        const UserContext&,
                        const RequestOptions&) {
    emit(on_event, EventType::Error,
         "this build has no MNN engine (configure with -DYOAI_MNN=ON -DMNN_DIR=...)");
    return false;
}

#endif  // YOAI_MNN

}
}
