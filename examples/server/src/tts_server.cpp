// libwebsockets.h first: on Windows it brings in winsock2.h, which has to come
// before anything that includes windows.h -- and windows.h itself, whose min
// and max macros break std::min and std::max unless told not to define them.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <libwebsockets.h>

#include "tts_server.hpp"

#include "utils/log.hpp"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace yoai {
namespace server {

namespace {

// The subprotocol, for a client that names one. One that names none gets it
// anyway: libwebsockets hands such a connection to the first protocol.
const char kProtocol[] = "yoai-tts";

// libwebsockets' own log lines, errors and warnings only, into ours. Its
// notices are one per connection and say nothing the server does not.
void lws_log(int level, const char* line) {
    std::string s(line ? line : "");
    while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == '\r')) {
        s.erase(s.size() - 1);
    }
    if (level & LLL_ERR) {
        utils::log_error("lws: %s", s.c_str());
    } else {
        utils::log_warn("lws: %s", s.c_str());
    }
}

typedef rapidjson::Writer<rapidjson::StringBuffer> JsonWriter;

std::string json_error(const char* code, const std::string& message) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("type");
    w.String("error");
    w.Key("code");
    w.String(code);
    w.Key("message");
    w.String(message.c_str(), static_cast<rapidjson::SizeType>(message.size()));
    w.EndObject();
    return buf.GetString();
}

std::string json_started(bool f32) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("type");
    w.String("started");
    w.Key("sample_rate");
    w.Int(voice::Reader::sample_rate());
    w.Key("channels");
    w.Int(1);
    w.Key("format");
    w.String(f32 ? "f32" : "s16");
    w.EndObject();
    return buf.GetString();
}

std::string json_done(const voice::ReaderResult& r) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("type");
    w.String("done");
    w.Key("sentences");
    w.Int(r.sentences);
    w.Key("speech_seconds");
    w.Double(r.speech_seconds);
    w.Key("elapsed_seconds");
    w.Double(r.elapsed);
    w.Key("first_audio_seconds");
    w.Double(r.first_audio_seconds);
    w.Key("starved_seconds");
    w.Double(r.starved_seconds);
    w.EndObject();
    return buf.GetString();
}

std::string json_type(const char* type) {
    return std::string("{\"type\":\"") + type + "\"}";
}

}  // namespace

//------------------------------------------------------------------------------
// Session
//------------------------------------------------------------------------------

// One connection. Shared between the loop, the Reader's emitter and the
// finishing thread: the finishing thread holds a reference for as long as the
// stream runs, so a client that disconnects mid-sentence does not take the
// sink's user_data with it.
struct TtsServer::Session {
    enum State { Idle, Streaming, Finishing };

    // One message waiting to go out, with the LWS_PRE bytes libwebsockets
    // needs in front of the payload already reserved.
    struct Frame {
        std::vector<unsigned char> buf;
        bool                       binary;
    };

    TtsServer*        server;
    lws*              wsi;        // the loop's alone; null once closed
    std::string       peer;
    std::string       rx;         // the loop's alone: the message being assembled
    bool              rx_binary;
    bool              rx_over;
    std::atomic<int>  state;
    bool              f32;        // set before Reader::begin, read by the emitter
    std::atomic<bool> cancelled;

    std::mutex         mu;
    std::deque<Frame>  out;       // guarded by mu

    Session(TtsServer* srv, lws* w)
        : server(srv), wsi(w), rx_binary(false), rx_over(false), state(Idle), f32(false),
          cancelled(false) {}

    void push(const void* data, std::size_t n, bool binary) {
        std::vector<unsigned char> buf(LWS_PRE + n);
        if (n > 0) {
            std::memcpy(&buf[LWS_PRE], data, n);
        }
        std::lock_guard<std::mutex> lock(mu);
        out.push_back(Frame());
        out.back().buf.swap(buf);
        out.back().binary = binary;
    }

    void push_text(const std::string& s) { push(s.data(), s.size(), false); }

    bool pending() {
        std::lock_guard<std::mutex> lock(mu);
        return !out.empty();
    }
};

//------------------------------------------------------------------------------
// Callbacks
//------------------------------------------------------------------------------

