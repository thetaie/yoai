// Implementation of the public C ABI (src/bodyclock.h).
//
// Thin by design: the registry is a map, and a remote turn is one
// OpenAIChatClient built from a copy of the matching entry. No session state, no
// config file, no background thread -- everything the core knows, the host told
// it.
//
// The one place that is not stateless is the on-device lane: a LocalClient holds
// several GB of mmapped weights, so it is cached per id and outlives the turn.
// Reloading per turn would make on-device chat unusable, which is why this file
// keeps clients for one kind of entry and not the other.

#include "bodyclock.h"

#include "client/http_client.hpp"
#include "llm/local.hpp"
#include "llm/openai_chat.hpp"
#include "llm/types.hpp"
#include "platform/bundle.hpp"
#include "platform/log.hpp"
#include "platform/membw.hpp"
#include "voice/omni.hpp"
#include "voice/prosody.hpp"
#include "voice/sensevoice.hpp"

#include <chrono>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using bodyclock::llm::ChatMessage;
using bodyclock::llm::Event;
using bodyclock::llm::EventType;
using bodyclock::llm::LocalClient;
using bodyclock::llm::LocalOptions;
using bodyclock::llm::LocalStats;
using bodyclock::llm::OpenAIChatOptions;

// Wall clock, not clock(). These figures are reported to a person as "how long
// it took", and clock() answers a different question: on POSIX it is processor
// time summed across threads, so anything multithreaded reports several seconds
// per second. Measured on a phone, an engine on four threads printed 87.9 s for
// 18.2 s of work. The recognizer takes a thread count too, so the same applies
// to everything timed below.
typedef std::chrono::steady_clock wall_clk;

double wall_seconds_since(wall_clk::time_point started) {
    return std::chrono::duration<double>(wall_clk::now() - started).count();
}

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------

// One entry, of either kind. A tagged struct rather than two maps because the
// id namespace is shared: bodyclock_chat, bodyclock_providers and bodyclock_provider_remove all
// take an id without being told which lane it is, and two maps would let the
// same id exist twice with nothing deciding which one wins.
struct Entry {
    bool              local = false;
    OpenAIChatOptions remote;      // meaningful when !local
    LocalOptions      on_device;   // meaningful when local
};

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

// Guarded by registry_mutex(). Ordered rather than hashed so bodyclock_providers()
// lists ids in a stable order -- a menu that reshuffles itself when a key is
// added would be a UI bug sourced here.
std::map<std::string, Entry>& registry() {
    static std::map<std::string, Entry> r;
    return r;
}

// Guarded by registry_mutex(). One live engine per on-device id, kept because a
// LocalClient owns the loaded weights: dropping it between turns would re-read
// gigabytes every time. Entries are erased whenever their registry entry is
// replaced or removed, so a re-registered path never keeps answering from the
// model it used to point at.
std::map<std::string, std::shared_ptr<LocalClient> >& local_clients() {
    static std::map<std::string, std::shared_ptr<LocalClient> > c;
    return c;
}

// curl's global state must be initialized once before any easy handle is used.
// Left to lazy init it is not thread-safe, and every entry point below can be
// the first one a host calls.
void ensure_curl() {
    static std::once_flag once;
    std::call_once(once, []() { bodyclock::client::HttpClient::global_init(); });
}

// Look up `id` and hand back a COPY of its entry, so the turn that follows is
// unaffected by a concurrent registry edit. Returns false when unknown.
bool lookup(const std::string& id, Entry* out) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = registry().find(id);
    if (it == registry().end()) return false;
    *out = it->second;
    return true;
}

// The engine for one on-device id, built on first use. Returns null when `id` is
// unknown or is a remote provider.
//
// The client is constructed under the lock -- cheap, it only initializes the
// llama.cpp backend -- but the WEIGHTS are not read here: that happens on the
// first ainvoke() or in bodyclock_local_load, both of which run with no lock held.
// Holding the registry mutex across a multi-second, multi-gigabyte read would
// stall every other entry point in this file for the duration.
std::shared_ptr<LocalClient> local_client_for(const std::string& id) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = registry().find(id);
    if (it == registry().end() || !it->second.local) return std::shared_ptr<LocalClient>();

    std::shared_ptr<LocalClient>& slot = local_clients()[id];
    if (!slot) {
        slot.reset(new LocalClient(it->second.on_device));
    }
    return slot;
}

// Drop any cached engine for `id`. Call whenever its entry changes or goes away.
// Expects registry_mutex() to be HELD -- the erase has to be atomic with the
// registry edit that made the engine stale.
//
// Safe to call during a turn: bodyclock_chat holds its own shared_ptr for the
// duration, so erasing here only means the NEXT turn builds a fresh engine. The
// running one finishes against the model it started with, and the weights are
// freed when it lets go.
void forget_local_client(const std::string& id) {
    local_clients().erase(id);
}

//------------------------------------------------------------------------------
// Event mapping
//------------------------------------------------------------------------------

