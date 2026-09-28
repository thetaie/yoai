#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace yoai { namespace llm {

// Splits a raw HTTP byte stream into SSE event payloads.
//
// Each call to feed() appends to an internal buffer and emits all complete
// events found. An "event" terminates on a blank line (`\n\n`). Within an
// event, every `data:` field is concatenated with `\n`, matching the SSE
// spec; lines starting with `:` are treated as comments and dropped.
//
// The terminal `[DONE]` payload is passed through verbatim — the caller
// decides whether it ends the stream.
class SseParser {
public:
    // The parsed SSE `data` payload as a borrowed (pointer, length) range, valid
    // only for the call. Not null-terminated.
    using DataHandler = std::function<void(const void* data, std::size_t length)>;

    void feed(const char* chunk, std::size_t length, const DataHandler& on_data);

    // Flush a trailing event whose terminating blank line never arrived.
    // Networks usually deliver one, but servers that close mid-event leave
    // the last payload stuck in the buffer otherwise.
    void finish(const DataHandler& on_data);

    void reset();

private:
    void drain(const DataHandler& on_data);

    std::string buffer_;
};

}
}
