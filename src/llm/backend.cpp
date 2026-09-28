#include "llm/backend.hpp"

#ifdef YOAI_ONDEVICE_LLM
#include "llama.h"

#include <cstdio>
#include <mutex>
#endif

#ifdef YOAI_MNN_VULKAN
// Declared here rather than in an MNN header, because it is not part of MNN's
// API: it exists only to be referenced. See start_mnn().
extern "C" void MNNForceLinkVulkanRuntime();
#endif

namespace yoai { namespace llm {

namespace {

void start_llama_cpp() {
#ifdef YOAI_ONDEVICE_LLM
    static std::once_flag once;
    std::call_once(once, []() {
        // ggml reports every tensor at INFO, which on a device buries
        // everything else in the log window. Errors go to stderr rather than to
        // utils/log.cpp's sink: they are llama.cpp's words, not ours.
        llama_log_set([](ggml_log_level level, const char* text, void*) {
            if (level >= GGML_LOG_LEVEL_ERROR && text != nullptr) {
                std::fputs(text, stderr);
            }
        }, nullptr);
        llama_backend_init();
    });
#endif
}

void start_mnn() {
#ifdef YOAI_MNN_VULKAN
    // The call does nothing; REFERENCING it is the point. A static libMNN
    // leaves the Vulkan registrar's object out of the link, and the only
    // symptom is one line of MNN's own output saying it fell back to the CPU.
    // It has to happen before createLLM asks for the backend. voice/omni does
    // the same for its own engine; see mnn-vulkan-static-register.patch.
    MNNForceLinkVulkanRuntime();
#endif
}

}  // namespace

Backend::Backend(Engine engine) {
    switch (engine) {
        case Engine::LlamaCpp: start_llama_cpp(); break;
        case Engine::Mnn:      start_mnn();       break;
        case Engine::Remote:   break;
    }
}

}
}
