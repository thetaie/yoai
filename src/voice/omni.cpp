#include "voice/omni.hpp"

#include "platform/log.hpp"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#ifdef BODYCLOCK_MNN
#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#endif
#include <cstdio>
#include <MNN/expr/Expr.hpp>
#include <MNN/expr/ExprCreator.hpp>
#include <llm/llm.hpp>
#include <ostream>
#include <sstream>

#ifdef BODYCLOCK_MNN_VULKAN
// Declared here rather than in an MNN header, because it is not part of MNN's
// API: it exists only to be referenced. See the call in load().
extern "C" void MNNForceLinkVulkanRuntime();
#endif
#endif

namespace bodyclock {
namespace voice {

// Thirty seconds is the window a Whisper-family audio encoder has, and MNN does
// not chunk past it -- it segfaults inside the front end. Measured: 28 s fine,
// 31 s crash. Two seconds of margin below the cliff, because the limit is
// upstream's and may not be exactly 30.
float Omni::max_seconds() {
    return 28.0f;
}

#ifdef BODYCLOCK_MNN

namespace {

// The key the prompt template refers the audio by. MNN looks the placeholder up
// in the audios map by exactly the text between the tags, so this string appears
// in two places and has to match.
const char* const kAudioKey = "recording";

// Everything about the model directory, written to the log before anything is
// loaded.
//
// This is not scaffolding to delete once it works. Loading this engine can take
// the process down with a SIGSEGV, and MNN's own diagnostics (hilog tag MNNJNI,
// MNN_ERROR at error level, MNN_PRINT at debug) stop at whatever it managed to
// reach -- which the first time round was nothing at all, because it died in
// config parsing before any code of its own ran. So the last line printed from
// HERE is what says which step it was, and a missing or short file is the most
// likely reason for a load to die: worth knowing before the crash, not after.
//
// The stack is recoverable too, and not where it looks like it should be. The
// faultlog directory refuses a shell, but processdump writes the whole
// backtrace into the system log under the tag DfxFaultLogger, and the addresses
// symbolize against the unstripped .so under entry/build.
void log_model_dir(const std::string& dir) {
    platform::log_info("omni: --- model directory %s", dir.c_str());

#if !defined(_WIN32)
    DIR* d = opendir(dir.c_str());
    if (!d) {
        platform::log_error("omni: cannot open the directory itself (errno %d)", errno);
        return;
    }
    int count = 0;
    unsigned long long total = 0;
    struct dirent* e = 0;
    while ((e = readdir(d)) != 0) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            platform::log_error("omni:   %-30s UNREADABLE (errno %d)", name.c_str(), errno);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            platform::log_info("omni:   %-30s <dir>", name.c_str());
            continue;
        }
        // Opened as well as stat'ed: a file can be listed and sized and still
        // not be readable, which is exactly the failure a sandbox produces.
        std::FILE* f = std::fopen(path.c_str(), "rb");
        const bool readable = f != 0;
        if (f) {
            std::fclose(f);
        }
        platform::log_info("omni:   %-30s %10lld bytes%s", name.c_str(),
                           static_cast<long long>(st.st_size),
                           readable ? "" : "  NOT READABLE");
        ++count;
        total += static_cast<unsigned long long>(st.st_size);
    }
    closedir(d);
    platform::log_info("omni: --- %d files, %llu bytes total", count, total);
#else
    platform::log_info("omni: (directory listing is only built for the device)");
#endif
}

// The whole of a small text file, for the log. config.json and llm_config.json
// decide which sub-models load and at what precision, and a wrong one explains
// more failures than any stack would.
void log_small_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        platform::log_error("omni: cannot read %s", path.c_str());
        return;
    }
    std::string text;
    char buf[512];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 4000) {
        text.append(buf, got);
    }
    std::fclose(f);
    // One line, because hilog truncates long entries and a JSON blob split over
    // several lines is harder to read back than one that was cut off.
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') {
            text[i] = ' ';
        }
    }
    platform::log_info("omni: %s = %s", path.c_str(), text.c_str());
}

// Wall clock, and the distinction is not pedantic here.
//
// This used to be clock(), which on POSIX returns PROCESSOR time summed over
// threads. MNN runs four, so every figure this file reported was inflated by
// something near that factor, and the one that mattered most was the worst:
// the first phone run printed "5.18 s of speech in 87.9 s (17.0x real time)"
// against 18.2 s on the log's own timestamps -- 3.5x. That ratio is the number
// design/voice.md is waiting on to decide whether this lane ships at all, so
// the difference between 17x and 3.5x is the difference between two answers.
//
// A person waits wall time. Processor-seconds would be the right unit for a
// battery question, and this is not one.
typedef std::chrono::steady_clock clk;

