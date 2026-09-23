#include "voice/qtts.hpp"

#include "platform/log.hpp"

#include <chrono>
#include <string>

#ifdef BODYCLOCK_QTTS
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <qwen.h>

#ifdef BODYCLOCK_QTTS_VULKAN_PROBE
#    include <vulkan/vulkan.h>
#endif
#endif

namespace bodyclock {
namespace voice {

// 24 kHz, because that is the codec's rate and nothing in a float buffer says
// so. A player told 16000 plays this a third too slow, which sounds like a
// model problem and is not one.
int Qtts::sample_rate() {
    return 24000;
}

#ifdef BODYCLOCK_QTTS

namespace {

// steady_clock, not clock(). The same trap omni fell into: POSIX clock() sums
// processor time across threads, and this engine runs four of them, so a ratio
// built on it is four times too pessimistic and decides the wrong way.
typedef std::chrono::steady_clock clk;

double seconds_since(clk::time_point start) {
    return std::chrono::duration_cast<std::chrono::duration<double> >(clk::now() - start).count();
}

// qwentts.cpp writes to stderr by default, which on a phone is nowhere. This
// puts its load and synthesis cadence in the same log as everything else.
//
// Process-wide rather than per handle, which is upstream's design and worth
// knowing: installing it in load() means it outlives unload(). Harmless, since
// nothing else in this process logs through qwen.
void forward_log(qt_log_level level, const char* msg, void* /*user_data*/) {
    if (!msg) {
        return;
    }
    switch (level) {
        case QT_LOG_ERROR: platform::log_error("qtts: %s", msg); break;
        case QT_LOG_WARN:  platform::log_warn("qtts: %s", msg);  break;
        case QT_LOG_INFO:  platform::log_info("qtts: %s", msg);  break;
        default:           platform::log_debug("qtts: %s", msg); break;
    }
}

// setenv with "leave what is already there", spelled for three compilers.
// MSVC has no setenv at all, and _putenv_s always overwrites, so the part that
// makes an outside override win has to be done by hand on that side.
void set_env_default(const char* name, const char* value) {
    if (std::getenv(name)) {
        return;
    }
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Taking a variable OUT of the environment, which is a different operation from
// setting it to "0" and the only one that works on the three below: ggml reads
// two of them for PRESENCE -- `prefer_host_memory = getenv(...) != nullptr` --
// so "0" is still a yes. An empty value is how the Windows CRT spells removal.
void unset_env(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// The thread count, handed to qwentts.cpp the only way it takes one.
//
// An environment variable is a poor interface and it is the interface that
// exists: qwen.h has no thread parameter, and the count is read once inside
// qt_init. harmony/patches/qwentts-thread-count-override.patch is what makes
// this variable mean anything; without it this is a no-op, which is the right
// behaviour for a build made from an unpatched checkout.
//
// n <= 0 REMOVES it rather than writing a 0, so "leave it alone" and "use one
// thread" stay different requests.
const char* const kThreadsEnv = "QWEN_N_THREADS";

void apply_thread_env(int n) {
#if defined(_WIN32)
    char value[16];
    std::snprintf(value, sizeof(value), "%d", n > 0 ? n : 0);
    _putenv_s(kThreadsEnv, n > 0 ? value : "");
#else
    if (n > 0) {
        char value[16];
        std::snprintf(value, sizeof(value), "%d", n);
        setenv(kThreadsEnv, value, 1);
    } else {
        unsetenv(kThreadsEnv);
    }
#endif
}

// A RIFF/WAVE reader narrow enough to be read in one sitting: mono, 16-bit PCM,
// exactly 24000 Hz, and a refusal with a reason for anything else.
//
// Strict rather than accommodating, and that is the same decision the
// recognition lane made at 16 kHz: the core carries no resampler, because one
// that differs from the resampler a measurement was taken with changes the
// answer quietly. Downmixing stereo would be the same kind of silent edit.
//
// Chunks are walked rather than assumed at fixed offsets. A WAV written by a
// tool that inserts LIST or fact ahead of data is ordinary, and reading `data`
// from offset 36 would hand the model a header as audio.
bool read_reference_wav(const std::string& path, std::vector<float>* out, std::string* err) {
    out->clear();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        *err = "cannot open " + path;
        return false;
    }

    unsigned char header[12];
    if (std::fread(header, 1, 12, f) != 12 ||
        std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        *err = "not a RIFF/WAVE file";
        std::fclose(f);
        return false;
    }

    int  rate = 0, channels = 0, bits = 0, format = 0;
    bool have_fmt = false;

    for (;;) {
        unsigned char chunk[8];
        if (std::fread(chunk, 1, 8, f) != 8) {
            *err = have_fmt ? "no data chunk" : "no fmt chunk";
            std::fclose(f);
            return false;
        }
        const unsigned long size =
            static_cast<unsigned long>(chunk[4]) | (static_cast<unsigned long>(chunk[5]) << 8) |
            (static_cast<unsigned long>(chunk[6]) << 16) | (static_cast<unsigned long>(chunk[7]) << 24);

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (size < 16 || std::fread(fmt, 1, 16, f) != 16) {
                *err = "truncated fmt chunk";
                std::fclose(f);
                return false;
            }
            format   = fmt[0] | (fmt[1] << 8);
            channels = fmt[2] | (fmt[3] << 8);
            rate     = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            bits     = fmt[14] | (fmt[15] << 8);
            have_fmt = true;
            // Past whatever else this fmt carries, plus the pad byte an
            // odd-sized chunk is followed by.
            std::fseek(f, static_cast<long>(size - 16) + static_cast<long>(size & 1), SEEK_CUR);
            continue;
        }

        if (std::memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt) {
                *err = "data chunk before fmt";
                std::fclose(f);
                return false;
            }
            if (format != 1 || bits != 16 || channels != 1 || rate != Qtts::sample_rate()) {
                char why[160];
                std::snprintf(why, sizeof(why),
                              "need mono 16-bit PCM at %d Hz; this is %d channel(s), "
                              "%d-bit, %d Hz, format %d",
                              Qtts::sample_rate(), channels, bits, rate, format);
                *err = why;
                std::fclose(f);
                return false;
            }

            const size_t count = static_cast<size_t>(size / 2);
            std::vector<short> pcm(count);
            if (count && std::fread(&pcm[0], 2, count, f) != count) {
                *err = "truncated data chunk";
                std::fclose(f);
                return false;
            }
            std::fclose(f);

            out->resize(count);
            for (size_t i = 0; i < count; ++i) {
                (*out)[i] = static_cast<float>(pcm[i]) / 32768.0f;
            }
            return true;
        }

        std::fseek(f, static_cast<long>(size) + static_cast<long>(size & 1), SEEK_CUR);
    }
}

#ifdef BODYCLOCK_QTTS_VULKAN
#ifdef BODYCLOCK_QTTS_VULKAN_PROBE
// What kind of memory does this machine's GPU have?
//
// THREE answers and not two, because "the probe failed" and "the GPU shares
// system memory" are the same word in a boolean and cost thirty times different
// amounts to confuse. That confusion is exactly what happened: a probe that
// returned false silently on the first load of a process put a gigabyte of
// weights in system RAM, and since ggml reads the environment only when it
// CREATES a device, the correct answers from every later load arrived too late
// to move them.
enum class GpuMemory {
    Shared,     // integrated: the GPU reads the DRAM the CPU does
    Dedicated,  // a discrete card, with memory of its own across a bus
    Unknown     // the probe could not say, and says which way it failed
};

// Asked with a throwaway instance rather than of ggml, because ggml reads the
// three variables below when it CREATES a device and never again, so the answer
// is needed before there is a ggml device to ask. An instance the loader throws
// away again costs a few milliseconds against a model load of a second.
//
// "Any discrete device present" makes the answer Dedicated, rather than asking
// which device ggml will pick, because ggml picks later and by the same rule --
// its device list puts discrete ahead of integrated and it takes the first GPU.
// On a machine with both, both of us say discrete.
GpuMemory vulkan_gpu_memory() {
    VkApplicationInfo app{};
    app.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    VkInstance     instance = VK_NULL_HANDLE;
    const VkResult rc       = vkCreateInstance(&ci, nullptr, &instance);
    if (rc != VK_SUCCESS) {
        platform::log_warn("qtts: GPU probe: vkCreateInstance failed (%d)", static_cast<int>(rc));
        return GpuMemory::Unknown;
    }

    uint32_t       count = 0;
    const VkResult n_rc  = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (n_rc != VK_SUCCESS || count == 0) {
        platform::log_warn("qtts: GPU probe: no physical devices (%d, count %u)",
                           static_cast<int>(n_rc), count);
        vkDestroyInstance(instance, nullptr);
        return GpuMemory::Unknown;
    }

    std::vector<VkPhysicalDevice> devices(count);
    const VkResult                e_rc = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (e_rc != VK_SUCCESS) {
        platform::log_warn("qtts: GPU probe: enumerate failed (%d)", static_cast<int>(e_rc));
        vkDestroyInstance(instance, nullptr);
        return GpuMemory::Unknown;
    }

    bool discrete   = false;
    bool integrated = false;
    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        platform::log_info("qtts: GPU probe: %s (type %d)", props.deviceName,
                           static_cast<int>(props.deviceType));
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            discrete = true;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            integrated = true;
        }
    }
    vkDestroyInstance(instance, nullptr);

