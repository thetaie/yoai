// Text in, speech out, over a WebSocket: voice::Reader's begin / feed / finish
// behind a port.
//
// The protocol is in README.md beside this file, for whoever writes a client.
// This header is the C++ side: who owns what, and on which thread.
//
// **One stream at a time, server-wide.** The server drives ONE Reader, and a
// Reader reads one passage at a time; each engine behind it is 1.4 GB, and a
// second one measured slower on the desktop, not faster (voice/tts/reader.hpp).
// So a second client that asks to start while another is speaking is told it
// is busy, not queued. Any number of clients may be connected.
//
// **The Reader is the caller's.** Loading it, the voice, the seed and the
// cutting are all set before run(), by whatever main() this is linked into --
// cli/server.cpp today, a container's entry point later. The server only reads
// what it is given; nothing a client sends changes the configuration.
//
// Threads:
//
//   - run() is the libwebsockets event loop. Every socket call happens here,
//     and so do Reader::begin and Reader::feed, which return at once.
//   - Reader::finish blocks until the last sample is out, so it runs on a
//     thread of its own, one per stream.
//   - Audio arrives on the Reader's emitter thread. It is queued per client and
//     the loop is woken to send it; libwebsockets allows nothing else to be
//     called from another thread.

#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "voice/tts/reader.hpp"

struct lws;
struct lws_context;

namespace yoai {
namespace server {

struct TtsServerOptions {
    // The interface to bind, as an address or an interface name. Empty binds
    // every interface.
    std::string host;
    int         port = 8765;

    // The longest text message a client may send, in bytes. A longer one is
    // refused with an error and dropped; the connection stays open. The text
    // arrives as deltas, so no single message needs to be large.
    std::size_t max_message_bytes = 64 * 1024;
};

class TtsServer {
public:
    // `reader` must be loaded, and must outlive the server.
    explicit TtsServer(voice::Reader& reader);
    ~TtsServer();

    // Serve until stop(). Returns false, with last_error() set, when the port
    // could not be opened; true after a stop.
    //
    // On the way out a stream still running is cancelled and finished, so the
    // Reader is idle again when this returns.
    bool run(const TtsServerOptions& opt);

    // Ask run() to return. Safe from any thread and from a signal handler, and
    // before run() too: a stop that arrives first makes run() return at once.
    // Once stopped, a server does not run again.
    void stop();

    const std::string& last_error() const;

private:
    TtsServer(const TtsServer&);
    TtsServer& operator=(const TtsServer&);

    struct Session;

    // The two callbacks, libwebsockets' and the Reader's. In the .cpp, where
    // libwebsockets.h is: its callback takes an enum this header cannot name.
    struct Glue;
    friend struct Glue;

    void on_message(const std::shared_ptr<Session>& s);
    void start_stream(const std::shared_ptr<Session>& s, bool f32);
    void finish_stream(const std::shared_ptr<Session>& s);
    void run_finish(std::shared_ptr<Session> s);
    void wake();

    voice::Reader&            reader_;
    TtsServerOptions          opt_;
    std::string               error_;
    std::atomic<lws_context*> context_;
    std::atomic<bool>         stopping_;

    // Whether a stream holds the Reader, from start until its finish() has
    // returned. Set on the loop, cleared on the finishing thread.
    std::atomic<bool>         busy_;
    std::thread               finisher_;

    // Every open connection. The loop's alone.
    std::map<lws*, std::shared_ptr<Session> > sessions_;
};

}  // namespace server
}  // namespace yoai