double seconds_since(clk::time_point started) {
    return std::chrono::duration<double>(clk::now() - started).count();
}

// MNN's generation status as a word. The numbers are in its public header and
// nowhere near the log line that prints them, so a raw "Status: 4" costs a trip
// into the SDK every time.
const char* status_name(MNN::Transformer::LlmStatus status) {
    switch (status) {
        case MNN::Transformer::LlmStatus::NOT_LOADED:          return "NOT_LOADED";
        case MNN::Transformer::LlmStatus::RUNNING:             return "RUNNING";
        case MNN::Transformer::LlmStatus::NORMAL_FINISHED:     return "NORMAL_FINISHED";
        case MNN::Transformer::LlmStatus::MAX_TOKENS_FINISHED: return "MAX_TOKENS_FINISHED";
        case MNN::Transformer::LlmStatus::USER_CANCEL:         return "USER_CANCEL";
        case MNN::Transformer::LlmStatus::INTERNAL_ERROR:      return "INTERNAL_ERROR";
        case MNN::Transformer::LlmStatus::TIMEOUT:             return "TIMEOUT";
    }
    return "?";
}

// A directory path with exactly one trailing separator, so the two places that
// build paths from it agree.
std::string model_dir_with_slash(const std::string& dir) {
    if (dir.empty()) {
        return "./";
    }
    const char last = dir[dir.size() - 1];
    return (last == '/' || last == '\\') ? dir : dir + "/";
}

}  // namespace

struct Omni::Impl {
    std::unique_ptr<MNN::Transformer::Llm> llm;
    bool loaded = false;

    // Whether the thinker graph exposes the hidden state the Talker consumes.
    // Checked once at load, because without it speech is not merely absent, it
    // is FATAL: the Talker's prefill reads its second embed unconditionally, so
    // an empty collection is an out-of-bounds read on a vector of one.
    bool has_talker_embeds = false;

    // Where the Talker's PCM goes while one exchange runs. A member rather than
    // a capture because the callback MNN takes is a std::function installed once,
    // and the sink it should forward to changes per call.
    OmniAudioSink sink      = 0;
    void*         sink_user = 0;
    bool          aborted   = false;
    size_t        produced  = 0;

    // Stamped when the exchange begins and when the first block comes back, so
    // the wait before the first word can be told apart from the wait for all of
    // it. blocks counts the handovers: one means nothing streamed.
    clk::time_point started;
    clk::time_point first_audio;
    int             blocks = 0;

    // The previous block's arrival and the first block's size, for the per-block
    // line: the gap between two blocks is what a listener hears as a hole, and
    // the first block has to be left out of the rate because it cost no clock.
    clk::time_point prev_audio;
    size_t          first_block = 0;
};

bool Omni::available() {
    return true;
}

Omni::Omni() : impl_(new Impl()) {}

Omni::~Omni() {
    delete impl_;
}

bool Omni::loaded() const {
    return impl_->loaded;
}