// The C ABI's tag for one internal event type. Distinct from
// llm::to_string(EventType) on purpose: that spelling serves the internal
// logs, while these are the strings a host app switches on, and they must not
// change when the internal enum is renamed.
const char* tag_for(EventType t) {
    switch (t) {
        case EventType::Reply:          return "reply";
        case EventType::Thinking:       return "thinking";
        case EventType::QueryTitle:     return "queryTitle";
        case EventType::QueryArguments: return "queryArguments";
        case EventType::QueryDetail:    return "queryDetail";
        case EventType::CostStatistics: return "usage";
        case EventType::Error:          return "error";
    }
    return "error";
}

std::string usage_json(const bodyclock::llm::CostStatistics& c) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("model");
    w.String(c.model.data(), static_cast<rapidjson::SizeType>(c.model.size()));
    w.Key("inputTokens");   w.Int64(c.input_tokens);
    w.Key("outputTokens");  w.Int64(c.output_tokens);
    w.Key("thoughtTokens"); w.Int64(c.thought_tokens);
    w.Key("totalTokens");   w.Int64(c.total_tokens);
    w.Key("totalCost");     w.Double(c.total_cost);
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

//------------------------------------------------------------------------------
// On-device options
//------------------------------------------------------------------------------

// Read one optional number. Absent, wrong-typed and unparseable all leave the
// default standing: a host that misspells a knob gets the tuned default, not a
// zeroed one, which is the difference between a slightly-off turn and a broken
// engine (n_ctx_max of 0 allocates nothing).
int json_int(const rapidjson::Value& v, const char* key, int fallback) {
    if (!v.HasMember(key)) return fallback;
    const rapidjson::Value& m = v[key];
    if (m.IsInt())    return m.GetInt();
    if (m.IsInt64())  return static_cast<int>(m.GetInt64());
    if (m.IsDouble()) return static_cast<int>(m.GetDouble());
    return fallback;
}

float json_float(const rapidjson::Value& v, const char* key, float fallback) {
    if (!v.HasMember(key)) return fallback;
    const rapidjson::Value& m = v[key];
    if (m.IsNumber()) return static_cast<float>(m.GetDouble());
    return fallback;
}

// Overlay `options_json` onto `opt`, leaving every member it does not mention.
// A malformed object is warned about and ignored rather than failing the
// registration -- same contract as extra_json on the remote lane.
void apply_local_options(const char* options_json, LocalOptions* opt) {
    if (options_json == nullptr || *options_json == '\0') return;

    rapidjson::Document doc;
    doc.Parse(options_json);
    if (doc.HasParseError() || !doc.IsObject()) {
        bodyclock::platform::log_warn(
            "bodyclock_local_set: ignoring options_json, not a JSON object: %.120s", options_json);
        return;
    }

    opt->n_threads       = json_int(doc, "threads",       opt->n_threads);
    opt->n_ctx_max       = json_int(doc, "contextMax",    opt->n_ctx_max);
    opt->n_predict       = json_int(doc, "predict",       opt->n_predict);
    opt->n_reply_reserve = json_int(doc, "replyReserve",  opt->n_reply_reserve);
    opt->thinking        = json_int(doc, "thinking",      opt->thinking);
    opt->top_k           = json_int(doc, "topK",          opt->top_k);
    opt->repeat_last_n   = json_int(doc, "repeatLastN",   opt->repeat_last_n);

    opt->temperature     = json_float(doc, "temperature",   opt->temperature);
    opt->top_p           = json_float(doc, "topP",          opt->top_p);
    opt->repeat_penalty  = json_float(doc, "repeatPenalty", opt->repeat_penalty);
}

// The last path segment, for labelling a local turn's usage event. The host's id
// would be the other candidate, but a GGUF's file name is what the user actually
// recognizes ("Qwen3-4B-Q4_K_M.gguf") and it is the same thing the remote lane
// reports there: the model, not the entry.
std::string file_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

//------------------------------------------------------------------------------

// Parse the host's transcript. An entry missing role or content is SKIPPED
// rather than failing the turn: one malformed row out of fifty should cost the
// user that row, not their message. Returns false only when the argument is not
// a JSON array at all.
bool parse_messages(const char* json, std::vector<ChatMessage>* out) {
    rapidjson::Document doc;
    doc.Parse(json);
    if (doc.HasParseError() || !doc.IsArray()) return false;

    for (const auto& m : doc.GetArray()) {
        if (!m.IsObject()) continue;
        if (!m.HasMember("role") || !m["role"].IsString()) continue;
        if (!m.HasMember("content") || !m["content"].IsString()) continue;
        ChatMessage msg;
        msg.role.assign(m["role"].GetString(), m["role"].GetStringLength());
        msg.content.assign(m["content"].GetString(), m["content"].GetStringLength());
        out->push_back(std::move(msg));
    }
    return true;
}

// Backing store for bodyclock_providers()' return value. Thread-local so two threads
// listing at once do not overwrite each other's buffer.
std::string& providers_buffer() {
    static thread_local std::string buf;
    return buf;
}

