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
#include "store/timeline.hpp"
#include "voice/omni.hpp"
#include "voice/prosody.hpp"
#include "voice/qtts.hpp"
#include "voice/reader.hpp"
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

//------------------------------------------------------------------------------
// The day log
//------------------------------------------------------------------------------

using bodyclock::store::Timeline;

std::mutex& timeline_mutex() {
    static std::mutex m;
    return m;
}

// Guarded by timeline_mutex(). A shared_ptr rather than a raw owner so that a
// read already in flight keeps the store alive across a concurrent
// bodyclock_timeline_close(): the pointer is copied out under the lock and the
// COPY is what the call runs against, the same way lookup() copies a registry
// entry so a turn is unaffected by an edit beside it.
std::shared_ptr<Timeline>& timeline_slot() {
    static std::shared_ptr<Timeline> t;
    return t;
}

// The directory it was opened with, so re-opening the same one can be the no-op
// the header promises. Guarded by timeline_mutex().
std::string& timeline_dir() {
    static std::string d;
    return d;
}

// Thread-local, so one thread's refusal is not reported to another as its own.
std::string& timeline_error() {
    static thread_local std::string e;
    return e;
}

// One buffer per read, not one shared: the header promises each read's result
// survives until that SAME function is called again, and a host that holds
// now()'s object while iterating recent() is doing nothing wrong.
std::string& timeline_now_buffer() {
    static thread_local std::string buf;
    return buf;
}
std::string& timeline_recent_buffer() {
    static thread_local std::string buf;
    return buf;
}
std::string& timeline_day_buffer() {
    static thread_local std::string buf;
    return buf;
}
std::string& timeline_iso_buffer() {
    static thread_local std::string buf;
    return buf;
}

// The open store, or null with the reason already recorded. Clearing the error
// here rather than in each entry point is what keeps a stale message from an
// earlier failure from being read as this call's.
std::shared_ptr<Timeline> timeline_checked() {
    timeline_error().clear();
    std::lock_guard<std::mutex> lock(timeline_mutex());
    std::shared_ptr<Timeline> t = timeline_slot();
    if (!t) {
        timeline_error() = "no day log is open -- call bodyclock_timeline_open first";
    }
    return t;
}

void write_json_string(rapidjson::Writer<rapidjson::StringBuffer>& w,
                       const std::string& s) {
    w.String(s.data(), static_cast<rapidjson::SizeType>(s.size()));
}

