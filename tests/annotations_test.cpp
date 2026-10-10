// A message's annotations, as the line the model reads in front of it.
//
//     build/yoai-annotations-test
//
// What it is here for:
//
//   1. When the time is written. Every message carries one as data; the model
//      gets it only on the first, after 30 minutes, across a date line, or when
//      the zone changes. A rule that stamps too often wastes the context, and one
//      that stamps too rarely leaves the model a day behind.
//   2. Prefix stability. Rendering a message must depend only on the messages
//      before it, or adding a turn rewrites an earlier one and every cached
//      prefix -- local KV cache, a provider's prompt cache -- is thrown away.
//   3. The local time itself: offsets east and west, half hours, a date line.
//   4. The JSON a host sends, through yoai_chat_preview: annotations read, a
//      field of the wrong type ignored, `content` untouched.

#include "llm/chat/annotations.hpp"
#include "yoai.h"

#include <rapidjson/document.h>

#include <cstdio>
#include <string>
#include <vector>

using yoai::llm::Annotations;
using yoai::llm::ChatMessage;
using yoai::llm::with_annotation_text;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
    check(got == want, what);
    if (got != want) {
        std::printf("        got:  %s\n        want: %s\n", got.c_str(), want.c_str());
    }
}

ChatMessage user(const std::string& text, long long time_ms, int offset, const char* zone) {
    ChatMessage m;
    m.role                   = "user";
    m.content                = text;
    m.annotations.time_ms    = time_ms;
    m.annotations.utc_offset = offset;
    m.annotations.timezone   = zone;
    return m;
}

ChatMessage reply(const std::string& text) {
    ChatMessage m;
    m.role    = "assistant";
    m.content = text;
    // Annotations on a reply are the host's business and never reach the model.
    m.annotations.time_ms = 1791613920000LL;
    return m;
}

// 2026-10-10, in UTC.
const long long k0632 = 1791613920000LL;
const long long k0642 = 1791614520000LL;   // +10 min
const long long k0712 = 1791616320000LL;   // +40 min
const long long k1550 = 1791647400000LL;   // 23:50 in UTC+8
const long long k1605 = 1791648300000LL;   // 00:05 the next day in UTC+8
const long long k1610 = 1791648600000LL;

void time_checks() {
    std::printf("-- when the time is written\n");
    std::vector<ChatMessage> t;
    t.push_back(user("你好", k0632, 480, "Asia/Shanghai"));
    t.push_back(reply("你好！"));
    t.push_back(user("在吗", k0642, 480, "Asia/Shanghai"));
    t.push_back(reply("在。"));
    t.push_back(user("还在吗", k0712, 480, "Asia/Shanghai"));
    t.push_back(user("睡了吗", k1550, 480, "Asia/Shanghai"));
    t.push_back(user("过零点了", k1605, 480, "Asia/Shanghai"));
    t.push_back(user("到了", k1610, 330, ""));
    t.push_back(user("没有时间", 0, 0, ""));

    const std::vector<ChatMessage> r = with_annotation_text(t);
    check(r.size() == t.size(), "every message comes back");
    check_eq(r[0].content, "[Time: Saturday, October 10, 2026, 2:32 PM UTC+8 Asia/Shanghai]\n你好",
             "the first message gets the weekday, date, time, offset and zone");
    check_eq(r[1].content, "你好！", "a reply is untouched, its annotations ignored");
    check_eq(r[2].content, "在吗", "ten minutes later: no time");
    check_eq(r[4].content, "[Time: Saturday, October 10, 2026, 3:12 PM UTC+8 Asia/Shanghai]\n还在吗",
             "forty minutes after the last one written: the time again");
    check_eq(r[5].content, "[Time: Saturday, October 10, 2026, 11:50 PM UTC+8 Asia/Shanghai]\n睡了吗",
             "hours later: the time again");
    check_eq(r[6].content, "[Time: Sunday, October 11, 2026, 12:05 AM UTC+8 Asia/Shanghai]\n过零点了",
             "fifteen minutes later but past midnight: the time again, a new day, 12 AM");
    check_eq(r[7].content, "[Time: Saturday, October 10, 2026, 9:40 PM UTC+5:30]\n到了",
             "five minutes later in another zone: the time again, half-hour offset, no name");
    check_eq(r[8].content, "没有时间", "no time given: nothing written");
    check(r[0].annotations.time_ms == k0632, "the annotations themselves are kept");

    // The property every cached prefix depends on.
    bool stable = true;
    for (std::size_t k = 1; k <= t.size(); ++k) {
        const std::vector<ChatMessage> head(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(k));
        const std::vector<ChatMessage> rh = with_annotation_text(head);
        for (std::size_t i = 0; i < k; ++i) {
            if (rh[i].content != r[i].content) stable = false;
        }
    }
    check(stable, "a message renders the same however many follow it");
    check(with_annotation_text(t)[6].content == r[6].content, "and the same on every call");

    std::vector<ChatMessage> west;
    west.push_back(user("早", 1772366400000LL, -210, "America/St_Johns"));
    check_eq(with_annotation_text(west)[0].content,
             "[Time: Sunday, March 1, 2026, 8:30 AM UTC-3:30 America/St_Johns]\n早",
             "west of UTC, half an hour");
    std::vector<ChatMessage> utc;
    utc.push_back(user("hi", 1772366400000LL, 0, ""));
    check_eq(with_annotation_text(utc)[0].content, "[Time: Sunday, March 1, 2026, 12:00 PM UTC]\nhi",
             "UTC itself, noon as 12 PM");
}