    // A CPU or virtual implementation -- SwiftShader, lavapipe -- is neither,
    // and reaches Unknown rather than Shared. The tuning is about a real GPU's
    // memory and has never been measured against a software one.
    if (discrete) {
        return GpuMemory::Dedicated;
    }
    return integrated ? GpuMemory::Shared : GpuMemory::Unknown;
}
#endif

// The Vulkan backend's environment, set before the engine builds a backend.
//
// **All three are for a GPU that shares system memory**, and all three are
// wrong on one that does not -- so a discrete card is asked about first and
// gets ggml's own defaults. The one measured here cost 89.6 ms a frame on an
// RTX 3090 for a 1.7B `Q4_K_M` talker, against 44.8 on a laptop's Arc iGPU:
// PREFER_HOST_MEMORY had put a gigabyte of weights and 896 MB of KV cache in
// system RAM, so every decode step crossed PCIe instead of reading the 936 GB/s
// beside the chip.
//
// **Neither of the two booleans can be turned off from outside**, which is why
// this is a branch in the code rather than a documented override. ggml reads
// them for PRESENCE and not value -- `prefer_host_memory = getenv(...) !=
// nullptr` -- so `GGML_VK_PREFER_HOST_MEMORY=0` in the environment still turns
// it on. Only SUBALLOCATION_BLOCK_SIZE, which is parsed, can be overridden.
//
// **HarmonyOS kills an app that holds too much GPU memory**, with
// `GPU_LEAK_KILL`, and it did: a 1.1 GB model was accounted 8.57 GB of GPU
// buffers and the process was SIGKILLed 32 seconds after loading, at the first
// real compute submission. Not a crash and not ordinary OOM -- a platform
// policy about one number.
//
// PREFER_HOST_MEMORY matches the hardware: a phone has no separate video memory
// to prefer, since the GPU reads the same DRAM the CPU does. **It did not stop
// the kill** -- 8.57 GB became 8.03 -- so it stays because it is correct here,
// not because it helped.
//
// SUBALLOCATION_BLOCK_SIZE is the one that matches the NUMBER. ggml batches
// allocations into blocks of 1 GB by default, to avoid fragmentation on a
// desktop card; both observed totals are small integer multiples of that, which
// is the shape of a handful of buffers each rounded up to a gigabyte rather
// than of a model that genuinely needs eight. 64 MB is small enough to make the
// rounding invisible against a 1.1 GB model and large enough not to fragment.
//
// This is a hypothesis fitted to one number, which this project's record says
// to distrust -- so it is worth stating what would refute it: if the app is
// still killed with a total near 8 GB, the memory is being used rather than
// reserved and no allocation knob will help.
//
// Set here rather than in the build because these are properties of the device
// the code is running on, not of how it was compiled -- which is the same
// reason the discrete case is decided here too, at the only moment that knows.
// DISABLE_MMVQ is the one of these three that is not about the phone, and the
// only one measured to do anything. MMVQ is ggml's quantized mat-vec path, a
// performance optimization -- and on this Intel driver turning it OFF made the
// engine both FASTER and better: 78.3 to 71.0 ms a frame at Q8_0, 44.8 to 46.1
// at Q4_K_M, with the Q4 cost being the only thing it took. Both quantizations
// gained on quality by ear, across three seeds, judged on pace and on how
// mechanical the pauses were. A kernel that is slower and lossier is not a
// trade, so it is off by default rather than offered as a choice.
//
// Two neighbours were tried and are not set. DISABLE_INTEGER_DOT_PRODUCT costs
// another 3.8% and, stacked on this, was indistinguishable by ear.
// DISABLE_F16 costs 17% and introduced audible artefacts, which is the opposite
// of what disabling a lower precision should do -- unexplained, and enough not
// to touch it.
void apply_vulkan_env() {
#if defined(__ANDROID__) || defined(__OHOS__)
    // A phone's GPU reads the same DRAM the CPU does. That is not something to
    // find out at runtime -- there is no phone where it is false -- so the probe
    // does not run here at all and cannot fail here either.
#else
    // On a desktop the tuning has to be EARNED: only a GPU proven to share
    // system memory gets it, and both of the other answers -- a discrete card,
    // and a probe that could not say -- take ggml's defaults.
    //
    // The asymmetry is the whole design. Being wrong on an integrated GPU costs
    // about 10% (design/voice.md has 78.3 against 71.0 ms a frame at `Q8_0`).
    // Being wrong on a discrete card costs a factor of thirty, measured here:
    // 2.8 ms a frame against 108.6. So Unknown lands on the cheap side.
    //
    // It is Unknown that this is really about. A boolean probe answered false
    // for "integrated" and false for "I failed", and one silent failure on the
    // first load of a process was enough: ggml reads the environment when it
    // CREATES a device, so the correct answers from every later load arrived
    // after the device they would have changed.
#if defined(BODYCLOCK_QTTS_VULKAN_PROBE)
    const GpuMemory mem = vulkan_gpu_memory();
#else
    const GpuMemory mem = GpuMemory::Unknown;
#endif
    if (mem != GpuMemory::Shared) {
        // REMOVED, not just left unset, and that distinction cost an afternoon.
        // Declining to set them does nothing about the two ways they arrive
        // anyway: inherited from whatever launched the process -- a shell that
        // once exported them for an experiment is enough -- and left behind by
        // an earlier load in this one.
        //
        // This runs on EVERY load and not only the first, because ggml holds its
        // device through a weak_ptr: when the last engine lets go the device is
        // destroyed, and the next load builds a new one that reads all of this
        // again.
        unset_env("GGML_VK_PREFER_HOST_MEMORY");
        unset_env("GGML_VK_SUBALLOCATION_BLOCK_SIZE");
        unset_env("GGML_VK_DISABLE_MMVQ");
        platform::log_info("qtts: %s -- ggml's Vulkan defaults, not the shared-memory tuning",
                           mem == GpuMemory::Dedicated ? "discrete GPU" : "GPU memory unknown");
        return;
    }
    platform::log_info("qtts: integrated GPU -- the shared-memory tuning");
#endif
    set_env_default("GGML_VK_PREFER_HOST_MEMORY", "1");
    set_env_default("GGML_VK_SUBALLOCATION_BLOCK_SIZE", "67108864");
    set_env_default("GGML_VK_DISABLE_MMVQ", "1");
}
#endif