// One entry, notes and all. The shape is documented in bodyclock.h; keep the
// two in step, because a host reads only the header.
void write_entry(rapidjson::Writer<rapidjson::StringBuffer>& w,
                 const bodyclock::store::Entry& e) {
    w.StartObject();
    w.Key("id");        w.Int64(e.id);
    w.Key("stateId");   write_json_string(w, e.state_id);
    w.Key("open");      w.Bool(e.open());
    w.Key("startedAt"); w.Int64(e.started_at);
    w.Key("startedTz"); w.Int(e.started_tz);
    w.Key("endedAt");   w.Int64(e.ended_at);
    w.Key("endedTz");   w.Int(e.ended_tz);
    w.Key("localDay");  w.Int(e.local_day);
    w.Key("localDayEnd"); w.Int(e.local_day_end);
    w.Key("notes");
    w.StartArray();
    for (size_t i = 0; i < e.notes.size(); ++i) {
        const bodyclock::store::Note& n = e.notes[i];
        w.StartObject();
        w.Key("id");   w.Int64(n.id);
        w.Key("text"); write_json_string(w, n.text);
        w.Key("at");   w.Int64(n.at);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

// A list read -- recent() and day() differ only in which method they call and
// which buffer they land in, so the marshalling is written once.
const char* timeline_entries_json(std::vector<bodyclock::store::Entry>& entries,
                                  std::string& buffer) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for (size_t i = 0; i < entries.size(); ++i) {
        write_entry(w, entries[i]);
    }
    w.EndArray();
    buffer.assign(buf.GetString(), buf.GetSize());
    return buffer.c_str();
}

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
// The day log
//------------------------------------------------------------------------------

extern "C" int bodyclock_timeline_open(const char* dir) {
    timeline_error().clear();
    if (blank(dir)) {
        timeline_error() = "bodyclock_timeline_open: dir is required";
        return 1;
    }

    const std::string want(dir);
    std::lock_guard<std::mutex> lock(timeline_mutex());
    if (timeline_slot() && timeline_dir() == want) {
        return 0;
    }

    // Built BEFORE the old one is dropped is wrong here: two connections to the
    // same file would overlap for the length of the migration. The old store
    // goes first, and a failed open therefore leaves nothing open -- which is
    // the honest outcome, since the host is about to be told it failed.
    timeline_slot().reset();
    timeline_dir().clear();

    std::string err;
    Timeline* opened = Timeline::open(want, &err);
    if (!opened) {
        timeline_error() = err;
        return 1;
    }
    timeline_slot().reset(opened);
    timeline_dir() = want;
    return 0;
}

extern "C" int bodyclock_timeline_opened(void) {
    std::lock_guard<std::mutex> lock(timeline_mutex());
    return timeline_slot() ? 1 : 0;
}

extern "C" void bodyclock_timeline_close(void) {
    std::lock_guard<std::mutex> lock(timeline_mutex());
    timeline_slot().reset();
    timeline_dir().clear();
}

extern "C" int bodyclock_timeline_switch_to(const char* state_id) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return 1;
    std::string err;
    if (!t->switch_to(blank(state_id) ? std::string() : std::string(state_id), &err)) {
        timeline_error() = err;
        return 1;
    }
    return 0;
}

extern "C" int bodyclock_timeline_end(void) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return 1;
    std::string err;
    if (!t->end_current(&err)) {
        timeline_error() = err;
        return 1;
    }
    return 0;
}

extern "C" int bodyclock_timeline_end_at(long long at) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return 1;
    std::string err;
    if (!t->end_at(at, &err)) {
        timeline_error() = err;
        return 1;
    }
    return 0;
}

extern "C" int bodyclock_timeline_undo(void) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return 1;
    std::string err;
    if (!t->undo(&err)) {
        timeline_error() = err;
        return 1;
    }
    return 0;
}

extern "C" int bodyclock_timeline_note_add(const char* text) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return 1;
    std::string err;
    if (!t->note_add(blank(text) ? std::string() : std::string(text), &err)) {
        timeline_error() = err;
        return 1;
    }
    return 0;
}

extern "C" const char* bodyclock_timeline_now(void) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return nullptr;

    bodyclock::store::Now now;
    std::string err;
    if (!t->now(&now, &err)) {
        timeline_error() = err;
        return nullptr;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("current");
    if (now.has_current) {
        write_entry(w, now.current);
    } else {
        w.Null();
    }
    w.Key("lastEnded");
    if (now.has_last_ended) {
        write_entry(w, now.last_ended);
    } else {
        w.Null();
    }
    w.EndObject();

    timeline_now_buffer().assign(buf.GetString(), buf.GetSize());
    return timeline_now_buffer().c_str();
}

extern "C" const char* bodyclock_timeline_recent(int hours) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return nullptr;

    std::vector<bodyclock::store::Entry> entries;
    std::string err;
    if (!t->recent(hours, &entries, &err)) {
        timeline_error() = err;
        return nullptr;
    }
    return timeline_entries_json(entries, timeline_recent_buffer());
}

extern "C" const char* bodyclock_timeline_day(int local_day) {
    std::shared_ptr<Timeline> t = timeline_checked();
    if (!t) return nullptr;

    std::vector<bodyclock::store::Entry> entries;
    std::string err;
    if (!t->day(local_day, &entries, &err)) {
        timeline_error() = err;
        return nullptr;
    }
    return timeline_entries_json(entries, timeline_day_buffer());
}

// The three below are pure arithmetic on the class, so unlike everything else
// in this section they do not need a log to be open and cannot fail for having
// none. They leave timeline_last_error() alone for the same reason: there is
// nothing for them to report and clearing it would discard the message from the
// call the host is probably about to ask about.
extern "C" int bodyclock_timeline_today(void) {
    return Timeline::today_local_day();
}