struct TtsServer::Glue {
    // On the Reader's emitter thread. Converts and queues; the loop sends.
    static bool audio(const float* pcm, std::size_t n, int /*sentence*/, bool /*last*/,
                      void* user_data) {
        Session* s = static_cast<Session*>(user_data);
        if (s->cancelled.load()) {
            return false;   // the Reader abandons generation at its next block
        }
        if (n == 0) {
            return true;    // the end marker; the loop reports the end itself
        }
        // Little-endian on the wire, which is what every target here is, so
        // both formats are the host's own bytes.
        if (s->f32) {
            s->push(pcm, n * sizeof(float), true);
        } else {
            std::vector<short> pcm16(n);
            for (std::size_t i = 0; i < n; ++i) {
                const float v = std::max(-1.0f, std::min(1.0f, pcm[i]));
                pcm16[i] = static_cast<short>(v * 32767.0f);
            }
            s->push(&pcm16[0], n * sizeof(short), true);
        }
        s->server->wake();
        return true;
    }

    static int event(lws* wsi, enum lws_callback_reasons reason, void* user, void* in,
                     std::size_t len) {
        lws_context* ctx = lws_get_context(wsi);
        TtsServer* self = ctx ? static_cast<TtsServer*>(lws_context_user(ctx)) : 0;
        if (!self) {
            return lws_callback_http_dummy(wsi, reason, user, in, len);
        }

        switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            std::shared_ptr<Session> s(new Session(self, wsi));
            char peer[128];
            s->peer = lws_get_peer_simple(wsi, peer, sizeof(peer)) ? peer : "?";
            self->sessions_[wsi] = s;
            utils::log_info("server: %s connected, %d open", s->peer.c_str(),
                            static_cast<int>(self->sessions_.size()));
            return 0;
        }

        case LWS_CALLBACK_RECEIVE: {
            std::map<lws*, std::shared_ptr<Session> >::iterator it = self->sessions_.find(wsi);
            if (it == self->sessions_.end()) {
                return -1;
            }
            const std::shared_ptr<Session>& s = it->second;
            // A message can come in several pieces: frames of a fragmented
            // message, and a frame larger than the receive buffer. It is whole
            // when both have run out.
            if (lws_frame_is_binary(wsi)) {
                s->rx_binary = true;
            }
            if (!s->rx_over && s->rx.size() + len <= self->opt_.max_message_bytes) {
                s->rx.append(static_cast<const char*>(in), len);
            } else {
                s->rx_over = true;
                s->rx.clear();
            }
            if (!lws_is_final_fragment(wsi) || lws_remaining_packet_payload(wsi) > 0) {
                return 0;
            }
            if (s->rx_binary) {
                s->push_text(json_error("bad_message", "binary messages are not accepted; "
                                                       "send text as JSON"));
            } else if (s->rx_over) {
                s->push_text(json_error("too_large", "a message may be at most " +
                                                         std::to_string(self->opt_.max_message_bytes) +
                                                         " bytes; send the text in pieces"));
            } else {
                self->on_message(s);
            }
            s->rx.clear();
            s->rx_binary = false;
            s->rx_over   = false;
            if (s->pending()) {
                lws_callback_on_writable(wsi);
            }
            return 0;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            std::map<lws*, std::shared_ptr<Session> >::iterator it = self->sessions_.find(wsi);
            if (it == self->sessions_.end()) {
                return 0;
            }
            Session& s = *it->second;
            Session::Frame f;
            bool more = false;
            {
                std::lock_guard<std::mutex> lock(s.mu);
                if (s.out.empty()) {
                    return 0;
                }
                f.buf.swap(s.out.front().buf);
                f.binary = s.out.front().binary;
                s.out.pop_front();
                more = !s.out.empty();
            }
            // One message per callback. libwebsockets keeps whatever the
            // socket did not take and holds the next callback until it has
            // gone, so a slow client backs up here rather than in the kernel.
            const std::size_t n = f.buf.size() - LWS_PRE;
            if (lws_write(wsi, &f.buf[LWS_PRE], n, f.binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT) <
                static_cast<int>(n)) {
                return -1;   // closes the connection
            }
            if (more) {
                lws_callback_on_writable(wsi);
            }
            return 0;
        }