bool blank(const char* s) { return s == nullptr || *s == '\0'; }

}  // namespace

//------------------------------------------------------------------------------
// Diagnostics
//------------------------------------------------------------------------------

extern "C" const char* bodyclock_version(void) {
    return "0.1.0";
}

extern "C" void bodyclock_set_log_level(const char* level) {
    if (blank(level)) return;
    bodyclock::platform::set_log_level(level);
}

//------------------------------------------------------------------------------
// Provider registry
//------------------------------------------------------------------------------

extern "C" int bodyclock_provider_set(const char* id,
                                 const char* base_url,
                                 const char* api_key,
                                 const char* model,
                                 const char* extra_json) {
    if (blank(id) || blank(base_url) || blank(api_key) || blank(model)) {
        bodyclock::platform::log_warn("bodyclock_provider_set: id, base_url, api_key and model are all required");
        return 1;
    }

    OpenAIChatOptions opt;
    opt.base_url = base_url;
    opt.api_key  = api_key;
    opt.model    = model;
    if (!blank(extra_json)) opt.extra_body_json = extra_json;

    // Trailing slash on the base URL would produce "…//chat/completions", which
    // some gateways 404 on. Cheap to fix here, once, rather than in every host.
    while (!opt.base_url.empty() && opt.base_url.back() == '/') opt.base_url.pop_back();

    Entry entry;
    entry.local  = false;
    entry.remote = std::move(opt);

    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[id] = std::move(entry);
        // This id may have been an on-device model a moment ago; its engine is
        // now unreachable and would otherwise hold gigabytes for nothing.
        forget_local_client(id);
    }
    // The key is deliberately absent from this line.
    bodyclock::platform::log_info("provider registered: %s -> %s (%s)", id, base_url, model);
    return 0;
}

extern "C" int bodyclock_provider_remove(const char* id) {
    if (blank(id)) return 0;
    std::lock_guard<std::mutex> lock(registry_mutex());
    forget_local_client(id);
    return registry().erase(id) > 0 ? 1 : 0;
}

extern "C" void bodyclock_provider_clear(void) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().clear();
    local_clients().clear();
}

extern "C" const char* bodyclock_providers(void) {
    std::string& buf = providers_buffer();
    buf.clear();
    std::lock_guard<std::mutex> lock(registry_mutex());
    for (auto it = registry().begin(); it != registry().end(); ++it) {
        if (!buf.empty()) buf += '\n';
        buf += it->first;
    }
    return buf.c_str();
}

//------------------------------------------------------------------------------
// On-device models
//------------------------------------------------------------------------------

extern "C" int bodyclock_local_available(void) {
    return LocalClient::available() ? 1 : 0;
}

extern "C" int bodyclock_local_set(const char* id,
                              const char* model_path,
                              const char* options_json) {
    if (blank(id) || blank(model_path)) {
        bodyclock::platform::log_warn("bodyclock_local_set: id and model_path are both required");
        return 1;
    }

    LocalOptions opt;
    opt.model_path = model_path;
    apply_local_options(options_json, &opt);

    Entry entry;
    entry.local     = true;
    entry.on_device = opt;

    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry()[id] = std::move(entry);
        // Whatever was cached under this id answered for the OLD options -- a
        // different file, or the same file with a different context size. The
        // next turn builds a fresh engine from what was just registered.
        forget_local_client(id);
    }
    bodyclock::platform::log_info("on-device model registered: %s -> %s", id, model_path);
    return 0;
}

extern "C" int bodyclock_local_load(const char* id) {
    if (blank(id)) return 1;

    // Outside the registry lock, deliberately: this reads gigabytes.
    std::shared_ptr<LocalClient> client = local_client_for(id);
    if (!client) {
        bodyclock::platform::log_warn("bodyclock_local_load: %s is not an on-device model", id);
        return 1;
    }

    std::string err;
    if (!client->load(err)) {
        bodyclock::platform::log_warn("bodyclock_local_load(%s): %s", id, err.c_str());
        return 1;
    }
    return 0;
}

extern "C" int bodyclock_local_set_threads(const char* id, int threads) {
    if (blank(id)) return 1;

    std::shared_ptr<LocalClient> client = local_client_for(id);
    if (!client) {
        bodyclock::platform::log_warn(
            "bodyclock_local_set_threads: %s is not an on-device model", id);
        return 1;
    }
    client->setThreads(threads);
    // Logged rather than returned, because the number that matters is the one
    // the NEXT "on-device turn:" line was produced with, and the two lines are
    // read together.
    bodyclock::platform::log_info("on-device threads: %s -> %d", id, threads);
    return 0;
}