// Running loudness, accumulated as the samples go past so that nothing has to
// keep the audio. See QttsResult::level_drift_db for what it is for and what it
// is not.
//
// A quarter second per window: long enough to average a syllable, short enough
// that a four-second mean over them is built from sixteen points rather than
// four.
struct LevelTrace {
    static const int kWindowSamples = 6000;   // 0.25 s at 24 kHz

    double sum_squares;
    int    in_window;
    float  peak;
    std::vector<float> window_db;

    LevelTrace() : sum_squares(0.0), in_window(0), peak(0.0f) {}

    void reset() {
        sum_squares = 0.0;
        in_window   = 0;
        peak        = 0.0f;
        window_db.clear();
    }

    void feed(const float* pcm, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const float v = pcm[i];
            const float a = v < 0.0f ? -v : v;
            if (a > peak) {
                peak = a;
            }
            sum_squares += static_cast<double>(v) * static_cast<double>(v);
            if (++in_window == kWindowSamples) {
                const double mean = sum_squares / kWindowSamples;
                window_db.push_back(static_cast<float>(
                    20.0 * std::log10(std::sqrt(mean) + 1e-12)));
                sum_squares = 0.0;
                in_window   = 0;
            }
        }
    }

    float peak_dbfs() const {
        return peak > 0.0f ? static_cast<float>(20.0 * std::log10(peak)) : 0.0f;
    }

    // The range of a four-second moving mean over the SPEECH windows.
    //
    // Pauses are dropped first, at 25 dB below peak. Without that the metric
    // measures how much silence an utterance contains: a pause reads about
    // -40 dBFS, so any phrase boundary swamps a level difference of five.
    float drift_db() const {
        const float floor_db = peak_dbfs() - 25.0f;
        std::vector<float> speech;
        for (size_t i = 0; i < window_db.size(); ++i) {
            if (window_db[i] > floor_db) {
                speech.push_back(window_db[i]);
            }
        }
        const size_t span = 16;   // 4 s of speech windows
        if (speech.size() < 2 * span) {
            return 0.0f;
        }
        float lo = 0.0f, hi = 0.0f;
        for (size_t i = 0; i + span <= speech.size(); ++i) {
            float mean = 0.0f;
            for (size_t j = 0; j < span; ++j) {
                mean += speech[i + j];
            }
            mean /= static_cast<float>(span);
            if (i == 0 || mean < lo) lo = mean;
            if (i == 0 || mean > hi) hi = mean;
        }
        return hi - lo;
    }
};

