// yoai-llama -- a GGUF on llama.cpp, from the command line: chat, or embed.
//
// Not a product: it exists so the on-device models can be tried on a
// workstation without a phone or a host app in the way. It drives the llm/
// classes directly rather than the C ABI -- through the Chat and Embedder
// interfaces, the way the core's own callers do -- because embedding has no C
// entry points, and because a person checking a model wants the numbers the
// ABI flattens away: prefill and decode speed, and how similar two texts are.
//
//   yoai-llama chat  <model.gguf> [options] ["question"]
//   yoai-llama embed <model.gguf> [options] <text> [more text ...]
//
// chat with a question runs one turn and exits. Without one it reads turns from
// stdin and keeps the transcript, so the KV-cache reuse across turns is
// exercised too; /new starts a new conversation.
//
// embed prints each text's vector and how similar the texts are to each other.
// With --query it instead ranks the texts against a query, embedded as a query
// (with the model's instruction) while the texts are embedded as documents --
// which is how search will use it.

#include "console.hpp"

#include "llm/chat/local.hpp"
#include "llm/embed/local.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using yoai::llm::Chat;
using yoai::llm::ChatMessage;
using yoai::llm::EmbedKind;
using yoai::llm::Embedder;
using yoai::llm::Event;
using yoai::llm::EventType;
using yoai::llm::LocalClient;
using yoai::llm::LocalEmbedOptions;
using yoai::llm::LocalEmbedder;
using yoai::llm::LocalOptions;
using yoai::llm::LocalStats;
using yoai::llm::RequestOptions;