bool Omni::load(const std::string& model_dir) {
    Impl* m = impl_;
    m->loaded = false;
    m->llm.reset();

#ifdef BODYCLOCK_MNN_VULKAN
    // Before createLLM, because the backend has to be registered before anything
    // asks for it. This call does nothing; REFERENCING it is the point, and the
    // reason is in the patch it needs -- a static libMNN leaves the Vulkan
    // registrar's object out of the link, and the only symptom is one line of
    // MNN's own output saying it fell back to the CPU.
    MNNForceLinkVulkanRuntime();
#endif

    // MNN resolves every other file relative to this one, which is why the
    // public API takes a directory: handing it llm.mnn directly would leave the
    // talker, the vocoder and the speaker dictionary unfindable.
    const std::string config = model_dir_with_slash(model_dir) + "config.json";

    // Logged step by step, and not only while this is new. Loading this engine
    // can take the whole process down with a SIGSEGV and no usable stack: the
    // faultlog directory is not readable from a shell, and the system log
    // carries nothing between the last line before the crash and the app
    // restarting. These four lines are the only way to learn which step it was.
    log_model_dir(model_dir);
    log_small_file(config);
    log_small_file(model_dir_with_slash(model_dir) + "llm_config.json");

    platform::log_info("omni: createLLM '%s'", config.c_str());
    m->llm.reset(MNN::Transformer::Llm::createLLM(config));
    if (!m->llm) {
        platform::log_error("omni: cannot create an engine from '%s'", config.c_str());
        return false;
    }

    // **Nothing about precision or memory is overridden here, and that is the
    // result of an experiment rather than an omission.**
    //
    // Two overrides used to sit in this call and neither survived. use_mmap
    // came first, reasoned from "3.8 GB of weights should not be read into an
    // app's heap" and never measured; it was added to explain a load that
    // killed the process, and that explanation turned out to be wrong -- the
    // system log showed a segfault, not a memory kill. precision high came
    // second, to dodge the phone's fp16 kernels after the thinker's prefill
    // died in shape computation:
    //
    //     MNNJNI: Compute Shape Error for /Unsqueeze_output_0
    //     MNNJNI: onForward returned no outputs. seqLen=182, inDecode=0
    //
    // It changed nothing -- the same error, at the same op. In hindsight it
    // should not have been the first guess: shape computation runs before a
    // kernel is chosen, so fp16 was never a plausible cause.
    //
    // What the desktop then established, with THIS call sequence and an
    // in-memory waveform rather than MNN's own demo: the model's own
    // config.json answers correctly, 263 prompt tokens to 35 generated. The
    // phone was running that configuration plus these two overrides. So they
    // go, and the phone gets the configuration the desktop succeeds with.
    //
    // tmp_path stays. It is where MNN puts its caches, it has to be somewhere
    // writable, and MNN creates it. A wrong path costs a per-run recompile
    // rather than failing, so it degrades quietly -- inside the model
    // directory, which the caller already had to be able to read.
    //
    // If memory does turn out to be the problem, use_mmap is where to look
    // again, but with a measurement behind it this time.
    const std::string cache = model_dir_with_slash(model_dir) + "mnn-cache";
    platform::log_info("omni: set_config, cache '%s'", cache.c_str());
    m->llm->set_config("{\"tmp_path\":\"" + cache + "\"}");
    platform::log_info("omni: config as MNN resolved it = %s",
                       m->llm->dump_config().c_str());

    platform::log_info("omni: load() begin -- if this is the last line, it died inside MNN");
    const clk::time_point load_started = clk::now();
    if (!m->llm->load()) {
        platform::log_error("omni: '%s' failed to load", config.c_str());
        m->llm.reset();
        return false;
    }

    // AFTER load(), and that ordering is load-bearing rather than tidy.
    // Omni::setWavformCallback forwards to the Talker, which load() is what
    // creates -- and it guards with `if (mTalker)`, so calling it first is a
    // silent no-op. The callback would simply never be installed, the Talker
    // would generate into nothing, and the page would report zero seconds of
    // speech with no error anywhere. Found by reading MNN rather than by it
    // failing.
    m->llm->setWavformCallback([m](const float* pcm, size_t n, bool last) -> bool {
        if (!m->sink) {
            return true;
        }
        // Stamped before the sink runs, not after: the sink writes the samples
        // somewhere, and how long that takes is the host's business, not the
        // engine's latency.
        const clk::time_point now = clk::now();
        if (m->blocks == 0) {
            m->first_audio = now;
        }
        ++m->blocks;
        m->produced += n;

        // One line per block, because the summary at the end cannot answer the
        // question a listener is asking. "16 blocks, 19.1 s of speech in 66 s"
        // is compatible with an even trickle and with a long stall followed by a
        // burst, and those sound completely different -- the first is a slow
        // voice, the second is a voice with holes in it.
        //
        // The rate here is the TALKER's, so the first block is excluded from
        // both halves of it: it arrives with whatever was ready, at no cost in
        // clock, and counting it made this lane read 0.56x when it was running
        // at 0.28x. Below 1x, a player that started at block 1 runs dry between
        // every pair of blocks and the shortfall is the ratio itself.
        const double at   = std::chrono::duration<double>(now - m->started).count();
        const double gap  = std::chrono::duration<double>(now - m->prev_audio).count();
        const double have = static_cast<double>(m->produced) / 24000.0;
        if (m->blocks == 1) {
            m->first_block = n;
            platform::log_info("omni: block 1 at %.2f s -- %.2f s of audio",
                               at, static_cast<double>(n) / 24000.0);
        } else {
            const double since_first =
                std::chrono::duration<double>(now - m->first_audio).count();
            const double rate = since_first > 0.0
                                    ? (have - static_cast<double>(m->first_block) / 24000.0) /
                                          since_first
                                    : 0.0;
            platform::log_info("omni: block %d at %.2f s (+%.2f s) -- %.2f s of audio, "
                               "%.2f s in hand, talker at %.2fx real time",
                               m->blocks, at, gap, static_cast<double>(n) / 24000.0, have, rate);
        }
        m->prev_audio = now;

        if (!m->sink(pcm, n, last, m->sink_user)) {
            m->aborted = true;
            return false;
        }
        return true;
    });

    // The thinker's SECOND output, and the whole speech path hangs off it. MNN
    // asks the graph for "talker_embeds" only when llm_config.json says
    // has_talker, and it collects one per forward pass; the Talker's prefill
    // then concatenates embeds[0] and embeds[1]. Nothing in that chain checks,
    // so a graph that does not publish the output produces a load that looks
    // fine and a crash later, inside MNN, with the audio already recorded.
    const int talker_embeds = m->llm->getOutputIndex("talker_embeds");
    m->has_talker_embeds = talker_embeds >= 0;
    platform::log_info("omni: output index of 'talker_embeds' = %d%s", talker_embeds,
                       m->has_talker_embeds ? "" : "  -- NO SPEECH POSSIBLE");

    m->loaded = true;
    platform::log_info("omni: load() done in %.1f s, callback installed",
                       seconds_since(load_started));
    return true;
}