const char* status_name(qt_status rc) {
    switch (rc) {
        case QT_STATUS_OK:              return "ok";
        case QT_STATUS_INVALID_PARAMS:  return "invalid params";
        case QT_STATUS_MODE_INVALID:    return "wrong mode for this checkpoint";
        case QT_STATUS_GENERATE_FAILED: return "generation failed";
        case QT_STATUS_OOM:             return "out of memory";
        case QT_STATUS_CANCELLED:       return "cancelled";
    }
    return "unknown";
}

}  // namespace

struct Qtts::Impl {
    qt_context* ctx;
    std::string instruct;
    std::string speaker;
    std::vector<float> reference;   // mono 24 kHz, empty when none
    std::string reference_text;
    // Clone mode A: the reference reduced to a speaker vector. Empty when the
    // reference is mode B or absent. The two are mutually exclusive by
    // construction -- setting either clears the other -- because qwen.h makes
    // them mutually exclusive in the parameters.
    std::vector<float> ref_spk_emb;
    // Cached at load, because say() has to know the checkpoint's kind to decide
    // which parameters are legal, and asking the engine per call would mean a
    // string compare against a pointer that only changes when the model does.
    std::string model_type;
    std::string error;
    long long    seed;
    int          top_k;      // 0 leaves qwentts.cpp's own default alone
    QttsSampling sampling;
    int         threads;    // 0 leaves its thread heuristic alone
    bool        streaming;
    LevelTrace  level;