extern "C" const char* bodyclock_timeline_day_to_iso(int local_day) {
    timeline_iso_buffer() = Timeline::day_to_iso(local_day);
    return timeline_iso_buffer().c_str();
}

extern "C" int bodyclock_timeline_iso_to_day(const char* iso) {
    if (blank(iso)) return -1;
    return Timeline::iso_to_day(iso);
}

extern "C" const char* bodyclock_timeline_last_error(void) {
    return timeline_error().c_str();
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
// Omni (MNN + Qwen2.5-Omni)
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

// Carries the host's C callbacks across the C++ sink signatures. A struct
// rather than globals because the sinks are per-call and the engine is not, and
// one struct for both because the engine gives them one `user_data` between
// them -- they are always two halves of the same host.
struct OmniSinkBridge {
    bodyclock_omni_audio_handler handler;
    bodyclock_omni_text_handler  on_text;
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

void omni_text_sink(const char* text, void* user_data) {
    OmniSinkBridge* bridge = static_cast<OmniSinkBridge*>(user_data);
    if (!bridge || !bridge->on_text) {
        return;
    }
    bridge->on_text(text ? text : "", bridge->user_data);
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

extern "C" int bodyclock_omni_set_dit_steps(int steps) {
    std::lock_guard<std::mutex> guard(omni_mutex());
    return omni().set_dit_steps(steps) ? 0 : 1;
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
                                     bodyclock_omni_text_handler on_text,
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
    bridge.on_text   = on_text;
    bridge.user_data = user_data;

    bodyclock::voice::OmniReply reply;
    const bool ok = omni().ask(&samples[0], samples.size(), prompt,
                               on_audio ? omni_sink : 0,
                               on_text ? omni_text_sink : 0,
                               (on_audio || on_text) ? &bridge : 0, &reply);
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
    // Where speechSeconds went. Zero on a stock MNN, which cannot report it --
    // see third/patches/mnn-talker-context.patch. token2wav is the one
    // dit_steps scales.
    w.Key("talkerPrefillSeconds"); w.Double(reply.talker_prefill_seconds);
    w.Key("talkerDecodeSeconds");  w.Double(reply.talker_decode_seconds);
    w.Key("token2wavSeconds");     w.Double(reply.token2wav_seconds);
    w.Key("codecTokens");          w.Int(reply.codec_tokens);
    // The ratio the whole lane turns on: how many seconds of computation buy one
    // second of speech. Above 1 the model cannot talk in real time.
    w.Key("speechRealtime");
    w.Double(reply.speech_seconds > 0.0f ? reply.speech_elapsed / reply.speech_seconds : 0.0);
    w.EndObject();

    omni_buffer().assign(buf.GetString(), buf.GetSize());
    return omni_buffer().c_str();
}

//------------------------------------------------------------------------------
// Speech output (qwentts.cpp)
//------------------------------------------------------------------------------

namespace {

// One engine for the process, guarded, for the same reasons as the two above:
// the weights are gigabytes and one instance owns compute buffers a call writes
// through.
std::mutex& tts_mutex() {
    static std::mutex m;
    return m;
}

bodyclock::voice::Qtts& tts() {
    static bodyclock::voice::Qtts engine;
    return engine;
}

std::string& tts_buffer() {
    static thread_local std::string buf;
    return buf;
}

// NOT thread_local, unlike the JSON buffer. The error belongs to the engine,
// which is one object behind one mutex, and a host that loads on one thread and
// asks why on another would otherwise get an empty string.
std::string& tts_error() {
    static std::string buf;
    return buf;
}

std::string& tts_model_type() {
    static std::string buf;
    return buf;
}

// Its own buffer rather than sharing tts_buffer(). Both entry points document
// their result as valid until this thread's next call, and with one buffer that
// would make asking for the speaker list quietly invalidate a reply's JSON --
// true to the letter of the contract and a trap in practice.
std::string& tts_speakers_buffer() {
    static thread_local std::string buf;
    return buf;
}

struct TtsSinkBridge {
    bodyclock_tts_audio_handler handler;
    void*                       user_data;
};

bool tts_sink(const float* pcm, size_t n_samples, bool last, void* user_data) {
    TtsSinkBridge* bridge = static_cast<TtsSinkBridge*>(user_data);
    if (!bridge || !bridge->handler) {
        return true;
    }
    return bridge->handler(pcm, static_cast<int>(n_samples), last ? 1 : 0,
                           bridge->user_data) != 0;
}

}  // namespace

extern "C" int bodyclock_tts_available(void) {
    return bodyclock::voice::Qtts::available() ? 1 : 0;
}

extern "C" int bodyclock_tts_load(const char* talker_path, const char* codec_path) {
    if (blank(talker_path) || blank(codec_path)) {
        bodyclock::platform::log_warn("bodyclock_tts_load: both model paths are required");
        return 1;
    }
    std::lock_guard<std::mutex> guard(tts_mutex());
    const bool ok = tts().load(talker_path, codec_path);
    tts_error() = tts().last_error();
    tts_model_type() = ok ? tts().model_type() : std::string();
    return ok ? 0 : 1;
}

extern "C" int bodyclock_tts_loaded(void) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    return tts().loaded() ? 1 : 0;
}

extern "C" void bodyclock_tts_unload(void) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts().unload();
    tts_model_type().clear();
}

extern "C" const char* bodyclock_tts_model_type(void) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts_model_type() = tts().model_type();
    return tts_model_type().c_str();
}

