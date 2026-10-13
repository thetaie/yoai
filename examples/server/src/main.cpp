// yoai-server -- text in, speech out, over a WebSocket.
//
//   yoai-server <talker.gguf> <tokenizer.gguf> --voice <reference.spk> [options]
//
// Loads one Reader, configures it the way the desktop does, and serves it with
// tts_server.cpp. The protocol is in examples/server/README.md. Ctrl-C, or
// SIGTERM from a container runtime, stops it; a stream still running is
// cancelled first.

#include "console.hpp"

#include "tts_server.hpp"
#include "utils/log.hpp"
#include "voice/tts/reader.hpp"

#include <cctype>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

using yoai::server::TtsServer;
using yoai::server::TtsServerOptions;
using yoai::voice::QttsSampling;
using yoai::voice::Reader;
using yoai::voice::SplitOptions;

namespace {

void err(const std::string& s) { cli::write(stderr, s); }

void usage() {
    err("usage: yoai-server <talker.gguf> <tokenizer.gguf> [options]\n"
        "\n"
        "voice, one of, by checkpoint\n"
        "  --voice FILE       base: the reference voice, a .spk or a mono 16-bit\n"
        "                     24 kHz .wav\n"
        "  --speaker NAME     custom_voice: a named speaker\n"
        "  --instruct TEXT    voice_design: the voice, described in words\n"
        "\n"
        "network\n"
        "  --host ADDR        interface to bind (default: every interface)\n"
        "  --port N           default 8765\n"
        "\n"
        "reading\n"
        "  --seed N           default 101\n"
        "  --temperature T    the talker's sampling temperature (default 1.3)\n"
        "  --threads N        engine threads (default: upstream's choice)\n"
        "  --pieces           cut each reply into pieces, each its own generation,\n"
        "                     instead of reading it as one generation that follows\n"
        "                     the text. For a writer slower than 12.5 tokens a second\n"
        "  --debug            verbose core logging\n");
}

const std::string* value_of(const std::vector<std::string>& args, std::size_t* i) {
    if (*i + 1 >= args.size()) {
        err("yoai-server: " + args[*i] + " needs a value\n");
        return nullptr;
    }
    return &args[++*i];
}

bool ends_with(const std::string& s, const char* tail) {
    const std::string t(tail);
    if (s.size() < t.size()) {
        return false;
    }
    std::string end = s.substr(s.size() - t.size());
    for (std::size_t k = 0; k < end.size(); ++k) {
        end[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(end[k])));
    }
    return end == t;
}

TtsServer* g_server = nullptr;

void on_signal(int) {
    if (g_server) {
        g_server->stop();
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = cli::utf8_args(argc, argv);

    std::vector<std::string> paths;
    std::string      voice, speaker, instruct;
    TtsServerOptions opt;
    long long        seed        = 101;
    float            temperature = 1.3f;
    int              threads     = 0;
    bool             pieces      = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const std::string* v = nullptr;
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--voice") {
            if (!(v = value_of(args, &i))) return 2;
            voice = *v;
        } else if (a == "--speaker") {
            if (!(v = value_of(args, &i))) return 2;
            speaker = *v;
        } else if (a == "--instruct") {
            if (!(v = value_of(args, &i))) return 2;
            instruct = *v;
        } else if (a == "--host") {
            if (!(v = value_of(args, &i))) return 2;
            opt.host = *v;
        } else if (a == "--port") {
            if (!(v = value_of(args, &i))) return 2;
            opt.port = std::atoi(v->c_str());
        } else if (a == "--seed") {
            if (!(v = value_of(args, &i))) return 2;
            seed = std::atoll(v->c_str());
        } else if (a == "--temperature") {
            if (!(v = value_of(args, &i))) return 2;
            temperature = static_cast<float>(std::atof(v->c_str()));
        } else if (a == "--threads") {
            if (!(v = value_of(args, &i))) return 2;
            threads = std::atoi(v->c_str());
        } else if (a == "--pieces") {
            pieces = true;
        } else if (a == "--debug") {
            yoai::utils::set_log_level(yoai::utils::LogLevel::Debug);
        } else if (!a.empty() && a[0] == '-') {
            err("yoai-server: unknown option " + a + "\n");
            usage();
            return 2;
        } else {
            paths.push_back(a);
        }
    }
    if (paths.size() != 2) {
        usage();
        return 2;
    }
    if (opt.port <= 0 || opt.port > 65535) {
        err("yoai-server: --port must be 1 to 65535\n");
        return 2;
    }

    Reader reader;
    reader.set_threads(threads);
    if (!reader.load(paths[0], paths[1], 1)) {
        err("yoai-server: " + reader.last_error() + "\n");
        return 1;
    }

    // The voice. A base checkpoint takes neither an instruct nor a speaker,
    // and unanchored its pieces drift in register -- see voice/tts/reader.hpp
    // -- so it is refused without one rather than served in a voice that
    // wanders.
    const std::string type = reader.model_type();
    if (type == "base") {
        if (voice.empty()) {
            err("yoai-server: a base checkpoint needs --voice, a .spk or a .wav\n");
            return 2;
        }
        const bool ok = ends_with(voice, ".wav") ? reader.set_reference_voice(voice)
                                                 : reader.load_reference_embedding(voice);
        if (!ok) {
            err("yoai-server: --voice: " + reader.last_error() + "\n");
            return 1;
        }
    }
    if (!speaker.empty()) {
        reader.set_speaker(speaker);
    }
    if (!instruct.empty()) {
        reader.set_voice(instruct);
    }

    // The desktop's settings, examples/desktop/src/main/reader.ts, where each one says
    // what it was measured against.
    reader.set_seed(seed);
    QttsSampling sampling = reader.sampling();
    sampling.temperature  = temperature;
    reader.set_sampling(sampling);
    SplitOptions split    = reader.split();
    split.min_chars       = 12;
    split.max_chars       = 40;
    split.later_min_chars = 30;
    split.later_max_chars = 80;
    split.stop_chars      = 6;
    split.clause_cuts     = true;
    reader.set_split(split);
    reader.set_streaming_text(true);
    reader.set_eos_hold(2);
    reader.set_whole_reply(!pieces);

    // One passage read and discarded before the port opens. The first
    // synthesis on a GPU compiles its shaders, and the first client would
    // otherwise wait for that: measured, 13.7 s to first audio against 1.9 s
    // for the same stream afterwards.
    if (!reader.read("\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82", nullptr, nullptr, nullptr)) {   // 你好。
        err("yoai-server: warm-up: " + reader.last_error() + "\n");
        return 1;
    }

    TtsServer server(reader);
    g_server = &server;
    std::signal(SIGINT, &on_signal);
    std::signal(SIGTERM, &on_signal);
    const bool ok = server.run(opt);
    g_server = nullptr;
    if (!ok) {
        err("yoai-server: " + server.last_error() + "\n");
        return 1;
    }
    return 0;
}
