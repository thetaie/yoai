#include "llm/sse_parser.hpp"

#include <cstring>
#include <string>

namespace yoai { namespace llm {

namespace {

// Pulls the `data` field out of one SSE event block [block, block+len) (the
// bytes between two blank lines, NOT including the terminating `\n\n`).
//
// Multiple `data:` lines within the same event are concatenated with `\n`,
// per the SSE spec. Other field names (event:, id:, retry:) and comment
// lines (`:` prefix) are ignored — the miro-thinker server only uses `data`.
void extract_data(const char* block, std::size_t len, std::string& out) {
    static const char kData[] = "data:";
    const std::size_t kLen = 5;

    out.clear();
    std::size_t pos = 0;
    while (pos <= len) {
        const char* nl = static_cast<const char*>(std::memchr(block + pos, '\n', len - pos));
        const std::size_t eol = nl ? static_cast<std::size_t>(nl - block) : len;

        std::size_t line_end = eol;                                   // strip trailing '\r'
        if (line_end > pos && block[line_end - 1] == '\r') --line_end;

        if (line_end > pos && block[pos] != ':' &&
            line_end - pos >= kLen && std::memcmp(block + pos, kData, kLen) == 0) {
            std::size_t vbeg = pos + kLen;
            // SSE spec: a single leading space after the colon is stripped.
            if (vbeg < line_end && block[vbeg] == ' ') ++vbeg;
            if (!out.empty()) out.push_back('\n');
            out.append(block + vbeg, line_end - vbeg);
        }

        if (!nl) break;
        pos = eol + 1;
    }
}

}

//------------------------------------------------------------------------------

void SseParser::feed(const char* chunk, std::size_t length, const DataHandler& on_data) {
    buffer_.append(chunk, length);
    drain(on_data);
}

//------------------------------------------------------------------------------

void SseParser::finish(const DataHandler& on_data) {
    if (buffer_.empty()) return;
    // Synthesize an event terminator so drain() emits whatever is left.
    buffer_.append("\n\n");
    drain(on_data);
}

//------------------------------------------------------------------------------

void SseParser::reset() {
    buffer_.clear();
}

//------------------------------------------------------------------------------

void SseParser::drain(const DataHandler& on_data) {
    std::string payload;

    size_t scan = 0;
    while (true) {
        // Find the next event terminator: `\n\n` or `\r\n\r\n`.
        size_t end = std::string::npos;
        size_t skip = 0;
        for (size_t i = scan; i + 1 < buffer_.size(); ++i) {
            if (buffer_[i] == '\n' && buffer_[i + 1] == '\n') {
                end = i; skip = 2; break;
            }
            if (i + 3 < buffer_.size() &&
                buffer_[i] == '\r' && buffer_[i + 1] == '\n' &&
                buffer_[i + 2] == '\r' && buffer_[i + 3] == '\n') {
                end = i; skip = 4; break;
            }
        }
        if (end == std::string::npos) break;

        extract_data(buffer_.data() + scan, end - scan, payload);
        if (!payload.empty()) on_data(payload.data(), payload.size());

        scan = end + skip;
    }

    if (scan > 0) buffer_.erase(0, scan);
}

}
}