extern "C" void bodyclock_tts_set_voice(const char* instruct) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts().set_voice(instruct ? instruct : "");
}

extern "C" const char* bodyclock_tts_speakers(void) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    const std::vector<std::string> names = tts().speakers();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for (size_t i = 0; i < names.size(); ++i) {
        w.String(names[i].data(), static_cast<rapidjson::SizeType>(names[i].size()));
    }
    w.EndArray();

    tts_speakers_buffer().assign(buf.GetString(), buf.GetSize());
    return tts_speakers_buffer().c_str();
}

extern "C" void bodyclock_tts_set_speaker(const char* name) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts().set_speaker(name ? name : "");
}

extern "C" void bodyclock_tts_set_seed(long long seed) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts().set_seed(seed);
}

extern "C" void bodyclock_tts_set_top_k(int top_k) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts().set_top_k(top_k);
}

extern "C" int bodyclock_tts_set_reference(const char* wav_path, const char* transcript) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    if (blank(wav_path) && blank(transcript)) {
        tts().clear_reference();
        tts_error().clear();
        return 0;
    }
    const bool ok = tts().set_reference(wav_path ? wav_path : "",
                                        transcript ? transcript : "");
    tts_error() = tts().last_error();
    return ok ? 0 : 1;
}

extern "C" float bodyclock_tts_reference_seconds(void) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    return tts().reference_seconds();
}

extern "C" void bodyclock_tts_set_streaming(int on) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts().set_streaming(on != 0);
}

extern "C" void bodyclock_tts_set_threads(int threads) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    tts().set_threads(threads);
}

extern "C" const char* bodyclock_tts_last_error(void) {
    std::lock_guard<std::mutex> guard(tts_mutex());
    return tts_error().c_str();
}