    // Per-call streaming state. Written from the compute worker and read after
    // qt_synthesize has returned, which is the only ordering that matters --
    // qt_synthesize does not return until the worker is done with this request.
    QttsAudioSink   sink;
    void*           sink_user;
    clk::time_point started;
    double          first_audio;
    int             chunks;
    unsigned long   produced;
    bool            aborted;

    Impl()
        : ctx(0), seed(-1), top_k(0), threads(0), streaming(true),
          sink(0), sink_user(0),
          first_audio(0.0), chunks(0), produced(0), aborted(false) {}

    // Runs on qwentts.cpp's compute worker. Must not re-enter the qwen API and
    // must not block: a slow body here stalls generation, which is the thing
    // being timed.
    //
    // A member of Impl rather than a free function in the anonymous namespace
    // because Impl is private to Qtts, so only a member can see it.
    static bool on_chunk(const float* samples, int n_samples, void* user_data) {
        Impl* m = static_cast<Impl*>(user_data);
        if (!m) {
            return true;
        }
        if (m->chunks == 0) {
            m->first_audio = seconds_since(m->started);
        }
        ++m->chunks;
        if (n_samples > 0) {
            m->produced += static_cast<unsigned long>(n_samples);
            m->level.feed(samples, static_cast<size_t>(n_samples));
        }
        if (!m->sink) {
            return true;
        }
        if (!m->sink(samples, static_cast<size_t>(n_samples > 0 ? n_samples : 0), false, m->sink_user)) {
            m->aborted = true;
            return false;
        }
        return true;
    }
};

bool Qtts::available() {
    return true;
}

Qtts::Qtts() : impl_(new Impl()) {}

Qtts::~Qtts() {
    unload();
    delete impl_;
}

bool Qtts::loaded() const {
    return impl_ && impl_->ctx != 0;
}

bool Qtts::load(const std::string& talker_path, const std::string& codec_path) {
    Impl* m = impl_;
    m->error.clear();

    if (talker_path.empty() || codec_path.empty()) {
        m->error = "both a talker GGUF and a codec GGUF are required";
        platform::log_error("qtts: %s", m->error.c_str());
        return false;
    }
    unload();

    qt_log_set(&forward_log, 0);
    platform::log_info("qtts: %s", qt_version());
    platform::log_info("qtts: talker %s", talker_path.c_str());
    platform::log_info("qtts: codec  %s", codec_path.c_str());

    // Before qt_init, because that is the only moment it is read. The engine
    // logs the count it settled on -- "[Load] ... (CPU threads: N)" -- which is
    // what says whether the patch is in this build at all.
    apply_thread_env(m->threads);
#ifdef BODYCLOCK_QTTS_VULKAN
    apply_vulkan_env();
#endif

    qt_init_params p;
    qt_init_default_params(&p);
    p.talker_path = talker_path.c_str();
    p.codec_path  = codec_path.c_str();
    // No GPU backend is linked for this ABI, so flash attention has nothing to
    // apply to and the fp16 clamp guards a CUDA accumulation path that is not
    // here. Both left at their defaults rather than set, so that a future GPU
    // build changes behaviour by linking a backend and not by editing this.

    const clk::time_point started = clk::now();
    m->ctx = qt_init(&p);
    if (!m->ctx) {
        const char* why = qt_last_error();
        m->error = why && *why ? why : "qt_init failed";
        platform::log_error("qtts: load failed -- %s", m->error.c_str());
        return false;
    }
    const char* kind = qt_model_type(m->ctx);
    m->model_type = kind ? kind : "";
    platform::log_info("qtts: loaded in %.2f s, model type '%s', %d codebooks",
                       seconds_since(started), m->model_type.c_str(),
                       qt_num_codebooks(m->ctx));

    // The speaker table, logged once. "Nine named speakers" is something this
    // repo took from a model card and never checked; this is the file's answer.
    const int n = qt_n_speakers(m->ctx);
    for (int i = 0; i < n; ++i) {
        const char* name = qt_speaker_name(m->ctx, i);
        platform::log_info("qtts:   speaker %d: %s", i, name ? name : "(null)");
    }
    return true;
}

void Qtts::unload() {
    Impl* m = impl_;
    if (m && m->ctx) {
        qt_free(m->ctx);
        m->ctx = 0;
        m->model_type.clear();
    }
}

std::string Qtts::model_type() const {
    return impl_ ? impl_->model_type : std::string();
}