namespace {

typedef std::chrono::steady_clock clk;

double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

void out(const std::string& s) { cli::write(stdout, s); }
void err(const std::string& s) { cli::write(stderr, s); }

std::string format(const char* fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

void usage() {
    err("usage: yoai-llama chat  <model.gguf> [options] [\"question\"]\n"
        "       yoai-llama embed <model.gguf> [options] <text> [more text ...]\n"
        "\n"
        "common\n"
        "  -t N             threads (default 4; <= 0 means every core)\n"
        "  --gpu N          layers to offload to a GPU (default 0; 999 for all). Needs\n"
        "                   a GPU backend in the build, e.g. build.py llama tts with\n"
        "                   VULKAN_SDK set\n"
        "  --debug          verbose core logging\n"
        "\n"
        "chat\n"
        "  --system TEXT    system prompt\n"
        "  --thinking       let the model reason before answering (off by default)\n"
        "  --grammar FILE   constrain every reply with a GBNF grammar; greedy decoding\n"
        "  --ctx N          context ceiling in tokens (default 32768)\n"
        "  --predict N      most tokens per reply (default 8192)\n"
        "  --stats          prefill and decode speed after each turn\n"
        "  --warm           prefill the system prompt before the first turn and after\n"
        "                   /new, as the desktop does; --stats then shows the reuse\n"
        "  Interactive: /new starts a new conversation; Ctrl-D (Ctrl-Z, Enter on\n"
        "  Windows) quits.\n"
        "\n"
        "embed\n"
        "  --query TEXT     rank the texts by similarity to TEXT instead\n"
        "  --task TEXT      the instruction put in front of the query\n"
        "  --ctx N          longest text in tokens (default 2048)\n"
        "  --vectors        print every vector in full, one JSON array per line\n");
}

// Pulls the value of a flag that takes one, advancing `i`. Null, having said
// why, when the flag is last.
const std::string* value_of(const std::vector<std::string>& args, std::size_t* i) {
    if (*i + 1 >= args.size()) {
        err("yoai-llama: " + args[*i] + " needs a value\n");
        return nullptr;
    }
    return &args[++*i];
}

//------------------------------------------------------------------------------
// chat
//------------------------------------------------------------------------------

struct ChatArgs {
    LocalOptions    local;
    std::string     system;
    std::string     question;
    RequestOptions  req;
    bool            stats = false;
    bool            warm  = false;
};

// Prefill the system prompt now, reporting how long that took -- the time a
// first turn no longer spends before its reply.
void warm_prompt(LocalClient& client, const ChatArgs& a) {
    if (!a.warm) {
        return;
    }
    std::string why;
    const clk::time_point t0 = clk::now();
    if (!client.warm(a.system, why)) {
        err("yoai-llama: warm failed: " + why + "\n");
        return;
    }
    err(format("warmed the system prompt in %.0f ms\n", ms_since(t0)));
}

// What one turn accumulates: the reply goes back into the transcript.
struct Turn {
    std::string reply;
    bool        in_thinking = false;
};

void print_stats(const LocalStats& s) {
    const int fresh = s.prompt_tokens - s.reused_tokens;
    err(format("[stats] prompt %d tokens (%d reused)", s.prompt_tokens, s.reused_tokens));
    err(format(", prefill %.0f ms (%.1f tok/s)", s.prefill_ms,
               s.prefill_ms > 0 ? fresh * 1000.0 / s.prefill_ms : 0.0));
    err(format(", %d tokens decoded (%.1f tok/s)", s.decoded_tokens,
               s.decode_ms > 0 ? s.decoded_tokens * 1000.0 / s.decode_ms : 0.0));
    err(format(", ctx %d, threads %d", s.n_ctx, s.n_threads));
    if (s.load_ms > 0) {
        err(format(", load %.0f ms", s.load_ms));
    }
    err("\n");
}

// One turn over `history`, whose last message is the user's. On success the
// reply is appended; on failure the user's message is taken back out, since
// replaying a turn that failed would only fail again.
bool run_turn(LocalClient& client, const ChatArgs& a, std::vector<ChatMessage>* history) {
    Turn t;
    Chat& chat = client;
    const bool ok = chat.ainvoke(*history, a.system, [&t](const Event& e) -> bool {
        switch (e.type) {
            case EventType::Reply:
                if (t.in_thinking) { out("\n"); t.in_thinking = false; }
                out(e.content);
                t.reply += e.content;
                break;
            case EventType::Thinking:
                if (!t.in_thinking) { out("[thinking] "); t.in_thinking = true; }
                out(e.content);
                break;
            case EventType::Error:
                out("\n[error] " + e.content);
                break;
            default:   // CostStatistics: --stats prints the fuller story
                break;
        }
        return true;
    }, yoai::llm::UserContext(), a.req);
    out("\n");

    if (!ok) {
        history->pop_back();
        return false;
    }
    if (a.stats) {
        print_stats(client.stats());
    }
    ChatMessage m;
    m.role    = "assistant";
    m.content = t.reply;
    history->push_back(m);
    return true;
}

int chat_main(const std::vector<std::string>& args) {
    ChatArgs a;
    a.local.model_path = args[2];
    a.local.n_threads  = 4;
    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string& s = args[i];
        const std::string* v = nullptr;
        if (s == "-t") {
            if (!(v = value_of(args, &i))) return 2;
            a.local.n_threads = std::atoi(v->c_str());
        } else if (s == "--gpu") {
            if (!(v = value_of(args, &i))) return 2;
            a.local.n_gpu_layers = std::atoi(v->c_str());
        } else if (s == "--system") {
            if (!(v = value_of(args, &i))) return 2;
            a.system = *v;
        } else if (s == "--thinking") {
            a.local.thinking = 1;
        } else if (s == "--grammar") {
            if (!(v = value_of(args, &i))) return 2;
            std::ifstream f(v->c_str(), std::ios::binary);
            if (!f) {
                err("yoai-llama: cannot read " + *v + "\n");
                return 2;
            }
            std::stringstream ss;
            ss << f.rdbuf();
            a.req.grammar = ss.str();
        } else if (s == "--ctx") {
            if (!(v = value_of(args, &i))) return 2;
            a.local.n_ctx_max = std::atoi(v->c_str());
        } else if (s == "--predict") {
            if (!(v = value_of(args, &i))) return 2;
            a.local.n_predict = std::atoi(v->c_str());
        } else if (s == "--stats") {
            a.stats = true;
        } else if (s == "--warm") {
            a.warm = true;
        } else if (s == "--debug") {
            yoai::utils::set_log_level("debug");
        } else if (!s.empty() && s[0] == '-') {
            err("yoai-llama: unknown option " + s + "\n");
            return 2;
        } else {
            a.question = s;
        }
    }

    LocalClient client(a.local);
    std::string why;
    const clk::time_point t0 = clk::now();
    if (!client.load(why)) {
        err("yoai-llama: " + why + "\n");
        return 1;
    }
    err(format("loaded in %.0f ms\n", ms_since(t0)));
    warm_prompt(client, a);

    std::vector<ChatMessage> history;
    if (!a.question.empty()) {
        ChatMessage m;
        m.role    = "user";
        m.content = a.question;
        history.push_back(m);
        return run_turn(client, a, &history) ? 0 : 1;
    }

    err("Type a message; /new starts over; Ctrl-D (Ctrl-Z, Enter on Windows) quits.\n");
    std::string line;
    for (;;) {
        out("\n> ");
        if (!cli::read_line(&line)) {
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (line == "/new") {
            client.forget();
            history.clear();
            err("(new conversation)\n");
            warm_prompt(client, a);
            continue;
        }
        ChatMessage m;
        m.role    = "user";
        m.content = line;
        history.push_back(m);
        run_turn(client, a, &history);
    }
    return 0;
}

//------------------------------------------------------------------------------
// embed
//------------------------------------------------------------------------------

// Every Embedder returns unit vectors, so the dot product is the cosine.
double similarity(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        s += static_cast<double>(a[i]) * b[i];
    }
    return s;
}

std::string json_array(const std::vector<float>& v) {
    std::string s = "[";
    char        buf[32];
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::snprintf(buf, sizeof(buf), i ? ",%.7g" : "%.7g", v[i]);
        s += buf;
    }
    return s + "]";
}