extern "C" const char* bodyclock_tts_say(const char* text,
                                    bodyclock_tts_audio_handler on_audio,
                                    void* user_data) {
    if (blank(text)) {
        bodyclock::platform::log_warn("bodyclock_tts_say: text is required");
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(tts_mutex());

    TtsSinkBridge bridge;
    bridge.handler   = on_audio;
    bridge.user_data = user_data;

    bodyclock::voice::QttsResult result;
    const bool ok = tts().say(text, on_audio ? tts_sink : 0,
                              on_audio ? &bridge : 0, &result);
    tts_error() = tts().last_error();
    if (!ok) {
        return nullptr;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("speechSeconds");     w.Double(result.speech_seconds);
    w.Key("seconds");           w.Double(result.elapsed);
    // The ratio the whole lane turns on: seconds of computation per second of
    // speech. Above 1 it cannot talk in real time.
    w.Key("realtime");
    w.Double(result.speech_seconds > 0.0f ? result.elapsed / result.speech_seconds : 0.0);
    w.Key("firstAudioSeconds"); w.Double(result.first_audio_seconds);
    w.Key("chunks");            w.Int(result.chunks);
    // Which path this run ACTUALLY took, which is not the same as what was
    // asked for: with no handler there is nobody to stream to, so the engine
    // runs buffered whatever the setting says. Reported rather than left to the
    // host, which sets the switch once and then reads several results.
    w.Key("streamed");          w.Bool(on_audio != nullptr && tts().streaming());
    w.Key("peakDbfs");          w.Double(result.peak_dbfs);
    // A description of the delivery's level, with no threshold attached -- see
    // QttsResult::level_drift_db for why there is not one yet.
    w.Key("levelDriftDb");      w.Double(result.level_drift_db);
    w.EndObject();

    tts_buffer().assign(buf.GetString(), buf.GetSize());
    return tts_buffer().c_str();
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


//------------------------------------------------------------------------------
// Reading a reply aloud while it is still being written
//------------------------------------------------------------------------------

namespace {

// A SECOND engine instance over the same weights as the lane above, which is
// why bodyclock.h says to load one or the other. Kept separate rather than
// folded into tts() because that lane works and this one is being judged; the
// day it wins, the other goes rather than the two being merged.
std::mutex& reader_mutex() {
    static std::mutex m;
    return m;
}

bodyclock::voice::Reader& reader() {
    static bodyclock::voice::Reader r;
    return r;
}

std::string& reader_buffer() {
    static thread_local std::string buf;
    return buf;
}

// NOT thread_local, for the reason tts_error() is not: the engine is one object
// and a host that begins on one thread and asks why on another should be told.
std::string& reader_error() {
    static std::string buf;
    return buf;
}

std::string& reader_model_type() {
    static std::string buf;
    return buf;
}

struct ReaderSinkBridge {
    bodyclock_reader_audio_handler handler;
    void*                          user_data;
};

// Held for the length of the stream, because begin() returns and the sink is
// called from threads the Reader owns. A single stream at a time, which the
// Reader enforces on its own side by refusing a second begin().
ReaderSinkBridge& reader_bridge() {
    static ReaderSinkBridge bridge = {0, 0};
    return bridge;
}

bool reader_sink(const float* pcm, size_t n_samples, int sentence, bool last, void* user_data) {
    ReaderSinkBridge* bridge = static_cast<ReaderSinkBridge*>(user_data);
    if (!bridge || !bridge->handler) {
        return true;
    }
    return bridge->handler(pcm, static_cast<int>(n_samples), sentence, last ? 1 : 0,
                           bridge->user_data) != 0;
}

}  // namespace

extern "C" int bodyclock_reader_available(void) {
    return bodyclock::voice::Reader::available() ? 1 : 0;
}

extern "C" int bodyclock_reader_load(const char* talker_path, const char* codec_path,
                                     int voices) {
    if (blank(talker_path) || blank(codec_path)) {
        reader_error() = "talker_path and codec_path are both required";
        bodyclock::platform::log_warn("bodyclock_reader_load: %s", reader_error().c_str());
        return 1;
    }
    std::lock_guard<std::mutex> guard(reader_mutex());
    if (!reader().load(talker_path, codec_path, voices)) {
        reader_error() = reader().last_error();
        return 1;
    }
    reader_error().clear();
    reader_model_type() = reader().model_type();
    return 0;
}

extern "C" int bodyclock_reader_loaded(void) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    return reader().loaded() ? 1 : 0;
}

extern "C" void bodyclock_reader_unload(void) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    reader().unload();
    reader_model_type().clear();
}

extern "C" const char* bodyclock_reader_model_type(void) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    reader_model_type() = reader().model_type();
    return reader_model_type().c_str();
}