        case LWS_CALLBACK_CLOSED: {
            std::map<lws*, std::shared_ptr<Session> >::iterator it = self->sessions_.find(wsi);
            if (it == self->sessions_.end()) {
                return 0;
            }
            std::shared_ptr<Session> s = it->second;
            self->sessions_.erase(it);
            s->wsi = 0;
            // Nobody is listening any more, so whatever is being read stops;
            // it still has to be finished to free the Reader.
            s->cancelled.store(true);
            if (s->state.load() == Session::Streaming) {
                self->finish_stream(s);
            }
            utils::log_info("server: %s disconnected, %d open", s->peer.c_str(),
                            static_cast<int>(self->sessions_.size()));
            return 0;
        }

        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            // wake(), from the emitter or a finishing thread: something was
            // queued somewhere. Cheap enough to look at every connection.
            for (std::map<lws*, std::shared_ptr<Session> >::iterator it = self->sessions_.begin();
                 it != self->sessions_.end(); ++it) {
                if (it->second->pending()) {
                    lws_callback_on_writable(it->first);
                }
            }
            return 0;

        default:
            return lws_callback_http_dummy(wsi, reason, user, in, len);
        }
    }
};

//------------------------------------------------------------------------------
// TtsServer
//------------------------------------------------------------------------------

TtsServer::TtsServer(voice::Reader& reader)
    : reader_(reader), context_(0), stopping_(false), busy_(false) {}

TtsServer::~TtsServer() {
    if (finisher_.joinable()) {
        finisher_.join();
    }
}

const std::string& TtsServer::last_error() const {
    return error_;
}

void TtsServer::stop() {
    stopping_.store(true);
    wake();
}

// The one libwebsockets call that is allowed from another thread: it makes the
// loop's wait return with LWS_CALLBACK_EVENT_WAIT_CANCELLED.
void TtsServer::wake() {
    lws_context* ctx = context_.load();
    if (ctx) {
        lws_cancel_service(ctx);
    }
}

bool TtsServer::run(const TtsServerOptions& opt) {
    error_.clear();
    opt_ = opt;

    lws_set_log_level(LLL_ERR | LLL_WARN, &lws_log);

    lws_protocols protocols[2];
    std::memset(protocols, 0, sizeof(protocols));
    protocols[0].name     = kProtocol;
    protocols[0].callback = &Glue::event;

    lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));
    info.port      = opt.port;
    info.iface     = opt.host.empty() ? 0 : opt.host.c_str();
    info.protocols = protocols;
    info.user      = this;
    // Text frames that are not UTF-8 close the connection before they reach
    // the Reader, which would otherwise count and cut them as characters.
    info.options   = LWS_SERVER_OPTION_VALIDATE_UTF8;

    lws_context* ctx = lws_create_context(&info);
    if (!ctx) {
        error_ = "could not listen on " + (opt.host.empty() ? std::string("*") : opt.host) + ":" +
                 std::to_string(opt.port);
        return false;
    }
    context_.store(ctx);
    utils::log_info("server: listening on ws://%s:%d/, %s, %d voice(s)",
                    opt.host.empty() ? "*" : opt.host.c_str(), opt.port,
                    reader_.model_type().c_str(), reader_.voices());

    while (!stopping_.load()) {
        if (lws_service(ctx, 0) < 0) {
            break;
        }
    }

    // Whatever is still being read is abandoned, and finished so the Reader
    // is idle again. Before the context goes: the finishing thread wakes it.
    for (std::map<lws*, std::shared_ptr<Session> >::iterator it = sessions_.begin();
         it != sessions_.end(); ++it) {
        it->second->cancelled.store(true);
        if (it->second->state.load() == Session::Streaming) {
            finish_stream(it->second);
        }
    }
    if (finisher_.joinable()) {
        finisher_.join();
    }

    context_.store(0);
    lws_context_destroy(ctx);   // closes every connection, through CLOSED
    sessions_.clear();
    utils::log_info("server: stopped");
    return true;
}