std::vector<std::string> Qtts::speakers() const {
    std::vector<std::string> out;
    if (!impl_ || !impl_->ctx) {
        return out;
    }
    const int n = qt_n_speakers(impl_->ctx);
    for (int i = 0; i < n; ++i) {
        const char* name = qt_speaker_name(impl_->ctx, i);
        if (name) {
            out.push_back(std::string(name));
        }
    }
    return out;
}

void Qtts::set_speaker(const std::string& name) {
    impl_->speaker = name;
}

const std::string& Qtts::speaker() const {
    return impl_->speaker;
}

bool Qtts::set_reference(const std::string& wav_path, const std::string& transcript) {
    Impl* m = impl_;
    m->error.clear();
    if (wav_path.empty() || transcript.empty()) {
        m->error = "a reference needs both a WAV and its transcript";
        return false;
    }

    std::vector<float> pcm;
    std::string why;
    if (!read_reference_wav(wav_path, &pcm, &why)) {
        m->error = why;
        platform::log_error("qtts: reference %s -- %s", wav_path.c_str(), why.c_str());
        return false;
    }

    m->reference.swap(pcm);
    m->reference_text = transcript;
    m->ref_spk_emb.clear();   // mutually exclusive; see qwen.h
    // The frame count as well as the duration, because the frames are what it
    // costs: the reference sits in the talker's context for every step, so this
    // many frames of attention are added to each one. Asked of the model rather
    // than divided out here, so it stays right if the codec's rate ever differs.
    const int frames = m->ctx ? qt_duration_sec_to_tokens(m->ctx, reference_seconds()) : 0;
    platform::log_info("qtts: reference %.2f s, %d frames of context, from %s",
                       reference_seconds(), frames, wav_path.c_str());
    return true;
}

bool Qtts::set_reference_voice(const std::string& wav_path) {
    Impl* m = impl_;
    m->error.clear();
    if (wav_path.empty()) {
        m->error = "a reference voice needs a WAV";
        return false;
    }
    if (!m->ctx) {
        // Not a detail that can be worked around by reordering the call later:
        // the extraction runs the model's speaker encoder, so the model has to
        // be there. Said plainly because the other setters are all load-order
        // independent and this one is not.
        m->error = "set_reference_voice needs a loaded model -- call it after load()";
        return false;
    }

    std::vector<float> pcm;
    std::string why;
    if (!read_reference_wav(wav_path, &pcm, &why)) {
        m->error = why;
        platform::log_error("qtts: reference voice %s -- %s", wav_path.c_str(), why.c_str());
        return false;
    }

    qt_voice_ref ref;
    std::memset(&ref, 0, sizeof(ref));
    const qt_status rc = qt_extract_voice_ref(m->ctx, &pcm[0], static_cast<int>(pcm.size()), &ref);
    if (rc != QT_STATUS_OK || !ref.ref_spk_emb || ref.ref_spk_dim <= 0) {
        const char* engine = qt_last_error();
        m->error = std::string("could not extract a speaker embedding: ") +
                   (engine && *engine ? engine : status_name(rc));
        qt_voice_ref_free(&ref);
        platform::log_error("qtts: %s", m->error.c_str());
        return false;
    }

    // The embedding is copied out and everything else is dropped -- the codes
    // in particular, which are what mode B would have carried and what makes it
    // expensive. Nothing here keeps the recording.
    m->ref_spk_emb.assign(ref.ref_spk_emb, ref.ref_spk_emb + ref.ref_spk_dim);
    qt_voice_ref_free(&ref);

    m->reference.clear();
    m->reference_text.clear();

    platform::log_info("qtts: reference voice from %.2f s of %s -- %d-dimension embedding, "
                       "no context cost",
                       static_cast<double>(pcm.size()) / sample_rate(), wav_path.c_str(),
                       static_cast<int>(m->ref_spk_emb.size()));
    return true;
}

void Qtts::clear_reference() {
    impl_->reference.clear();
    impl_->reference_text.clear();
    impl_->ref_spk_emb.clear();
}

float Qtts::reference_seconds() const {
    if (!impl_ || impl_->reference.empty()) {
        return 0.0f;
    }
    return static_cast<float>(impl_->reference.size()) / static_cast<float>(sample_rate());
}

void Qtts::set_voice(const std::string& instruct) {
    impl_->instruct = instruct;
}

const std::string& Qtts::voice() const {
    return impl_->instruct;
}

void Qtts::set_seed(long long seed) {
    impl_->seed = seed;
}

void Qtts::set_top_k(int top_k) {
    impl_->top_k = top_k;
}

void Qtts::set_sampling(const QttsSampling& sampling) {
    impl_->sampling = sampling;
}

const QttsSampling& Qtts::sampling() const {
    return impl_->sampling;
}

void Qtts::set_threads(int n) {
    impl_->threads = n > 0 ? n : 0;
}

void Qtts::set_streaming(bool on) {
    impl_->streaming = on;
}

