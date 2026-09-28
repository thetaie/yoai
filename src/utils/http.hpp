#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yoai { namespace utils {

// Tell libcurl to consult the platform's native trust store (Windows Schannel,
// macOS keychain) at handshake time, regardless of which SSL backend it was
// built against. Call it on every easy handle before curl_easy_perform, so the
// trust-store configuration lives in exactly one place. `curl` is a CURL*;
// taking void* keeps <curl/curl.h> out of this header.
//
// On Windows + vcpkg, libcurl is wired against OpenSSL, which only reads its
// compiled-in cert path — frequently a non-existent location on the user's
// machine. The result is every HTTPS request failing with
//
//     SSL peer certificate or SSH remote key was not OK
//
// even for certs signed by public CAs. CURLSSLOPT_NATIVE_CA flips libcurl to
// hand the OpenSSL backend the system trust roots, which is what every other
// HTTPS client on the box already does.
//
// No-op on libcurl versions that don't define the flag (added in 7.71 for
// Schannel, 7.81 broadly for OpenSSL). Cheap (just sets a flag).
//
// On OpenHarmony / HarmonyOS NEXT there is no NATIVE_CA path either -- curl is
// built against our cross-compiled OpenSSL, whose compiled-in default
// (OPENSSLDIR /etc/ssl -> cert.pem + a c_rehash'd certs/ dir) matches nothing
// on the device. What the OS actually ships is a Mozilla-style PEM bundle at
// /etc/ssl/certs/cacert.pem, so point CAINFO straight at it. Without this every
// HTTPS request fails the handshake with the same "SSL peer certificate or SSH
// remote key was not OK".
void configure_tls_trust(void* curl);

//------------------------------------------------------------------------------

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
