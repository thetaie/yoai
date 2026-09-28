#include "llm/embed/local.hpp"

#include <mutex>
#include <utility>

#ifdef YOAI_ONDEVICE_LLM
#include "llama.h"

#include <cmath>
#include <thread>
#endif

namespace yoai { namespace llm {

std::string qwen3_embedding_query(const std::string& task, const std::string& text) {
    // No space after "Query:". That is how the model card writes it, and the
    // model was trained on that exact string.
    return "Instruct: " + task + "\nQuery:" + text;
}

#ifdef YOAI_ONDEVICE_LLM

struct LocalEmbedder::Impl {
    LocalEmbedOptions opt;
    llama_model*      model = nullptr;
    llama_context*    ctx   = nullptr;
    int               dim   = 0;
    std::mutex        lock;

    explicit Impl(LocalEmbedOptions o) : opt(std::move(o)) {}

    ~Impl() {
        if (ctx != nullptr) {
            llama_free(ctx);
        }
        if (model != nullptr) {
            llama_model_free(model);
        }
    }

    int threads() const {
        if (opt.n_threads > 0) {
            return opt.n_threads;
        }
        const unsigned hw = std::thread::hardware_concurrency();
        return hw > 0 ? static_cast<int>(hw) : 4;
    }

    // Under `lock`. A no-op once loaded.
    bool ensure_loaded(std::string& err) {
        if (ctx != nullptr) {
            return true;
        }
        if (model == nullptr) {
            llama_model_params mp = llama_model_default_params();
            model = llama_model_load_from_file(opt.model_path.c_str(), mp);
            if (model == nullptr) {
                err = "could not load model: " + opt.model_path;
                return false;
            }
        }

        const int n_threads = threads();

        llama_context_params cp = llama_context_default_params();
        cp.embeddings      = true;
        // One sequence, all of it in one batch: a pooled embedding is read
        // after the whole text has gone through, so the text cannot be split
        // across decode calls.
        cp.n_ctx           = static_cast<uint32_t>(opt.n_ctx);
        cp.n_batch         = static_cast<uint32_t>(opt.n_ctx);
        cp.n_ubatch        = static_cast<uint32_t>(opt.n_ctx);
        cp.n_seq_max       = 1;
        cp.n_threads       = n_threads;
        cp.n_threads_batch = n_threads;
        // UNSPECIFIED means the model's own, which the GGUF records. For
        // Qwen3-Embedding that is LAST.
        cp.pooling_type    = LLAMA_POOLING_TYPE_UNSPECIFIED;

        ctx = llama_init_from_model(model, cp);
        if (ctx == nullptr) {
            err = "could not create an embedding context";
            return false;
        }
        // `enum` is required: llama.h names the type and the function alike.
        const enum llama_pooling_type pooling = llama_pooling_type(ctx);
        if (pooling == LLAMA_POOLING_TYPE_NONE || pooling == LLAMA_POOLING_TYPE_RANK) {
            llama_free(ctx);
            ctx = nullptr;
            err = "not an embedding model (it does not pool): " + opt.model_path;
            return false;
        }
        dim = llama_model_n_embd_out(model);
        return true;
    }
};

bool LocalEmbedder::available() { return true; }

LocalEmbedder::LocalEmbedder(LocalEmbedOptions opt)
    : Embedder(Engine::LlamaCpp), impl_(new Impl(std::move(opt))) {}
LocalEmbedder::~LocalEmbedder() = default;

bool LocalEmbedder::load(std::string& err) {
    std::lock_guard<std::mutex> lock(impl_->lock);
    return impl_->ensure_loaded(err);
}

bool LocalEmbedder::loaded() const { return impl_->ctx != nullptr; }

void LocalEmbedder::set_threads(int n) {
    std::lock_guard<std::mutex> lock(impl_->lock);   // never mid-pass
    impl_->opt.n_threads = n;
    if (impl_->ctx != nullptr) {
        const int t = impl_->threads();
        llama_set_n_threads(impl_->ctx, t, t);
    }
}

int LocalEmbedder::dim() const { return impl_->dim; }

bool LocalEmbedder::embed(const std::string& text, EmbedKind kind,
                          std::vector<float>* out, std::string& err) {
    std::lock_guard<std::mutex> lock(impl_->lock);
    out->clear();
    if (!impl_->ensure_loaded(err)) {
        return false;
    }

    llama_context*     ctx   = impl_->ctx;
    const llama_vocab* vocab = llama_model_get_vocab(impl_->model);

    const std::string input = kind == EmbedKind::Query
                            ? qwen3_embedding_query(impl_->opt.query_task, text)
                            : text;

    // add_special: the GGUF says whether to append EOS, and for this model it
    // does. parse_special off: a user who types "<|endoftext|>" gets those
    // characters embedded, not a control token.
    const int32_t len = static_cast<int32_t>(input.size());
    int32_t n = -llama_tokenize(vocab, input.c_str(), len, nullptr, 0, true, false);
    std::vector<llama_token> tokens(static_cast<size_t>(n > 0 ? n : 0) + 1);
    n = llama_tokenize(vocab, input.c_str(), len, tokens.data(),
                       static_cast<int32_t>(tokens.size()), true, false);
    if (n < 0) {
        err = "tokenize failed";
        return false;
    }
    tokens.resize(static_cast<size_t>(n));

    // Last-token pooling reads the vector off the final token, and the model
    // was trained with that token being EOS. A GGUF converted without
    // add_eos_token would pool the last word instead, and nothing would fail --
    // the vectors would just be worse. So make sure of it here.
    if (llama_pooling_type(ctx) == LLAMA_POOLING_TYPE_LAST &&
        (tokens.empty() || tokens.back() != llama_vocab_eos(vocab))) {
        tokens.push_back(llama_vocab_eos(vocab));
    }

    if (static_cast<int>(tokens.size()) > impl_->opt.n_ctx) {
        err = "text is " + std::to_string(tokens.size()) + " tokens; the limit is " +
              std::to_string(impl_->opt.n_ctx);
        return false;
    }

    // Each text is its own sequence 0, so nothing from the last one may remain.
    llama_memory_clear(llama_get_memory(ctx), true);

    // No output flags: with embeddings on, a null array means every token is
    // output, which is what pooling reads from.
    llama_batch batch = llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()));
    if (llama_decode(ctx, batch) != 0) {
        err = "decode failed";
        return false;
    }

