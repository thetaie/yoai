#pragma once

// A model that turns text into a vector, for search by meaning.
//
// Implemented by LocalEmbedder (llama.cpp, llm/embed/local.hpp). A remote one
// behind an OpenAI-compatible /embeddings endpoint would be the second.
//
// Every implementation returns unit-length vectors of dim() floats.
// store/vector_index.hpp searches by L2 distance, and between unit vectors L2
// ranks exactly as cosine does.

#include "llm/backend.hpp"

#include <string>
#include <vector>

namespace yoai { namespace llm {

// Retrieval models embed the two sides differently. Qwen3-Embedding puts an
// instruction in front of a query and nothing in front of a document, and
// others have their own conventions. The kind is passed in rather than the
// prefix written by the caller, so code that stores and searches never needs
// to know which model it has.
enum class EmbedKind {
    Document,   // text that is stored and later found
    Query,      // text that is searched with
};

class Embedder : public Backend {
public:
    // The vector length. 0 until known, which for an on-device model is once
    // it is loaded.
    virtual int dim() const = 0;

    // One text to one unit vector. Blocks for one forward pass or one request.
    virtual bool embed(const std::string& text, EmbedKind kind,
                       std::vector<float>* out, std::string& err) = 0;

protected:
    explicit Embedder(Engine engine) : Backend(engine) {}
};

}
}