bool Omni::set_dit_steps(int steps) {
    Impl* m = impl_;
    if (!m->loaded || !m->llm) {
        platform::log_warn("omni: set_dit_steps with no model loaded");
        return false;
    }
    // Below 2 would make MNN's loop run zero forwards and hand the vocoder the
    // initial noise, which is not a fast voice -- it is a broken one. Refused
    // here rather than discovered by listening.
    if (steps < 2) {
        platform::log_warn("omni: dit_steps must be at least 2, got %d", steps);
        return false;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{\"dit_steps\":%d}", steps);
    m->llm->set_config(buf);
    // Logged rather than returned, because the number that matters is the one
    // the NEXT "talker produced" line was made with, and the two are read
    // together.
    platform::log_info("omni: dit_steps -> %d (%d DiT forward(s) per chunk)",
                       steps, steps - 1);
    return true;
}

bool Omni::ask(const float* pcm, size_t n_samples, const std::string& prompt,
               OmniAudioSink sink, OmniTextSink on_text, void* user_data, OmniReply* out) {
    if (!out) {
        return false;
    }
    *out = OmniReply();

    Impl* m = impl_;
    if (!m->loaded || !m->llm) {
        platform::log_error("omni: ask called with no model loaded");
        return false;
    }
    if (!pcm || n_samples == 0 || prompt.empty()) {
        platform::log_warn("omni: pcm, n_samples and prompt are all required");
        return false;
    }

    // Refused before the microphone data is thrown away. MNN's error, timeout
    // and cancel statuses are terminal: every entry point checks the status
    // first and returns, so once one is set this engine will never answer
    // again. Reported here rather than after another full prefill that cannot
    // produce anything.
    const MNN::Transformer::LlmStatus before = m->llm->getContext()->status;
    if (before == MNN::Transformer::LlmStatus::INTERNAL_ERROR ||
        before == MNN::Transformer::LlmStatus::TIMEOUT ||
        before == MNN::Transformer::LlmStatus::USER_CANCEL) {
        platform::log_error("omni: engine stuck in %s -- load the model again",
                            status_name(before));
        return false;
    }

    const float seconds = static_cast<float>(n_samples) / 16000.0f;
    if (seconds > max_seconds()) {
        // Refused, not attempted. This is upstream's cliff, and walking off it
        // takes the whole process with it.
        platform::log_error(
            "omni: %.1f s of audio exceeds the %.0f s this engine survives -- "
            "chunk it before calling",
            seconds, max_seconds());
        return false;
    }

    // Straight from memory. MNN will take a file path instead, and that would be
    // easier, but a voice note written to disk can be left behind by a crash.
    MNN::Express::VARP waveform = MNN::Express::_Const(
        pcm, {static_cast<int>(n_samples)}, MNN::Express::NCHW, halide_type_of<float>());

    MNN::Transformer::MultimodalPrompt mm;
    mm.prompt_template = std::string("<audio>") + kAudioKey + "</audio>" + prompt;
    MNN::Transformer::PromptAudioPart part;
    part.waveform = waveform;
    mm.audios[kAudioKey] = part;

    m->sink      = sink;
    m->sink_user = user_data;
    m->aborted   = false;
    m->produced  = 0;
    m->blocks    = 0;
    m->first_block = 0;

    const clk::time_point started = clk::now();
    m->started    = started;
    m->prev_audio = started;
    std::ostringstream text;

    // ONE response() call, prompt through to the last token, which is MNN's own
    // call shape and not the one this used to have.
    //
    // It ran prefill-only (max_new_tokens 0) and then stepped generate(1) by
    // hand, to work around speech coming back as silence. That was the wrong
    // fix for a real problem: the silence came from installing the waveform
    // callback before load(), which is a no-op because load() is what creates
    // the Talker -- see the note there. With the callback in place the Talker
    // collects an embed on every forward pass of this one call, and the hand
    // stepping only put MNN into a state its own code never produces.
    //
    // The empty third argument is `end_with`, and empty is the point. MNN
    // writes that string into the stream when generation stops -- it is a
    // terminator for a console, not anything the model produced. This passed
    // "<eop>" and the page duly showed it at the end of every reply, looking
    // like a token the model had emitted. NULL is not the way to silence it:
    // MNN reads NULL as "use the default" and the default is a newline.
    const int kMaxNewTokens = 512;
    m->llm->response(mm, &text, "", kMaxNewTokens);

    const MNN::Transformer::LlmContext* ctx = m->llm->getContext();
    out->text            = text.str();
    out->thinker_seconds = seconds_since(started);
    out->seconds         = out->thinker_seconds;
    platform::log_info("omni: thinker %d prompt tokens -> %d generated in %.1f s, status %s",
                       ctx->prompt_len, ctx->gen_seq_len, out->thinker_seconds,
                       status_name(ctx->status));

    // Where the thinker's time went, straight off MNN's own counters. These sit
    // on the public LlmContext and cost nothing to read, which makes it worth
    // asking why they were not being read: the one number reported was the sum,
    // and a sum cannot say whether a text-only answer is expensive because of
    // the audio encoder (fixed per second of clip) or the prefill (fixed per
    // token). Those have different fixes.
    //
    // They are microseconds, and audio_us covers the encoder pass that turns
    // the waveform into the tokens prefill then reads.
    //
    // The clip length comes from n_samples, not from MNN's audio_input_s: that
    // field is only filled on the file-path variant of audioProcess, and this
    // hands over a waveform from memory, so it reads 0.0 and printing it looked
    // like a bug in the front end rather than a field nobody set.
    platform::log_info("omni: thinker split -- audio %.2f s, prefill %.2f s, decode %.2f s, "
                       "sample %.2f s, for %.1f s of input audio",
                       ctx->audio_us / 1e6, ctx->prefill_us / 1e6, ctx->decode_us / 1e6,
                       ctx->sample_us / 1e6, seconds);

    // No tokens is a FAILURE, and saying so is the point. It used to come back
    // as success with an empty string, which the page showed as "(nothing)" --
    // the one outcome that looks like a bad answer rather than a broken engine.
    //
    // And once it happens it is permanent: MNN's error status is checked at the
    // top of every entry point, so every later ask returns in 0.0 s having done
    // nothing. The model has to be loaded again, which is why that is what this
    // says. MNN's own reason is on hilog under the tag MNNJNI.
    if (ctx->gen_seq_len == 0) {
        platform::log_error(
            "omni: the thinker generated nothing (status %s) -- see tag MNNJNI "
            "for MNN's reason. An error status is sticky: load the model again.",
            status_name(ctx->status));
        m->sink      = 0;
        m->sink_user = 0;
        return false;
    }

    // The words, before the voice. AFTER the failure check above, so a host is
    // never handed an empty reply that it would have to tell apart from a real
    // one, and before generateWavform(), which is where the rest of the minute
    // goes. See OmniTextSink for the two numbers that make the difference.
    if (on_text) {
        on_text(out->text.c_str(), user_data);
    }

    // Two conditions before handing over to the Talker, and both are about not
    // crashing inside MNN:
    //
    //   - the graph publishes talker_embeds at all (checked at load);
    //   - the thinker produced at least two tokens. The Talker's prefill
    //     concatenates embeds[0] and embeds[1], and the embeds come one per
    //     forward pass, so a reply of a single token leaves it one short and
    //     the index is not bounds-checked. MNN's own interleaved path guards
    //     the same spot with hasEmbeds(), which is not reachable from here.
    const bool can_speak = sink && !m->aborted && m->has_talker_embeds && ctx->gen_seq_len >= 2;
    if (sink && !can_speak) {
        platform::log_warn("omni: no speech -- talker_embeds %s, %d tokens generated",
                           m->has_talker_embeds ? "present" : "MISSING", ctx->gen_seq_len);
    }
    if (can_speak) {
        // The Talker keeps its own counters, on its own context. generate_init()
        // clears prefill_us, decode_us and sample_us but NOT audio_us, so that
        // one is snapshotted and subtracted; the other two would be correct
        // either way, and are read the same way so the three stay symmetrical.
        const MNN::Transformer::LlmContext* tctx = m->llm->getTalkerContext();
        const int64_t prefill0 = tctx ? tctx->prefill_us : 0;
        const int64_t decode0  = tctx ? tctx->decode_us : 0;
        const int64_t wav0     = tctx ? tctx->audio_us : 0;

        const clk::time_point spoke = clk::now();
        m->llm->generateWavform();
        out->speech_elapsed = seconds_since(spoke);
        out->speech_seconds = static_cast<float>(m->produced) / 24000.0f;
        out->seconds        = seconds_since(started);
        out->audio_blocks   = m->blocks;
        if (m->blocks > 0) {
            out->first_audio_seconds =
                std::chrono::duration<double>(m->first_audio - started).count();
        }
        platform::log_info("omni: talker produced %.2f s of speech in %.1f s (%.1fx real time)",
                           out->speech_seconds, out->speech_elapsed,
                           out->speech_seconds > 0.0f
                               ? out->speech_elapsed / out->speech_seconds
                               : 0.0);
        // The streaming half, and the one that decides whether the ratio above
        // matters. If the first block arrives early and the rest keeps ahead of
        // playback, nobody experiences 3.7x.
        platform::log_info("omni: first audio at %.1f s in %d block(s); thinker %.1f s, "
                           "whole exchange %.1f s",
                           out->first_audio_seconds, out->audio_blocks,
                           out->thinker_seconds, out->seconds);

        // And WHERE that time went, which is the only question with an action
        // attached. dit_steps scales token2wav and nothing else, so a ratio
        // carried by the decode is one this lane cannot turn down.
        //
        // The remainder is real and worth printing: it is MNN graph setup,
        // buffer concatenation and the handovers to the sink, none of which is
        // counted anywhere. A large one means the split above is not the story.
        if (tctx != 0) {
            out->talker_prefill_seconds = (tctx->prefill_us - prefill0) / 1e6;
            out->talker_decode_seconds  = (tctx->decode_us - decode0) / 1e6;
            out->token2wav_seconds      = (tctx->audio_us - wav0) / 1e6;
            out->codec_tokens           = tctx->gen_seq_len;
            const double rest = out->speech_elapsed - out->talker_prefill_seconds -
                                out->talker_decode_seconds - out->token2wav_seconds;
            platform::log_info(
                "omni: speech split -- prefill %.2f s, decode %.2f s (%d codec tokens), "
                "token2wav %.2f s (DiT + vocoder), %.2f s elsewhere",
                out->talker_prefill_seconds, out->talker_decode_seconds, out->codec_tokens,
                out->token2wav_seconds, rest);
        } else {
            // Not a warning: a stock MNN is a supported build, it simply cannot
            // answer this. Said once per exchange so a sweep reading 0.00 knows
            // why rather than recording it as a measurement.
            platform::log_info("omni: speech split unavailable -- this MNN has no "
                               "getTalkerContext(); see third/patches");
        }
    }

    m->sink      = 0;
    m->sink_user = 0;
    return true;
}

#else   // !BODYCLOCK_MNN

struct Omni::Impl {
    int unused;
};

bool Omni::available() {
    return false;
}

Omni::Omni() : impl_(0) {}
Omni::~Omni() {}
bool Omni::loaded() const { return false; }

bool Omni::load(const std::string&) {
    platform::log_error("omni: this build has no MNN engine compiled in");
    return false;
}

bool Omni::set_dit_steps(int) {
    return false;
}

bool Omni::ask(const float*, size_t, const std::string&, OmniAudioSink, OmniTextSink, void*,
               OmniReply* out) {
    if (out) {
        *out = OmniReply();
    }
    return false;
}

#endif  // BODYCLOCK_MNN

}  // namespace voice
}  // namespace bodyclock