void arousal_checks() {
    std::printf("-- arousal\n");
    struct Case {
        float       value;
        const char* words;
    };
    const Case cases[] = {
        {-1.0f, "much flatter than usual"},
        {-0.3f, "a little flatter than usual"},
        {-0.2f, "about the same as usual"},
        {0.2f, "about the same as usual"},
        {0.3f, "a little more animated than usual"},
        {0.5f, "a little more animated than usual"},
        {1.0f, "much more animated than usual"},
        {3.0f, "much more animated than usual"},
    };
    bool all = true;
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        std::vector<ChatMessage> t(1, user("嗯", 0, 0, ""));
        t[0].annotations.has_arousal = true;
        t[0].annotations.arousal     = cases[i].value;
        const std::string want = std::string("[Tone: ") + cases[i].words + " (measured from the voice)]\n嗯";
        if (with_annotation_text(t)[0].content != want) {
            std::printf("        %.1f gave %s\n", cases[i].value, with_annotation_text(t)[0].content.c_str());
            all = false;
        }
    }
    check(all, "five levels, nearest half step, clamped");

    std::vector<ChatMessage> both(1, user("今天又加班", k0632, 480, ""));
    both[0].annotations.has_arousal = true;
    both[0].annotations.arousal     = 0.5f;
    check_eq(with_annotation_text(both)[0].content,
             "[Time: Saturday, October 10, 2026, 2:32 PM UTC+8; "
             "Tone: a little more animated than usual (measured from the voice)]\n今天又加班",
             "time and arousal share one line");

    std::vector<ChatMessage> none(1, user("嗯", 0, 0, ""));
    none[0].annotations.arousal = 1.0f;   // has_arousal left false
    check_eq(with_annotation_text(none)[0].content, "嗯", "arousal without has_arousal is not written");
}

void abi_checks() {
    std::printf("-- yoai_chat_preview\n");
    const char* json =
        "[{\"role\":\"user\",\"content\":\"你好\","
        "\"annotations\":{\"time\":1791613920000,\"utcOffset\":480,\"timezone\":\"Asia/Shanghai\"}},"
        "{\"role\":\"assistant\",\"content\":\"你好！\"},"
        "{\"role\":\"user\",\"content\":\"累了\","
        "\"annotations\":{\"time\":\"soon\",\"arousal\":-1}}]";
    rapidjson::Document doc;
    const char* out = yoai_chat_preview(json);
    check(out != nullptr, "returns a preview");
    if (out == nullptr) return;
    doc.Parse(out);
    check(!doc.HasParseError() && doc.IsArray() && doc.Size() == 3, "a JSON array of the three messages");
    if (doc.HasParseError() || !doc.IsArray() || doc.Size() != 3) return;
    check_eq(doc[0]["content"].GetString(),
             "[Time: Saturday, October 10, 2026, 2:32 PM UTC+8 Asia/Shanghai]\n你好",
             "annotations are read from the JSON");
    check_eq(doc[1]["content"].GetString(), "你好！", "the reply passes through");
    check_eq(doc[2]["content"].GetString(), "[Tone: much flatter than usual (measured from the voice)]\n累了",
             "a time of the wrong type is ignored, the arousal beside it is not");
    check(yoai_chat_preview("{}") == nullptr, "an argument that is not an array is NULL");
}

}  // namespace

int main() {
    time_checks();
    arousal_checks();
    abi_checks();

    std::printf("\n%s\n", g_failures == 0 ? "all annotation checks passed" : "SOME ANNOTATION CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