// A whole text message from `s`, on the loop.
void TtsServer::on_message(const std::shared_ptr<Session>& s) {
    rapidjson::Document d;
    d.Parse(s->rx.c_str(), s->rx.size());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("type") || !d["type"].IsString()) {
        s->push_text(json_error("bad_message", "expected a JSON object with a \"type\""));
        return;
    }
    const std::string type = d["type"].GetString();
    const int state = s->state.load();

    if (type == "start") {
        if (state != Session::Idle) {
            s->push_text(json_error("already_started", "this connection is already reading; "
                                                       "finish or cancel first"));
            return;
        }
        bool f32 = false;
        if (d.HasMember("format")) {
            const rapidjson::Value& f = d["format"];
            if (f.IsString() && std::strcmp(f.GetString(), "f32") == 0) {
                f32 = true;
            } else if (!f.IsString() || std::strcmp(f.GetString(), "s16") != 0) {
                s->push_text(json_error("bad_format", "format is \"s16\" or \"f32\""));
                return;
            }
        }
        start_stream(s, f32);
        return;
    }

    if (type == "text") {
        if (!d.HasMember("text") || !d["text"].IsString()) {
            s->push_text(json_error("bad_message", "a text message needs a string \"text\""));
            return;
        }
        if (state == Session::Idle) {
            s->push_text(json_error("not_started", "send start first"));
            return;
        }
        if (state == Session::Finishing) {
            return;   // after finish, or after a failure the finish will report
        }
        const rapidjson::Value& t = d["text"];
        if (!reader_.feed(std::string(t.GetString(), t.GetStringLength()))) {
            // The Reader has stopped -- an engine refused a piece. finish()
            // is what says why.
            finish_stream(s);
        }
        return;
    }

    if (type == "finish") {
        if (state == Session::Idle) {
            s->push_text(json_error("not_started", "send start first"));
            return;
        }
        if (state == Session::Streaming) {
            finish_stream(s);
        }
        return;
    }

    if (type == "cancel") {
        if (state == Session::Idle) {
            return;   // nothing to cancel, which is not a mistake
        }
        s->cancelled.store(true);
        if (state == Session::Streaming) {
            finish_stream(s);
        }
        return;
    }

    s->push_text(json_error("bad_message", "unknown type \"" + type + "\""));
}

void TtsServer::start_stream(const std::shared_ptr<Session>& s, bool f32) {
    if (busy_.load()) {
        s->push_text(json_error("busy", "another client is being read to; try again when it ends"));
        return;
    }
    // The last stream's finishing thread has already let go of the Reader,
    // so this does not wait on synthesis.
    if (finisher_.joinable()) {
        finisher_.join();
    }
    s->f32 = f32;
    s->cancelled.store(false);
    busy_.store(true);
    if (!reader_.begin(&Glue::audio, s.get())) {
        busy_.store(false);
        s->push_text(json_error("synthesis", reader_.last_error()));
        return;
    }
    s->state.store(Session::Streaming);
    // Ahead of any audio: none can come before the first text, and that is
    // read after this returns.
    s->push_text(json_started(f32));
    utils::log_info("server: %s started", s->peer.c_str());
}

// No more text for this stream. finish() blocks until the last sample is
// out, so it goes to a thread of its own.
void TtsServer::finish_stream(const std::shared_ptr<Session>& s) {
    s->state.store(Session::Finishing);
    if (finisher_.joinable()) {
        finisher_.join();
    }
    finisher_ = std::thread(&TtsServer::run_finish, this, s);
}

void TtsServer::run_finish(std::shared_ptr<Session> s) {
    voice::ReaderResult r;
    const bool ok = reader_.finish(&r);
    std::string msg;
    if (!ok) {
        msg = json_error("synthesis", reader_.last_error());
    } else if (s->cancelled.load()) {
        msg = json_type("cancelled");
    } else {
        msg = json_done(r);
    }
    // The Reader first, then the connection, so a client that starts again
    // the moment it hears the end never finds its own stream still busy.
    busy_.store(false);
    s->state.store(Session::Idle);
    s->push_text(msg);
    utils::log_info("server: %s %s", s->peer.c_str(),
                    !ok ? "failed" : s->cancelled.load() ? "cancelled" : "done");
    wake();
}

}  // namespace server
}  // namespace yoai
