#include "utils/http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <stdexcept>

namespace yoai { namespace utils {

void configure_tls_trust(void* handle) {
    CURL* curl = static_cast<CURL*>(handle);
#if defined(__OHOS__)
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/cacert.pem");
#elif defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#else
    (void)curl;
#endif
}

//------------------------------------------------------------------------------

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

//------------------------------------------------------------------------------

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* resp = static_cast<HttpResponse*>(userdata);
    const size_t total = size * nitems;

    // Trim trailing CRLF / whitespace.
    size_t end = total;
    while (end > 0) {
        const char c = buffer[end - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') --end;
        else break;
    }
    if (end == 0) return total;     // blank line after final header

    // No colon: either the status line ("HTTP/1.1 200 OK") or a malformed
    // continuation. Skip without populating headers.
    size_t colon = 0;
    while (colon < end && buffer[colon] != ':') ++colon;
    if (colon == end) return total;

    // Value: after the colon, leading whitespace trimmed.
    size_t vbeg = colon + 1;
    while (vbeg < end && (buffer[vbeg] == ' ' || buffer[vbeg] == '\t')) ++vbeg;

    std::string lower_key;
    lower_key.reserve(colon);
    for (size_t i = 0; i < colon; ++i) {
        lower_key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(buffer[i]))));
    }

    std::string value_str(buffer + vbeg, end - vbeg);
    if (lower_key == "content-type") {
        resp->content_type = value_str;
    }
    resp->headers[std::move(lower_key)] = std::move(value_str);
    return total;
}

//------------------------------------------------------------------------------

// State threaded through the streaming callbacks below.
struct StreamState {
    CURL* curl = nullptr;
    const HttpStreamSink* sink = nullptr;
    std::string content_type;   // captured from headers, reported with on_headers
    bool headers_fired = false;
    bool aborted = false;       // on_chunk asked to stop
};

size_t stream_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* st = static_cast<StreamState*>(userdata);
    const size_t total = size * nitems;

    size_t colon = 0;
    while (colon < total && buffer[colon] != ':') ++colon;
    if (colon == total) return total;

    std::string lower_key;
    lower_key.reserve(colon);
    for (size_t i = 0; i < colon; ++i) {
        lower_key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(buffer[i]))));
    }
    if (lower_key == "content-type") {
        // Value: after the colon, leading and trailing whitespace trimmed.
        size_t vbeg = colon + 1;
        size_t vend = total;
        while (vbeg < vend && (buffer[vbeg] == ' ' || buffer[vbeg] == '\t')) ++vbeg;
        while (vend > vbeg) {
            const char c = buffer[vend - 1];
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t') --vend;
            else break;
        }
        st->content_type.assign(buffer + vbeg, vend - vbeg);
    }
    return total;
}

// Fire on_headers lazily on the first body byte, when the status code is final
// (it may still be 1xx/redirect during header_cb), then hand the chunk over.
size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* st = static_cast<StreamState*>(userdata);
    size_t n = size * nmemb;
    if (!st->headers_fired) {
        long status = 0;
        curl_easy_getinfo(st->curl, CURLINFO_RESPONSE_CODE, &status);
        if (st->sink->on_headers) st->sink->on_headers(status, st->content_type);
        st->headers_fired = true;
    }
    if (st->sink->on_chunk && !st->sink->on_chunk(ptr, n)) {
        st->aborted = true;
        return 0;   // != n tells curl to abort with CURLE_WRITE_ERROR
    }
    return n;
}

// Polled by curl (~several times/sec) regardless of data flow; a non-zero return
// aborts the transfer. Lets sink.keep_going() tear down an idle-but-open stream.
int stream_xferinfo_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* st = static_cast<StreamState*>(clientp);
    if (st->sink->keep_going && !st->sink->keep_going()) {
        st->aborted = true;
        return 1;
    }
    return 0;
}

}

//------------------------------------------------------------------------------

HttpClient::HttpClient() = default;
HttpClient::~HttpClient() = default;

//------------------------------------------------------------------------------

void HttpClient::global_init() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        throw std::runtime_error("curl_global_init failed");
    }
}

//------------------------------------------------------------------------------

void HttpClient::global_cleanup() {
    curl_global_cleanup();
}

//------------------------------------------------------------------------------

HttpResponse HttpClient::post(const HttpRequest& req) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    HttpResponse resp;
    struct curl_slist* hdrs = nullptr;

    std::string content_type_header = "Content-Type: " + req.content_type;
    hdrs = curl_slist_append(hdrs, content_type_header.c_str());
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());

    configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    } else {
        resp.status = -1;
        resp.body = curl_easy_strerror(rc);
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return resp;
}

//------------------------------------------------------------------------------

long HttpClient::post_stream(const HttpRequest& req, const HttpStreamSink& sink, std::string* err) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (err) *err = "curl_easy_init failed";
        return -1;
    }

    struct curl_slist* hdrs = nullptr;
    std::string content_type_header = "Content-Type: " + req.content_type;
    hdrs = curl_slist_append(hdrs, content_type_header.c_str());
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());

    StreamState st;
    st.curl = curl;
    st.sink = &sink;

    configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, stream_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &st);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (sink.keep_going) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_xferinfo_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &st);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK) {
        // A response with no body never reaches stream_write_cb; surface headers
        // here so the caller still learns the status and Content-Type.
        if (!st.headers_fired && sink.on_headers) {
            long s = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &s);
            sink.on_headers(s, st.content_type);
        }
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    } else if (err) {
        *err = st.aborted ? "transfer aborted by caller" : curl_easy_strerror(rc);
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return status;
}

//------------------------------------------------------------------------------

HttpResponse HttpClient::request(const std::string& method, const HttpRequest& req) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    HttpResponse resp;
    struct curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    // Drop curl's default "Expect: 100-continue" on bodied requests: S3/OSS do
    // not run the 100-continue handshake (it stalls a round-trip), and it is
    // not among the signed headers, so removing it is safe.
    hdrs = curl_slist_append(hdrs, "Expect:");

    const std::string method_str{method};

    configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method_str.c_str());
    // Attach the body for methods that carry one. POSTFIELDS does not copy, so
    // req.body must outlive the perform() call below — it does (req is a ref).
    if (!req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    } else {
        resp.status = -1;
        resp.body = curl_easy_strerror(rc);
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return resp;
}

//------------------------------------------------------------------------------

HttpResponse HttpClient::get(const std::string& url, int timeout_ms, const std::vector<std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    HttpResponse resp;
    std::string url_str{url};
    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());

    configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    } else {
        resp.status = -1;
        resp.body = curl_easy_strerror(rc);
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return resp;
}

}
}
