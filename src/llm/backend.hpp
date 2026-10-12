#pragma once

// What every model in this directory has, wherever it runs: a way to get it
// ready, a way to ask whether it is, and a thread count.
//
// Callers hold a Chat or an Embedder and never ask whether it runs on this
// device or behind an HTTP API. The difference is entirely in how these three
// behave:
//
//                  on-device (llama.cpp)            remote (HTTP API)
//   load()         reads the weights; seconds       nothing to read; succeeds
//   loaded()       true once the weights are in     always true
//   set_threads()  CPU threads from the next call   ignored
//
// load() never touches the network. Whether an endpoint answers and whether
// the key is good is found out by using it: a probe costs a request and proves
// nothing about the next one.
//
// Errors are returned, never thrown: everything here sits under a C ABI.

#include <string>

namespace yoai { namespace llm {

class Backend {
public:
    // What runs the model. Whether it is on this device follows from this --
    // everything but Remote is -- so there is no separate flag for it.
    enum class Engine {
        LlamaCpp,   // a GGUF on this device
        Mnn,        // an MNN export on this device: a directory with config.json
        Remote,     // behind an HTTP API
    };

    virtual ~Backend() = default;

    // Make the model ready. Idempotent, and optional: a model that is not
    // loaded loads on first use. Calling it early moves the wait to a moment
    // when nobody is waiting, e.g. while the user is still typing.
    virtual bool load(std::string& err) = 0;

    virtual bool loaded() const = 0;

    // <= 0 means one per hardware thread. Never reloads anything. llama.cpp
    // applies it from the next call; MNN builds its runtime with a thread count
    // at load, so there it applies from the next load.
    virtual void set_threads(int n) = 0;

protected:
    // Starts the engine's process-wide state, once per process, so no model
    // can reach its engine before it is up. For LlamaCpp that is its log routed
    // to stderr, errors only, and the library initialized; a no-op in a build
    // without YOAI_ONDEVICE_LLM. For Mnn it is keeping the Vulkan backend in a
    // static link, when there is one. Remote has nothing to start.
    //
    // That is all `engine` decides. The three methods above stay pure for
    // every engine: a default could only be the remote one, and a local model
    // that forgot to override load() would then report itself loaded with no
    // weights read -- compiling fine, and stalling on first use instead.
    explicit Backend(Engine engine);
};

}
}