    const float* v = llama_get_embeddings_seq(ctx, 0);
    if (v == nullptr) {
        err = "no pooled embedding";
        return false;
    }

    double sum = 0;
    for (int i = 0; i < impl_->dim; ++i) {
        sum += static_cast<double>(v[i]) * v[i];
    }
    if (sum <= 0) {
        err = "the embedding is all zeros";
        return false;
    }
    const float scale = static_cast<float>(1.0 / std::sqrt(sum));
    out->resize(static_cast<size_t>(impl_->dim));
    for (int i = 0; i < impl_->dim; ++i) {
        (*out)[static_cast<size_t>(i)] = v[i] * scale;
    }
    return true;
}

#else   // ---------------------------------------------------------------- stub

struct LocalEmbedder::Impl {
    LocalEmbedOptions opt;
    explicit Impl(LocalEmbedOptions o) : opt(std::move(o)) {}
};

bool LocalEmbedder::available() { return false; }

LocalEmbedder::LocalEmbedder(LocalEmbedOptions opt)
    : Embedder(Engine::LlamaCpp), impl_(new Impl(std::move(opt))) {}
LocalEmbedder::~LocalEmbedder() = default;

bool LocalEmbedder::load(std::string& err) {
    err = "this build has no on-device engine";
    return false;
}

bool LocalEmbedder::loaded() const { return false; }

void LocalEmbedder::set_threads(int) {}

int LocalEmbedder::dim() const { return 0; }

bool LocalEmbedder::embed(const std::string&, EmbedKind,
                          std::vector<float>* out, std::string& err) {
    out->clear();
    err = "this build has no on-device engine "
          "(configure with -DYOAI_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=...)";
    return false;
}

#endif  // YOAI_ONDEVICE_LLM

}
}