extern "C" const char* bodyclock_memory_bandwidth(int threads, int mib, int passes) {
    bodyclock::platform::MemoryBandwidth m;
    if (!bodyclock::platform::measure_memory_bandwidth(threads, mib, passes, &m)) {
        return nullptr;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("threads");     w.Int(m.threads);
    w.Key("bytes");       w.Double(static_cast<double>(m.bytes));
    w.Key("passes");      w.Int(m.passes);
    w.Key("seconds");     w.Double(m.seconds);
    // Decimal GB, matching the 19.5 this is here to check. Mixing GB and GiB is
    // a silent 7% and has already produced one wrong table in design/voice.md.
    w.Key("gbPerSecond"); w.Double(m.gb_per_s);
    w.EndObject();

    // Thread-local for the same reason the others are: two callers must not
    // overwrite each other's answer.
    static thread_local std::string out;
    out = buf.GetString();
    return out.c_str();
}

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

extern "C" int bodyclock_chat(const char* provider_id,
                         const char* messages_json,
                         const char* system_prompt,
                         const char* grammar,
                         bodyclock_chat_handler on_event,
                         void* user_data) {
    if (on_event == nullptr) return 1;

    // Every failure from here on is reported through the handler as well as the
    // return code: the host has a place to show text and no way to turn an int
    // into one.
    auto fail = [&](const std::string& msg) {
        bodyclock::platform::log_warn("bodyclock_chat: %s", msg.c_str());
        on_event("error", msg.c_str(), user_data);
        return 1;
    };

    if (blank(provider_id))   return fail("no provider given");
    if (blank(messages_json)) return fail("no messages given");

    Entry entry;
    if (!lookup(provider_id, &entry)) {
        return fail(std::string("unknown provider: ") + provider_id);
    }

    std::vector<ChatMessage> messages;
    if (!parse_messages(messages_json, &messages)) {
        return fail("messages must be a JSON array");
    }
    if (messages.empty()) return fail("no usable messages");

    const std::string system = blank(system_prompt) ? std::string()
                                                    : std::string(system_prompt);

    bodyclock::llm::RequestOptions req;
    if (!blank(grammar)) {
        req.grammar = grammar;
    }

    // One handler for both lanes: the clients speak the same Event vocabulary, so
    // the only per-lane work is which client runs the turn.
    const bodyclock::llm::EventHandler forward = [&](const Event& e) -> bool {
        const std::string payload = (e.type == EventType::CostStatistics)
                                  ? usage_json(e.cost)
                                  : e.content;
        return on_event(tag_for(e.type), payload.c_str(), user_data) != 0;
    };

    if (entry.local) {
        std::shared_ptr<LocalClient> client = local_client_for(provider_id);
        if (!client) {
            // Only reachable if the entry was replaced between the lookup above
            // and here, which is legal -- the host may edit the registry mid-turn.
            return fail(std::string("no on-device engine for: ") + provider_id);
        }

        const bool ok = client->ainvoke(messages, system, forward,
                                        bodyclock::llm::UserContext(), req);

        // The engine reports token counts through stats() rather than as an event,
        // because it has no wire protocol to carry one. Turning them into the same
        // "usage" event the remote lane ends with is what lets a host show one
        // token counter instead of two. Skipped when the turn failed or was
        // cancelled: a partial count presented as a total is worse than none.
        if (ok) {
            const LocalStats st = client->stats();
            bodyclock::llm::CostStatistics cost;
            cost.model         = file_name(entry.on_device.model_path);
            cost.input_tokens  = st.prompt_tokens;
            cost.output_tokens = st.decoded_tokens;
            cost.total_tokens  = st.prompt_tokens + st.decoded_tokens;
            cost.total_cost    = 0.0;   // nobody was billed; the tokens still matter
            const std::string payload = usage_json(cost);
            on_event("usage", payload.c_str(), user_data);

            // threads and the two rates are on this line rather than derived by
            // whoever reads the log: a sweep is compared across many of these,
            // and pairing each one with a separate "threads ->" line breaks the
            // moment another turn interleaves. st.n_threads is what the turn
            // ACTUALLY ran with, after <= 0 has been resolved to a core count.
            bodyclock::platform::log_info(
                "on-device turn: %d prompt (%d reused) + %d decoded tokens, ctx %d, "
                "threads %d, load %.0fms prefill %.0fms (%.1f tok/s) "
                "decode %.0fms (first %.0fms, then %.1f tok/s), %d msgs dropped",
                st.prompt_tokens, st.reused_tokens, st.decoded_tokens, st.n_ctx,
                st.n_threads, st.load_ms,
                st.prefill_ms,
                st.prefill_ms > 0
                    ? (st.prompt_tokens - st.reused_tokens) * 1000.0 / st.prefill_ms : 0.0,
                st.decode_ms, st.first_token_ms,
                // The rate AFTER the first token, which is the one that predicts
                // a longer reply. Dividing the whole span by the whole count
                // mixes in a per-turn cost that does not repeat, and on an
                // eleven-token reply that cost is most of the span.
                (st.decode_ms - st.first_token_ms) > 0 && st.decoded_tokens > 1
                    ? (st.decoded_tokens - 1) * 1000.0 /
                          (st.decode_ms - st.first_token_ms)
                    : 0.0,
                st.dropped_msgs);
        }
        return ok ? 0 : 1;
    }

    ensure_curl();

    bodyclock::llm::OpenAIChatClient client(entry.remote);
    const bool ok = client.ainvoke(messages, system, forward,
                                   bodyclock::llm::UserContext(), req);

    return ok ? 0 : 1;
}

//------------------------------------------------------------------------------
// Voice
//------------------------------------------------------------------------------

namespace {

// One recognizer for the process, because the weights are 254 MB and reloading
// them per recording would make voice input unusable -- the same argument the
// on-device LLM lane makes for caching its clients.
//
// The mutex is not an optimization. A SenseVoice instance owns a compute buffer
// that a pass writes through, so two recordings analysed at once would corrupt
// each other's graph. Analysis therefore serializes, which is the honest
// behaviour for something that takes a second and a half.
std::mutex& voice_mutex() {
    static std::mutex m;
    return m;
}

bodyclock::voice::SenseVoice& recognizer() {
    static bodyclock::voice::SenseVoice sv;
    return sv;
}

bool& voice_loaded() {
    static bool loaded = false;
    return loaded;
}

// Thread-local for the same reason bodyclock_providers()' buffer is: two threads
// asking at once must not overwrite each other's answer.
std::string& voice_buffer() {
    static thread_local std::string buf;
    return buf;
}

// Sentence- and clause-ending marks, in the vocabulary's own spelling. Listed
// rather than derived from a Unicode category because the transcript is this
// model's output, not arbitrary text: this is the set it actually emits.
//
// Spelled as escapes rather than as the characters themselves. The file is
// UTF-8 and the build passes /utf-8, so either would work -- but these bytes
// have already been double-encoded once on the way into this file, and a
// mis-encoded literal here does not fail loudly: it matches nothing, and the
// clause cut silently falls back to pauses.
const char* const kClauseMarks[] = {
    "\xef\xbc\x8c",   // FULLWIDTH COMMA
    "\xe3\x80\x82",   // IDEOGRAPHIC FULL STOP
    "\xef\xbc\x81",   // FULLWIDTH EXCLAMATION MARK
    "\xef\xbc\x9f",   // FULLWIDTH QUESTION MARK
    "\xef\xbc\x9b",   // FULLWIDTH SEMICOLON
    "\xef\xbc\x9a",   // FULLWIDTH COLON
    "\xe3\x80\x81",   // IDEOGRAPHIC COMMA
    ",", ".", "!", "?", ";", ":",
    0
};

bool is_clause_mark(const std::string& piece) {
    for (int i = 0; kClauseMarks[i]; ++i) {
        if (piece == kClauseMarks[i]) return true;
    }
    return false;
}

// Clause boundaries from the transcript's own timestamps.
//
// This is what makes the analysis work on a note somebody will actually record.
// Cutting at pauses yields about one unit per three seconds, so a ten-second
// note gives three or four -- below the floor where a deviation means anything.
// **Measured** on the reference monologue in ten-second windows: 4, 3, 4 and 3
// by pause, against 4, 9, 7 and 5 by punctuation. People run straight through
// clause boundaries they nonetheless mark when the words are written down.
//
// Raw, one segment per mark. analyze_segments merges them up to
// min_supplied_utterance, which is where the short-unit pitch errors live.
std::vector<std::pair<float, float> > clause_segments(
    const bodyclock::voice::Transcript& transcript, float audio_seconds) {
    std::vector<std::pair<float, float> > segs;
    float start = 0.0f;
    for (size_t i = 0; i < transcript.tokens.size(); ++i) {
        const bodyclock::voice::Token& t = transcript.tokens[i];
        if (t.frame < 0 || t.dropped) {
            continue;   // a query token, or one the stray-capital filter removed
        }
        if (is_clause_mark(t.piece) && t.start > start) {
            segs.push_back(std::make_pair(start, t.start));
            start = t.start;
        }
    }
    if (audio_seconds > start) {
        segs.push_back(std::make_pair(start, audio_seconds));
    }
    return segs;
}

// The marker SentencePiece puts at the start of a word, and the space it means.
void append_piece(std::string* text, const std::string& piece) {
    const char* kWordStart = "\xe2\x96\x81";
    if (piece.size() >= 3 && piece.compare(0, 3, kWordStart) == 0) {
        *text += ' ';
        *text += piece.substr(3);
    } else {
        *text += piece;
    }
}

}  // namespace

extern "C" int bodyclock_voice_available(void) {
    return bodyclock::voice::SenseVoice::available() ? 1 : 0;
}

extern "C" int bodyclock_voice_load(const char* model_path) {
    if (blank(model_path)) {
        bodyclock::platform::log_warn("bodyclock_voice_load: model_path is required");
        return 1;
    }
    std::lock_guard<std::mutex> guard(voice_mutex());
    voice_loaded() = recognizer().load(model_path);
    return voice_loaded() ? 0 : 1;
}

extern "C" int bodyclock_voice_loaded(void) {
    std::lock_guard<std::mutex> guard(voice_mutex());
    return voice_loaded() ? 1 : 0;
}

extern "C" void bodyclock_voice_unload(void) {
    std::lock_guard<std::mutex> guard(voice_mutex());
    // There is no unload on the recognizer itself: the weights go when the
    // process does, or when another load replaces them. Forgetting the flag is
    // what a host can observe, and it is what stops a stale model being used.
    voice_loaded() = false;
}

extern "C" int bodyclock_voice_set_itn(int on) {
    std::lock_guard<std::mutex> guard(voice_mutex());
    if (!voice_loaded()) {
        bodyclock::platform::log_warn("bodyclock_voice_set_itn: no model is loaded");
        return 1;
    }
    return recognizer().set_inverse_text_normalization(on != 0) ? 0 : 1;
}

extern "C" int bodyclock_voice_set_threads(int threads) {
    std::lock_guard<std::mutex> guard(voice_mutex());
    if (!voice_loaded()) {
        bodyclock::platform::log_warn("bodyclock_voice_set_threads: no model is loaded");
        return 1;
    }
    recognizer().set_threads(threads);
    // Logged rather than returned, for the same reason the on-device lane's is:
    // the number that matters is the one the NEXT transcript was produced with,
    // and the two lines are read together.
    bodyclock::platform::log_info("voice threads: -> %d", threads);
    return 0;
}

extern "C" const char* bodyclock_voice_analyze(const short* pcm,
                                          int n_samples,
                                          int sample_rate) {
    if (pcm == nullptr || n_samples <= 0) {
        bodyclock::platform::log_warn("bodyclock_voice_analyze: pcm and n_samples are required");
        return nullptr;
    }
    if (sample_rate != 16000) {
        bodyclock::platform::log_error(
            "bodyclock_voice_analyze: sample_rate is %d, must be 16000 -- the core carries "
            "no resampler, capture at 16 kHz",
            sample_rate);
        return nullptr;
    }

    std::vector<float> samples(static_cast<size_t>(n_samples));
    for (int i = 0; i < n_samples; ++i) {
        samples[i] = pcm[i] / 32768.0f;
    }

    std::lock_guard<std::mutex> guard(voice_mutex());

    bodyclock::voice::Transcript transcript;
    bool   have_transcript    = false;
    double transcribe_seconds = 0.0;
    if (voice_loaded()) {
        const wall_clk::time_point started = wall_clk::now();
        have_transcript = recognizer().transcribe(&samples[0], samples.size(), &transcript);
        transcribe_seconds = wall_seconds_since(started);
    }

    // Cut at the transcript's clauses when there is a transcript, at pauses when
    // there is not. Which one ran is reported: the two are different coordinate
    // systems, and a number from one is not comparable with a number from the
    // other.
    const float audio_seconds = static_cast<float>(n_samples) / sample_rate;
    bodyclock::voice::Recording recording;
    const char* cut = "pause";
    const wall_clk::time_point prosody_started = wall_clk::now();
    bool have_prosody = false;
    if (have_transcript) {
        const std::vector<std::pair<float, float> > segs =
            clause_segments(transcript, audio_seconds);
        if (segs.size() > 1) {
            cut = "clause";
            have_prosody = bodyclock::voice::analyze_segments(
                &samples[0], samples.size(), sample_rate, segs,
                bodyclock::voice::ProsodyOptions(), &recording);
        }
    }
    if (!have_prosody) {
        have_prosody = bodyclock::voice::analyze(
            &samples[0], samples.size(), sample_rate,
            bodyclock::voice::ProsodyOptions(), &recording);
    }
    const double prosody_seconds = wall_seconds_since(prosody_started);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("audioSeconds"); w.Double(static_cast<double>(n_samples) / sample_rate);
    w.Key("sampleRate");   w.Int(sample_rate);

    if (have_transcript) {
        w.Key("transcript");
        w.StartObject();
        w.Key("text");
        w.String(transcript.text.data(), static_cast<rapidjson::SizeType>(transcript.text.size()));
        w.Key("language");
        w.String(transcript.language.data(),
                 static_cast<rapidjson::SizeType>(transcript.language.size()));
        w.Key("emotion");
        w.String(transcript.emotion.data(),
                 static_cast<rapidjson::SizeType>(transcript.emotion.size()));
        w.Key("event");
        w.String(transcript.event.data(),
                 static_cast<rapidjson::SizeType>(transcript.event.size()));
        w.Key("itn");                   w.Bool(transcript.itn);
        w.Key("filteredStrayCapitals"); w.Bool(transcript.filtered_stray_capitals);
        w.Key("seconds");               w.Double(transcribe_seconds);
        w.Key("tokens");
        w.StartArray();
        for (size_t i = 0; i < transcript.tokens.size(); ++i) {
            const bodyclock::voice::Token& t = transcript.tokens[i];
            w.StartObject();
            w.Key("at");    w.Double(t.start);
            w.Key("frame"); w.Int(t.frame);
            w.Key("piece");
            w.String(t.piece.data(), static_cast<rapidjson::SizeType>(t.piece.size()));
            w.Key("dropped"); w.Bool(t.dropped);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }

    if (have_prosody) {
        w.Key("prosody");
        w.StartObject();
        w.Key("seconds"); w.Double(prosody_seconds);
        w.Key("cut");     w.String(cut);
        // Below this the deviations are noise rather than signal. Reported as a
        // flag so the decision is the host's and is visible, instead of the core
        // silently returning numbers that should not be shown.
        w.Key("enough");  w.Bool(recording.utterances.size() >= 3);
        w.Key("baseline");
        w.StartObject();
        w.Key("f0Mean");     w.Double(recording.baseline.f0_mean);
        w.Key("f0Sd");       w.Double(recording.baseline.f0_sd);
        w.Key("rmsMean");    w.Double(recording.baseline.rms_mean);
        w.Key("rmsSd");      w.Double(recording.baseline.rms_sd);
        w.Key("speechMean"); w.Double(recording.baseline.speech_mean);
        w.Key("speechSd");   w.Double(recording.baseline.speech_sd);
        w.EndObject();

        w.Key("utterances");
        w.StartArray();
        for (size_t i = 0; i < recording.utterances.size(); ++i) {
            const bodyclock::voice::Utterance& u = recording.utterances[i];
            const bodyclock::voice::Deviation  d = bodyclock::voice::deviation(recording, i);
            w.StartObject();
            w.Key("start");        w.Double(u.start);
            w.Key("duration");     w.Double(u.duration);
            w.Key("f0Mean");       w.Double(u.f0_mean);
            w.Key("f0P10");        w.Double(u.f0_p10);
            w.Key("f0P90");        w.Double(u.f0_p90);
            w.Key("f0RangeSt");    w.Double(u.f0_range_st);
            w.Key("rmsMean");      w.Double(u.rms_mean);
            w.Key("speechRatio");  w.Double(u.speech_ratio);
            w.Key("voicedFrames"); w.Int(u.voiced_frames);
            // Derived on the way out, never stored: today's weighting of pitch
            // against continuity is a guess, and re-deriving it must not need
            // the audio back.
            w.Key("zF0");         w.Double(d.f0);
            w.Key("zLoudness");   w.Double(d.loudness);
            w.Key("zContinuity"); w.Double(d.continuity);
            w.Key("arousal");     w.Double(bodyclock::voice::arousal(recording, i));

            // What was being said, when there is a transcript to say it with.
            // The two halves are independent measurements of the same seconds,
            // and putting them side by side is the whole point.
            if (have_transcript) {
                std::string said;
                const float end = u.start + u.duration;
                for (size_t k = 0; k < transcript.tokens.size(); ++k) {
                    const bodyclock::voice::Token& t = transcript.tokens[k];
                    if (t.frame < 0 || t.dropped) {
                        continue;   // a query token, or one the filter removed
                    }
                    if (t.start >= u.start && t.start < end) {
                        append_piece(&said, t.piece);
                    }
                }
                w.Key("text");
                w.String(said.data(), static_cast<rapidjson::SizeType>(said.size()));
            }
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }

    w.EndObject();

    voice_buffer().assign(buf.GetString(), buf.GetSize());
    return voice_buffer().c_str();
}

//------------------------------------------------------------------------------
// Omni (MNN)
//------------------------------------------------------------------------------

namespace {

// One engine for the process, guarded, for the same two reasons the recognizer
// is: the weights are gigabytes, and one instance owns compute buffers that a
// call writes through.
std::mutex& omni_mutex() {
    static std::mutex m;
    return m;
}

bodyclock::voice::Omni& omni() {
    static bodyclock::voice::Omni engine;
    return engine;
}

std::string& omni_buffer() {
    static thread_local std::string buf;
    return buf;
}

// Carries the host's C callback across the C++ sink signature. A struct rather
// than two globals because a sink is per-call and the engine is not.
struct OmniSinkBridge {
    bodyclock_omni_audio_handler handler;
    void*                   user_data;
};

bool omni_sink(const float* pcm, size_t n_samples, bool last, void* user_data) {
    OmniSinkBridge* bridge = static_cast<OmniSinkBridge*>(user_data);
    if (!bridge || !bridge->handler) {
        return true;
    }
    return bridge->handler(pcm, static_cast<int>(n_samples), last ? 1 : 0,
                           bridge->user_data) != 0;
}

}  // namespace

extern "C" int bodyclock_omni_available(void) {
    return bodyclock::voice::Omni::available() ? 1 : 0;
}

extern "C" int bodyclock_omni_load(const char* model_dir) {
    if (blank(model_dir)) {
        bodyclock::platform::log_warn("bodyclock_omni_load: model_dir is required");
        return 1;
    }
    std::lock_guard<std::mutex> guard(omni_mutex());
    return omni().load(model_dir) ? 0 : 1;
}

extern "C" int bodyclock_omni_loaded(void) {
    std::lock_guard<std::mutex> guard(omni_mutex());
    return omni().loaded() ? 1 : 0;
}

extern "C" void bodyclock_omni_unload(void) {
    // Nothing to release explicitly: the engine drops its weights when another
    // load replaces them or the process ends. Kept for symmetry with the other
    // lanes, and so a host has one place to say "I am done with this".
    bodyclock::platform::log_info("bodyclock_omni_unload: weights stay until replaced");
}

extern "C" const char* bodyclock_omni_ask(const short* pcm,
                                     int n_samples,
                                     const char* prompt,
                                     bodyclock_omni_audio_handler on_audio,
                                     void* user_data) {
    if (pcm == nullptr || n_samples <= 0 || blank(prompt)) {
        bodyclock::platform::log_warn("bodyclock_omni_ask: pcm, n_samples and prompt are required");
        return nullptr;
    }

    std::vector<float> samples(static_cast<size_t>(n_samples));
    for (int i = 0; i < n_samples; ++i) {
        samples[i] = pcm[i] / 32768.0f;
    }

    std::lock_guard<std::mutex> guard(omni_mutex());

    OmniSinkBridge bridge;
    bridge.handler   = on_audio;
    bridge.user_data = user_data;

    bodyclock::voice::OmniReply reply;
    const bool ok = omni().ask(&samples[0], samples.size(), prompt,
                               on_audio ? omni_sink : 0,
                               on_audio ? &bridge : 0, &reply);
    if (!ok) {
        return nullptr;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("text");
    w.String(reply.text.data(), static_cast<rapidjson::SizeType>(reply.text.size()));
    w.Key("audioSeconds");  w.Double(static_cast<double>(n_samples) / 16000.0);
    w.Key("seconds");       w.Double(reply.seconds);
    w.Key("thinkerSeconds"); w.Double(reply.thinker_seconds);
    w.Key("speechSeconds"); w.Double(reply.speech_seconds);
    w.Key("speechElapsed"); w.Double(reply.speech_elapsed);
    // What a person actually waits before hearing anything, and how many blocks
    // it arrived in. speechRealtime below is the cost of the WHOLE reply; these
    // two say whether that cost is ever experienced.
    w.Key("firstAudioSeconds"); w.Double(reply.first_audio_seconds);
    w.Key("audioBlocks");       w.Int(reply.audio_blocks);
    // The ratio the whole lane turns on: how many seconds of computation buy one
    // second of speech. Above 1 the model cannot talk in real time.
    w.Key("speechRealtime");
    w.Double(reply.speech_seconds > 0.0f ? reply.speech_elapsed / reply.speech_seconds : 0.0);
    w.EndObject();

    omni_buffer().assign(buf.GetString(), buf.GetSize());
    return omni_buffer().c_str();
}

//------------------------------------------------------------------------------
// Model bundles
//------------------------------------------------------------------------------

namespace {

std::string& bundle_buffer() {
    static thread_local std::string buf;
    return buf;
}

// Carries the host's C callback across the C++ progress signature.
struct BundleProgressBridge {
    bodyclock_bundle_progress handler;
    void*                user_data;
};

bool bundle_progress_bridge(const char* name, unsigned long long done,
                            unsigned long long total, void* user_data) {
    BundleProgressBridge* b = static_cast<BundleProgressBridge*>(user_data);
    if (!b || !b->handler) {
        return true;
    }
    const double fraction = total ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    return b->handler(name, fraction, b->user_data) != 0;
}

}  // namespace

extern "C" const char* bodyclock_bundle_list(const char* bundle_path) {
    if (blank(bundle_path)) {
        bodyclock::platform::log_warn("bodyclock_bundle_list: bundle_path is required");
        return nullptr;
    }

    std::vector<bodyclock::platform::BundleEntry> entries;
    unsigned long long total = 0;
    if (!bodyclock::platform::bundle_list(bundle_path, &entries, &total)) {
        return nullptr;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("totalBytes"); w.Uint64(total);
    w.Key("files");
    w.StartArray();
    for (size_t i = 0; i < entries.size(); ++i) {
        w.StartObject();
        w.Key("name");
        w.String(entries[i].name.data(),
                 static_cast<rapidjson::SizeType>(entries[i].name.size()));
        w.Key("bytes"); w.Uint64(entries[i].size);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();

    bundle_buffer().assign(buf.GetString(), buf.GetSize());
    return bundle_buffer().c_str();
}

extern "C" int bodyclock_bundle_extract(const char* bundle_path,
                                   const char* dest_dir,
                                   bodyclock_bundle_progress on_progress,
                                   void* user_data) {
    if (blank(bundle_path) || blank(dest_dir)) {
        bodyclock::platform::log_warn(
            "bodyclock_bundle_extract: bundle_path and dest_dir are both required");
        return 1;
    }

    BundleProgressBridge bridge;
    bridge.handler   = on_progress;
    bridge.user_data = user_data;

    const bool ok = bodyclock::platform::bundle_extract(
        bundle_path, dest_dir,
        on_progress ? bundle_progress_bridge : 0,
        on_progress ? &bridge : 0);
    return ok ? 0 : 1;
}