extern "C" void bodyclock_reader_set_voice(const char* instruct) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    reader().set_voice(instruct ? instruct : "");
}

extern "C" void bodyclock_reader_set_speaker(const char* name) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    reader().set_speaker(name ? name : "");
}

extern "C" int bodyclock_reader_set_reference_voice(const char* wav_path) {
    if (blank(wav_path)) {
        reader_error() = "a reference voice needs a WAV";
        return 1;
    }
    std::lock_guard<std::mutex> guard(reader_mutex());
    if (!reader().set_reference_voice(wav_path)) {
        reader_error() = reader().last_error();
        return 1;
    }
    reader_error().clear();
    return 0;
}

extern "C" void bodyclock_reader_set_seed(long long seed) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    reader().set_seed(seed);
}

extern "C" void bodyclock_reader_set_split(int min_chars, int max_chars, int tail_chars) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    // Each field independently, so a host can set one and leave the measured
    // defaults in place for the others.
    bodyclock::voice::SplitOptions opt = reader().split();
    if (min_chars > 0) {
        opt.min_chars = static_cast<size_t>(min_chars);
    }
    if (max_chars > 0) {
        opt.max_chars = static_cast<size_t>(max_chars);
    }
    if (tail_chars > 0) {
        opt.tail_chars = static_cast<size_t>(tail_chars);
    }
    reader().set_split(opt);
}

extern "C" void bodyclock_reader_set_clause_cuts(int on) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    bodyclock::voice::SplitOptions opt = reader().split();
    opt.clause_cuts = (on != 0);
    reader().set_split(opt);
}

extern "C" void bodyclock_reader_set_threads(int threads) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    reader().set_threads(threads);
}

extern "C" int bodyclock_reader_begin(bodyclock_reader_audio_handler on_audio,
                                      void* user_data) {
    std::lock_guard<std::mutex> guard(reader_mutex());
    reader_bridge().handler   = on_audio;
    reader_bridge().user_data = user_data;
    if (!reader().begin(on_audio ? reader_sink : 0, on_audio ? &reader_bridge() : 0)) {
        reader_error() = reader().last_error();
        return 0;
    }
    reader_error().clear();
    return 1;
}

extern "C" int bodyclock_reader_feed(const char* delta) {
    if (delta == 0 || *delta == 0) {
        return 1;   // nothing to add is not a failure
    }
    // **No lock.** feed() is called from the thread producing the text, often
    // once per token, while the engines are running behind the Reader's own
    // mutex -- taking this one would serialise the two for no reason. The
    // Reader guards its own state; what this mutex protects is the singleton's
    // configuration, and feed() changes none of it.
    return reader().feed(delta) ? 1 : 0;
}

extern "C" const char* bodyclock_reader_finish(void) {
    bodyclock::voice::ReaderResult result;
    // Also without the lock: finish() BLOCKS for the rest of the speech, and
    // holding the configuration mutex for that long would freeze every status
    // call a host makes meanwhile. The omni lane learned this the hard way --
    // a status read from the renderer stalled the whole main thread.
    const bool ok = reader().finish(&result);
    if (!ok) {
        reader_error() = reader().last_error();
        return 0;
    }
    reader_error().clear();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("sentences");         w.Int(result.sentences);
    w.Key("speechSeconds");     w.Double(result.speech_seconds);
    w.Key("seconds");           w.Double(result.elapsed);
    w.Key("realtime");
    w.Double(result.speech_seconds > 0.0f ? result.elapsed / result.speech_seconds : 0.0);
    w.Key("firstAudioSeconds"); w.Double(result.first_audio_seconds);
    w.Key("starvedSeconds");    w.Double(result.starved_seconds);
    w.Key("heldSeconds");       w.Double(result.held_seconds);
    w.EndObject();

    reader_buffer().assign(buf.GetString(), buf.GetSize());
    return reader_buffer().c_str();
}

extern "C" const char* bodyclock_reader_last_error(void) {
    return reader_error().c_str();
}
