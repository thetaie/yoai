#include "llm/chat/annotations.hpp"

#include "llm/prompt/prompts.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace yoai {
namespace llm {

namespace {

//------------------------------------------------------------------------------
// Time arithmetic: epoch milliseconds and an offset to a local date
//------------------------------------------------------------------------------

const std::int64_t kMsPerMinute = 60 * 1000;
const std::int64_t kMsPerDay    = 24 * 60 * kMsPerMinute;

// How long a written time stays good enough. Past this the next user message
// carries a fresh one; within it, "what time is it?" is answered up to this
// much late, which is the trade for not stamping every message.
const std::int64_t kRestampMs = 30 * kMsPerMinute;

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Days since 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's
// civil_from_days). The core reads no timezone database: the host's offset is
// the whole of the conversion, the same rule store/timeline.hpp keeps.
void civil_from_days(std::int64_t z, int* y, int* m, int* d) {
    z += 719468;
    const std::int64_t era = floor_div(z, 146097);
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp  = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yoe + era * 400 + (*m <= 2 ? 1 : 0));
}

std::int64_t local_day(const Annotations& a) {
    return floor_div(a.time_ms + a.utc_offset * kMsPerMinute, kMsPerDay);
}

//------------------------------------------------------------------------------
// Wording: one annotation to the words the model reads
//------------------------------------------------------------------------------

// "Saturday, October 10, 2026, 2:32 PM UTC+8 Asia/Shanghai": American English,
// with a 12-hour clock. The weekday is spelled out because a model asked about
// "this Friday" otherwise has to work it out from a date, which small models get
// wrong.
std::string format_time(const Annotations& a) {
    static const char* const kWeekdays[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    };
    static const char* const kMonths[12] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December",
    };

    const std::int64_t local = a.time_ms + a.utc_offset * kMsPerMinute;
    const std::int64_t day   = floor_div(local, kMsPerDay);
    const std::int64_t in    = local - day * kMsPerDay;
    int y = 0, mo = 0, d = 0;
    civil_from_days(day, &y, &mo, &d);
    const int weekday = static_cast<int>(((day + 4) % 7 + 7) % 7);   // 1970-01-01 was a Thursday
    const int hour    = static_cast<int>(in / (60 * kMsPerMinute));
    const int minute  = static_cast<int>((in / kMsPerMinute) % 60);

    char offset[16];
    const int minutes = a.utc_offset < 0 ? -a.utc_offset : a.utc_offset;
    if (a.utc_offset == 0) {
        std::snprintf(offset, sizeof(offset), "UTC");
    } else if (minutes % 60 == 0) {
        std::snprintf(offset, sizeof(offset), "UTC%c%d", a.utc_offset < 0 ? '-' : '+', minutes / 60);
    } else {
        std::snprintf(offset, sizeof(offset), "UTC%c%d:%02d", a.utc_offset < 0 ? '-' : '+',
                      minutes / 60, minutes % 60);
    }

    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s, %s %d, %d, %d:%02d %s %s", kWeekdays[weekday], kMonths[mo - 1],
                  d, y, hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM", offset);
    std::string out = buf;
    if (!a.timezone.empty()) out += " " + a.timezone;
    return out;
}

// Arousal on the five-point scale the rest of the design uses, in words: a
// small model reads "a little more animated than usual" more reliably than
// "+0.5". Always relative to the speaker's own baseline, never a verdict on how
// they are.
std::string arousal_words(float arousal) {
    static const char* const kLevels[5] = {
        "much flatter than usual",
        "a little flatter than usual",
        "about the same as usual",
        "a little more animated than usual",
        "much more animated than usual",
    };
    float level = std::floor(arousal * 2.0f + 0.5f);   // nearest half step, times two
    if (level < -2.0f) level = -2.0f;
    if (level > 2.0f) level = 2.0f;
    return kLevels[static_cast<int>(level) + 2];
}

std::string render_part(const char* prompt, const char* arg, const std::string& value) {
    std::map<std::string, std::string> args;
    args[arg] = value;
    std::string out;
    if (!Prompts::instance().render(prompt, args, &out, nullptr)) return std::string();
    return out;
}

}  // namespace

//------------------------------------------------------------------------------
// The transcript: which messages get which parts
//------------------------------------------------------------------------------

std::vector<ChatMessage> with_annotation_text(const std::vector<ChatMessage>& messages) {
    std::vector<ChatMessage> out(messages);

    // The last time actually written, which is what the model is going by.
    bool         stamped     = false;
    std::int64_t last_time   = 0;
    std::int64_t last_day    = 0;
    int          last_offset = 0;
    std::string  last_zone;

    for (std::size_t i = 0; i < out.size(); ++i) {
        ChatMessage& m = out[i];
        if (m.role != "user") continue;
        const Annotations& a = m.annotations;

        std::string line;
        const auto add = [&line](const std::string& part) {
            if (part.empty()) return;
            if (!line.empty()) line += "; ";
            line += part;
        };

        if (a.time_ms > 0) {
            const std::int64_t day = local_day(a);
            // A clock that went backwards is restamped too: the model's idea of
            // now would otherwise be later than the message.
            const bool stale = !stamped || day != last_day || a.utc_offset != last_offset ||
                               a.timezone != last_zone || a.time_ms < last_time ||
                               a.time_ms - last_time >= kRestampMs;
            if (stale) {
                add(render_part(kPromptContextTime, "time", format_time(a)));
                stamped     = true;
                last_time   = a.time_ms;
                last_day    = day;
                last_offset = a.utc_offset;
                last_zone   = a.timezone;
            }
        }

        if (a.has_arousal && std::isfinite(a.arousal)) {
            add(render_part(kPromptContextArousal, "level", arousal_words(a.arousal)));
        }

        if (!line.empty()) m.content = "[" + line + "]\n" + m.content;
    }
    return out;
}

}  // namespace llm
}  // namespace yoai