bool Qtts::streaming() const {
    return impl_->streaming;
}

const std::string& Qtts::last_error() const {
    return impl_->error;
}

bool Qtts::say(const std::string& text, QttsAudioSink sink, void* user_data, QttsResult* out) {
    Impl* m = impl_;
    m->error.clear();
    if (out) {
        *out = QttsResult();
    }
    if (!m->ctx) {
        m->error = "no model loaded";
        return false;
    }
    if (text.empty()) {
        m->error = "nothing to say";
        return false;
    }

    m->sink        = sink;
    m->sink_user   = user_data;
    m->first_audio = 0.0;
    m->chunks      = 0;
    m->produced    = 0;
    m->aborted     = false;
    m->level.reset();

    qt_tts_params p;
    qt_tts_default_params(&p);
    p.text = text.c_str();
    // lang NULL is "auto": the prompt carries no language id and the model
    // infers it. Left that way deliberately -- this app's text mixes Chinese
    // with the odd English word, and naming one of them would be a claim about
    // the whole utterance.
    p.lang = 0;

    // The mode table, in one place. Each checkpoint takes a different set and
    // rejects the others outright, so sending a field left over from a model
    // that was loaded earlier turns a working configuration into
    // QT_STATUS_MODE_INVALID for a reason the host never asked for -- and the
    // host is a page whose text boxes keep their contents across a reload.
    //
    //   voice_design   instruct required, nothing else
    //   custom_voice   speaker, and instruct optional on top of it
    //   base           a reference recording and its transcript, nothing else
    //
    // The page mirrors this by showing only the controls that apply, so the
    // filtering here is the backstop rather than the explanation.
    const bool is_base   = (m->model_type == "base");
    const bool is_custom = (m->model_type == "custom_voice");

    if (!is_base && !m->instruct.empty()) {
        p.instruct = m->instruct.c_str();
    }
    if (is_custom && !m->speaker.empty()) {
        p.speaker = m->speaker.c_str();
    }
    if (is_base && !m->reference.empty() && !m->reference_text.empty()) {
        p.ref_audio_24k = &m->reference[0];
        p.ref_n_samples = static_cast<int>(m->reference.size());
        p.ref_text      = m->reference_text.c_str();
    } else if (is_base && !m->ref_spk_emb.empty()) {
        // Mode A. No ref_codes and no ref_text, which is exactly what selects
        // it -- and what keeps the reference out of the per-frame attention.
        p.ref_spk_emb = &m->ref_spk_emb[0];
        p.ref_spk_dim = static_cast<int>(m->ref_spk_emb.size());
    }

    p.seed = static_cast<int64_t>(m->seed);

    // Both stacks, every field. qt_tts_default_params has already seeded `p`
    // from sampling-defaults.h and QttsSampling's own defaults are the same
    // values, so this is a no-op until somebody changes one -- which is why it
    // is assigned unconditionally rather than guarded field by field. A guard
    // would also make "temperature 0", meaning argmax, unreachable.
    p.temperature           = m->sampling.temperature;
    p.top_k                 = m->sampling.top_k;
    p.top_p                 = m->sampling.top_p;
    p.repetition_penalty    = m->sampling.repetition_penalty;
    p.subtalker_temperature = m->sampling.subtalker_temperature;
    p.subtalker_top_k       = m->sampling.subtalker_top_k;
    p.subtalker_top_p       = m->sampling.subtalker_top_p;

    // set_top_k() last, because it is the older and narrower of the two doors
    // onto the same field and a caller that used it meant it. > 0 only: that
    // setter's contract is that 0 leaves the engine's choice alone, and the
    // engine's choice now arrives through the struct above.
    if (m->top_k > 0) {
        p.top_k = m->top_k;
    }
    // on_chunk is what selects the engine's streaming pipeline, so it goes on
    // only when both are wanted: a sink to give the chunks to, and streaming
    // asked for. Without it the engine decodes the whole utterance in one pass
    // and fills `audio` instead.
    const bool stream = m->streaming && sink != 0;
    if (stream) {
        p.on_chunk           = &Impl::on_chunk;
        p.on_chunk_user_data = m;
    }

    m->started = clk::now();
    qt_audio audio;
    audio.samples     = 0;
    audio.n_samples   = 0;
    audio.sample_rate = 0;
    audio.channels    = 0;

    const qt_status rc = qt_synthesize(m->ctx, &p, &audio);
    const double elapsed = seconds_since(m->started);

    // An abort is the host stopping, not a failure, so it takes the success
    // path with whatever was produced -- the same shape as every other lane.
    const bool cancelled_by_sink = (rc == QT_STATUS_CANCELLED) && m->aborted;
    if (rc != QT_STATUS_OK && !cancelled_by_sink) {
        const char* why = qt_last_error();
        m->error = std::string(status_name(rc)) + (why && *why ? std::string(": ") + why : std::string());
        platform::log_error("qtts: synthesis failed after %.2f s -- %s", elapsed, m->error.c_str());
        qt_audio_free(&audio);
        m->sink = 0;
        return false;
    }

    // The buffered path produced one buffer and nobody has seen it yet; the
    // streaming path produced nothing here and `audio` is empty by design.
    //
    // One handover, carrying `last` with the data, which is the omni sink's
    // shape -- there is no ambiguity about the end when there is only one call.
    // first_audio stays 0: nothing arrived early, and reporting the whole
    // elapsed time as a "wait for the first sample" would make the two modes
    // look comparable on a number where they are not.
    if (!stream && audio.n_samples > 0) {
        m->produced = static_cast<unsigned long>(audio.n_samples);
        m->chunks   = 1;
        m->level.feed(audio.samples, static_cast<size_t>(audio.n_samples));
        if (sink && !sink(audio.samples, static_cast<size_t>(audio.n_samples), true, user_data)) {
            m->aborted = true;
        }
    }
    qt_audio_free(&audio);

    // The end marker, on a call of its own, for the streaming path only.
    // qwentts.cpp's chunk callback has no `last` flag, and holding a chunk back
    // to give it one would delay the first chunk, which is the whole point of
    // streaming. See voice/qtts.hpp.
    if (stream && !m->aborted) {
        sink(0, 0, true, user_data);
    }

    const float speech = static_cast<float>(m->produced) / static_cast<float>(sample_rate());
    if (out) {
        out->speech_seconds      = speech;
        out->elapsed             = elapsed;
        out->first_audio_seconds = m->first_audio;
        out->chunks              = m->chunks;
        out->peak_dbfs           = m->level.peak_dbfs();
        out->level_drift_db      = m->level.drift_db();
    }
    platform::log_info("qtts: %.2f s of speech in %.2f s (%.2fx), %s, first audio %.0f ms, "
                       "%d chunk(s), peak %.1f dBFS, level drift %.1f dB%s",
                       speech, elapsed, speech > 0.0f ? elapsed / speech : 0.0,
                       stream ? "streamed" : "buffered",
                       m->first_audio * 1000.0, m->chunks,
                       m->level.peak_dbfs(), m->level.drift_db(),
                       m->aborted ? ", aborted" : "");
    m->sink = 0;
    return true;
}

