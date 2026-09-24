// yoai-cli -- a desktop harness for the core's C ABI.
//
// Not a product: it exists so the library can be exercised on a workstation,
// against a real provider, without a phone in the loop. It talks to the core
// exactly the way a host app does -- register a provider, hand over the whole
// transcript, stream the events back -- so a turn that works here has proven the
// same path the HarmonyOS module uses.
//
//   yoai-cli --base-url URL --key KEY --model NAME ["one question"]
//
// Each flag falls back to an environment variable (BODYCLOCK_BASE_URL, BODYCLOCK_API_KEY,
// BODYCLOCK_MODEL), which is the sane way to pass a key: a flag lands in the shell
// history and in `ps`.
//
// With a question argument it runs that one turn and exits. Without one it goes
// interactive and keeps the transcript, so multi-turn context is exercised too.

#include "bodyclock.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* kProviderId = "cli";

struct Options {
    std::string base_url;
    std::string api_key;
    std::string model;
    std::string system_prompt;
    std::string question;      // empty => interactive
    bool        show_thinking = true;
    bool        show_usage    = false;
};

std::string from_env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

void usage() {
    std::cerr <<
        "usage: yoai-cli [options] [\"question\"]\n"
        "\n"
        "  --base-url URL   OpenAI-compatible API root, no /chat/completions\n"
        "                   (env BODYCLOCK_BASE_URL)\n"
        "  --key KEY        bearer token (env BODYCLOCK_API_KEY)\n"
        "  --model NAME     model to send in the request (env BODYCLOCK_MODEL)\n"
        "  --system TEXT    system prompt\n"
        "  --no-thinking    drop reasoning output instead of dimming it\n"
        "  --usage          print the token/cost summary after each turn\n"
        "  --debug          verbose core logging\n"
        "\n"
        "With no question, reads turns from stdin and keeps the transcript.\n";
}

// Returns false when the arguments are unusable, having already explained why.
bool parse_args(int argc, char** argv, Options* opt) {
    opt->base_url = from_env("BODYCLOCK_BASE_URL");
    opt->api_key  = from_env("BODYCLOCK_API_KEY");
    opt->model    = from_env("BODYCLOCK_MODEL");

    bool debug = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        // A flag that needs a value but sits last would otherwise read argv
        // out of bounds.
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "yoai-cli: " << what << " needs a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--base-url") {
            const char* v = next("--base-url"); if (!v) return false;
            opt->base_url = v;
        } else if (a == "--key") {
            const char* v = next("--key"); if (!v) return false;
            opt->api_key = v;
        } else if (a == "--model") {
            const char* v = next("--model"); if (!v) return false;
            opt->model = v;
        } else if (a == "--system") {
            const char* v = next("--system"); if (!v) return false;
            opt->system_prompt = v;
        } else if (a == "--no-thinking") {
            opt->show_thinking = false;
        } else if (a == "--usage") {
            opt->show_usage = true;
        } else if (a == "--debug") {
            debug = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return false;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "yoai-cli: unknown option " << a << "\n";
            return false;
        } else {
            opt->question = a;
        }
    }

    bodyclock_set_log_level(debug ? "debug" : "warn");

    if (opt->base_url.empty() || opt->api_key.empty() || opt->model.empty()) {
        std::cerr << "yoai-cli: --base-url, --key and --model are all required "
                     "(or their BODYCLOCK_* environment variables)\n\n";
        usage();
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------

// Escape one string as a JSON string body (no surrounding quotes). Hand-rolled
// because the CLI has no business linking a JSON writer to build three fields,
// and the transcript is the only JSON it produces.
void append_json_string(std::string* out, const std::string& s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"':  *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\n': *out += "\\n";  break;
            case '\r': *out += "\\r";  break;
            case '\t': *out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    *out += "\\u00";
                    *out += kHex[(c >> 4) & 0xf];
                    *out += kHex[c & 0xf];
                } else {
                    *out += static_cast<char>(c);
                }
        }
    }
}

struct Message {
    std::string role;
    std::string content;
};

std::string to_messages_json(const std::vector<Message>& history) {
    std::string json = "[";
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (i) json += ',';
        json += "{\"role\":\"";
        append_json_string(&json, history[i].role);
        json += "\",\"content\":\"";
        append_json_string(&json, history[i].content);
        json += "\"}";
    }
    json += ']';
    return json;
}

//------------------------------------------------------------------------------

// What one turn accumulates. The reply is collected as well as printed, because
// it has to go back into the transcript for the next turn.
struct TurnState {
    std::string reply;
    bool        in_thinking   = false;
    bool        show_thinking = true;
    bool        show_usage    = false;
};

int on_event(const char* event_type, const char* content, void* user_data) {
    auto* st = static_cast<TurnState*>(user_data);

    if (std::strcmp(event_type, "reply") == 0) {
        if (st->in_thinking) { std::cout << "\n"; st->in_thinking = false; }
        std::cout << content << std::flush;
        st->reply += content;
    } else if (std::strcmp(event_type, "thinking") == 0) {
        if (!st->show_thinking) return 1;
        if (!st->in_thinking) { std::cout << "[thinking] " << std::flush; st->in_thinking = true; }
        std::cout << content << std::flush;
    } else if (std::strcmp(event_type, "usage") == 0) {
        if (st->show_usage) std::cout << "\n[usage] " << content << std::flush;
    } else if (std::strcmp(event_type, "error") == 0) {
        std::cout << "\n[error] " << content << std::flush;
    } else {
        // queryTitle / queryArguments / queryDetail: reserved for tool use,
        // which nothing registers yet.
        std::cout << "\n[" << event_type << "] " << content << std::flush;
    }
    return 1;   // non-zero keeps the stream running
}

// Run one turn and append the reply to `history`. Returns false if the turn
// failed, in which case the failed exchange is left out of the transcript --
// replaying a turn the provider rejected would just fail again.
bool run_turn(const Options& opt, std::vector<Message>* history) {
    TurnState st;
    st.show_thinking = opt.show_thinking;
    st.show_usage    = opt.show_usage;

    const std::string json = to_messages_json(*history);
    const int rc = bodyclock_chat(kProviderId, json.c_str(),
                             opt.system_prompt.empty() ? nullptr : opt.system_prompt.c_str(),
                             /*grammar=*/nullptr,
                             on_event, &st);
    std::cout << "\n";

    if (rc != 0) {
        history->pop_back();
        return false;
    }
    Message m;
    m.role    = "assistant";
    m.content = st.reply;
    history->push_back(m);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) return 2;

    if (bodyclock_provider_set(kProviderId, opt.base_url.c_str(), opt.api_key.c_str(),
                          opt.model.c_str(), nullptr) != 0) {
        std::cerr << "yoai-cli: could not register the provider\n";
        return 1;
    }

    std::vector<Message> history;

    if (!opt.question.empty()) {
        Message m;
        m.role    = "user";
        m.content = opt.question;
        history.push_back(m);
        return run_turn(opt, &history) ? 0 : 1;
    }

    std::cerr << "bodyclock " << bodyclock_version() << " -- " << opt.model
              << " @ " << opt.base_url << "\n"
                 "Type a message, or Ctrl-D to quit.\n";
    std::string line;
    while (true) {
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        Message m;
        m.role    = "user";
        m.content = line;
        history.push_back(m);
        run_turn(opt, &history);
    }
    return 0;
}
