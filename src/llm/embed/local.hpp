#pragma once

// On-device text embedding: llama.cpp + a GGUF embedding model, no network.
// A local Embedder (llm/embed/embedder.hpp).
//
// Written for Qwen3-Embedding-0.6B: 1024 dimensions, last-token pooling. Any
// GGUF that llama.cpp pools works, and dim() reports what the model gives.
//
// EmbedKind::Query text is wrapped in the instruction Qwen3-Embedding was
// trained with (see qwen3_embedding_query); Document text goes in as it is.
// Leaving the instruction off queries costs retrieval quality without failing
// anything, which is why the wrapping happens here and not in every caller.
//
// Same build switch as llm/chat/local.hpp: YOAI_ONDEVICE_LLM. Without it a stub
// compiles in, available() is false and load() fails.
//
// Threading: embed() blocks for one forward pass over the text and is
// serialized per object. The caller owns threading.

#include "llm/backend.hpp"
#include "llm/embed/embedder.hpp"

#include <memory>
#include <string>
#include <vector>

namespace yoai { namespace llm {

struct LocalEmbedOptions {
    std::string model_path;   // absolute path to a .gguf

    // The longest text accepted, in tokens, EOS included. Longer text is
    // refused, not cut: the model pools the LAST token, and cutting the text
    // would drop the EOS the pooled vector is read from. The compute buffer
    // grows with this, since the whole text goes through in one batch.
    int n_ctx = 2048;

    // <= 0 means hardware_concurrency(). See LocalOptions::n_threads for why a
    // phone wants fewer.
    int n_threads = 0;

    // The instruction put in front of every Query. One sentence saying what
    // the search is for; the default fits the app's own memory search.
    std::string query_task =
        "Given a question about the user's day, retrieve the notes that answer it";
};

class LocalEmbedder : public Embedder {
public:
    /// True when compiled against llama.cpp (YOAI_ONDEVICE_LLM).
    static bool available();

    explicit LocalEmbedder(LocalEmbedOptions opt);
    ~LocalEmbedder() override;

    LocalEmbedder(const LocalEmbedder&)            = delete;
    LocalEmbedder& operator=(const LocalEmbedder&) = delete;

    /// Read the weights and build the context. Idempotent. Fails for a model
    /// llama.cpp does not pool, since that is not an embedding model.
    bool load(std::string& err) override;

    bool loaded() const override;

    void set_threads(int n) override;

    /// The vector length, once loaded; 0 before.
    int dim() const override;

    /// One text to one unit vector of dim() floats. Loads on first use.
    bool embed(const std::string& text, EmbedKind kind,
               std::vector<float>* out, std::string& err) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A query in the form Qwen3-Embedding was trained on:
///
///     Instruct: {task}\nQuery:{text}
std::string qwen3_embedding_query(const std::string& task, const std::string& text);

}
}