#else   // !BODYCLOCK_QTTS

// The supported off state, not a degraded one. Everything links and answers
// truthfully, so a build without the SDK is a build without a described voice
// rather than a build that fails.

struct Qtts::Impl {
    std::string instruct;
    std::string speaker;
    std::string error;
};

bool Qtts::available() {
    return false;
}

Qtts::Qtts() : impl_(new Impl()) {}

Qtts::~Qtts() { delete impl_; }

bool Qtts::loaded() const { return false; }

bool Qtts::load(const std::string&, const std::string&) {
    impl_->error = "this build has no qwentts.cpp (BODYCLOCK_QTTS off)";
    platform::log_error("qtts: %s", impl_->error.c_str());
    return false;
}

void Qtts::unload() {}

std::string Qtts::model_type() const { return std::string(); }

std::vector<std::string> Qtts::speakers() const { return std::vector<std::string>(); }

void Qtts::set_voice(const std::string& instruct) { impl_->instruct = instruct; }

const std::string& Qtts::voice() const { return impl_->instruct; }

void Qtts::set_speaker(const std::string& name) { impl_->speaker = name; }

const std::string& Qtts::speaker() const { return impl_->speaker; }

bool Qtts::set_reference(const std::string&, const std::string&) {
    impl_->error = "this build has no qwentts.cpp (BODYCLOCK_QTTS off)";
    return false;
}

bool Qtts::set_reference_voice(const std::string&) {
    impl_->error = "this build has no qwentts.cpp (BODYCLOCK_QTTS off)";
    return false;
}

void Qtts::clear_reference() {}

float Qtts::reference_seconds() const { return 0.0f; }

void Qtts::set_seed(long long) {}

void Qtts::set_top_k(int) {}

void Qtts::set_sampling(const QttsSampling&) {}

// The defaults, which is what a build with no engine would have used anyway.
const QttsSampling& Qtts::sampling() const {
    static const QttsSampling kDefaults;
    return kDefaults;
}

void Qtts::set_threads(int) {}

void Qtts::set_streaming(bool) {}

bool Qtts::streaming() const { return false; }

const std::string& Qtts::last_error() const { return impl_->error; }

bool Qtts::say(const std::string&, QttsAudioSink, void*, QttsResult* out) {
    if (out) {
        *out = QttsResult();
    }
    impl_->error = "this build has no qwentts.cpp (BODYCLOCK_QTTS off)";
    return false;
}

#endif  // BODYCLOCK_QTTS

}  // namespace voice
}  // namespace bodyclock