int embed_main(const std::vector<std::string>& args) {
    LocalEmbedOptions        opt;
    std::vector<std::string> texts;
    std::string              query;
    bool                     vectors = false;
    opt.model_path = args[2];
    opt.n_threads  = 4;
    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string& s = args[i];
        const std::string* v = nullptr;
        if (s == "-t") {
            if (!(v = value_of(args, &i))) return 2;
            opt.n_threads = std::atoi(v->c_str());
        } else if (s == "--gpu") {
            if (!(v = value_of(args, &i))) return 2;
            opt.n_gpu_layers = std::atoi(v->c_str());
        } else if (s == "--query") {
            if (!(v = value_of(args, &i))) return 2;
            query = *v;
        } else if (s == "--task") {
            if (!(v = value_of(args, &i))) return 2;
            opt.query_task = *v;
        } else if (s == "--ctx") {
            if (!(v = value_of(args, &i))) return 2;
            opt.n_ctx = std::atoi(v->c_str());
        } else if (s == "--vectors") {
            vectors = true;
        } else if (s == "--debug") {
            yoai::utils::set_log_level("debug");
        } else if (!s.empty() && s[0] == '-') {
            err("yoai-llama: unknown option " + s + "\n");
            return 2;
        } else {
            texts.push_back(s);
        }
    }
    if (texts.empty()) {
        err("yoai-llama: embed needs at least one text\n");
        return 2;
    }

    LocalEmbedder local(opt);
    Embedder&     emb = local;
    std::string   why;
    clk::time_point t0 = clk::now();
    if (!emb.load(why)) {
        err("yoai-llama: " + why + "\n");
        return 1;
    }
    err(format("loaded in %.0f ms, %d dimensions\n", ms_since(t0), emb.dim()));

    std::vector<std::vector<float> > vecs(texts.size());
    t0 = clk::now();
    for (std::size_t i = 0; i < texts.size(); ++i) {
        if (!emb.embed(texts[i], EmbedKind::Document, &vecs[i], why)) {
            err("yoai-llama: text " + std::to_string(i + 1) + ": " + why + "\n");
            return 1;
        }
    }
    err(format("%d texts embedded in %.0f ms\n", static_cast<int>(texts.size()), ms_since(t0)));

    if (vectors) {
        for (std::size_t i = 0; i < vecs.size(); ++i) {
            out(json_array(vecs[i]) + "\n");
        }
        return 0;
    }

    if (!query.empty()) {
        std::vector<float> q;
        if (!emb.embed(query, EmbedKind::Query, &q, why)) {
            err("yoai-llama: query: " + why + "\n");
            return 1;
        }
        std::vector<std::pair<double, std::size_t> > ranked;
        for (std::size_t i = 0; i < vecs.size(); ++i) {
            ranked.push_back(std::make_pair(similarity(q, vecs[i]), i));
        }
        std::sort(ranked.rbegin(), ranked.rend());
        for (std::size_t r = 0; r < ranked.size(); ++r) {
            out(format("%.4f  ", ranked[r].first) + texts[ranked[r].second] + "\n");
        }
        return 0;
    }

    // Each text's first few components, then every pair's similarity.
    for (std::size_t i = 0; i < vecs.size(); ++i) {
        std::string head = format("[%d] ", static_cast<int>(i + 1));
        for (std::size_t k = 0; k < 4 && k < vecs[i].size(); ++k) {
            head += format("%+.4f ", vecs[i][k]);
        }
        out(head + "...  " + texts[i] + "\n");
    }
    if (vecs.size() > 1) {
        out("\nsimilarity\n");
        for (std::size_t i = 0; i < vecs.size(); ++i) {
            std::string row = format("[%d]", static_cast<int>(i + 1));
            for (std::size_t j = 0; j < vecs.size(); ++j) {
                row += format(" %7.4f", similarity(vecs[i], vecs[j]));
            }
            out(row + "\n");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = cli::utf8_args(argc, argv);
    yoai::utils::set_log_level("warn");

    if (args.size() < 3 || (args[1] != "chat" && args[1] != "embed")) {
        usage();
        return 2;
    }
    if (!LocalClient::available()) {
        err("yoai-llama: this build has no llama.cpp -- build it with: python build.py llama\n");
        return 1;
    }
    return args[1] == "chat" ? chat_main(args) : embed_main(args);
}
