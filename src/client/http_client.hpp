#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yoai { namespace client {

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string content_type;

    // All response headers, with the key lowercased so callers can do
    // case-insensitive lookups (HTTP header names are case-insensitive).
    // Repeated headers (e.g. Set-Cookie) only keep the last value: this
    // wrapper is intentionally simple.
    std::unordered_map<std::string, std::string> headers;
};

//------------------------------------------------------------------------------

struct HttpRequest {
    std::string url;
    std::string body;
    std::string content_type = "application/json";
    std::vector<std::string> headers;
    int connect_timeout_ms = 10000;
    int request_timeout_ms = 60000;
};

//------------------------------------------------------------------------------

// Callbacks for a streaming transfer. on_headers fires once, when the upstream
// response status and Content-Type are known, before any body. on_chunk fires
// for each body fragment as it arrives -- `chunk`/`length` is a borrowed,
// not-null-terminated byte range valid only for the call; return false to abort
// the transfer (e.g. the downstream client disconnected). Both run on the thread
// that calls post_stream(). Either may be left empty.
struct HttpStreamSink {
    std::function<void(long status, const std::string& content_type)> on_headers;
    std::function<bool(const void* chunk, std::size_t length)> on_chunk;
    // Optional liveness check, polled periodically even while the upstream is
    // idle (no body arriving). Return false to abort the transfer -- lets a
    // caller tear down a stalled connection without waiting for the timeout.
    std::function<bool()> keep_going;
};

//------------------------------------------------------------------------------
// HttpClient
//------------------------------------------------------------------------------

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse post(const HttpRequest& req);
    HttpResponse get(const std::string& url,
                     int timeout_ms = 10000,
                     const std::vector<std::string>& headers = {});

    // Send `req` with an arbitrary HTTP method ("PUT", "DELETE", "GET", ...).
    // Headers are sent verbatim from req.headers — unlike post(), no
    // Content-Type is added automatically, so the caller controls every header
    // (object-storage request signing depends on the exact header set). The
    // body is sent when non-empty. curl's default "Expect: 100-continue" is
    // suppressed. Same return convention as post(): HttpResponse.status is the
    // HTTP code, or -1 with the transport error in .body.
    HttpResponse request(const std::string& method, const HttpRequest& req);

    // Streaming POST: invokes sink.on_headers once, then sink.on_chunk per body
    // fragment as the upstream sends it, instead of buffering the whole body.
    // Returns the HTTP status (> 0) on success, or -1 on transport error or
    // caller abort, writing the error text to *err when non-null. Blocking --
    // run it on a worker thread.
    long post_stream(const HttpRequest& req, const HttpStreamSink& sink, std::string* err);

    static void global_init();
    static void global_cleanup();
};

}
}
