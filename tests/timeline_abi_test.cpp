// The day log's C ABI, exercised through the same door a host uses.
//
// timeline_test.cpp already pins the RULES. This one pins the MARSHALLING,
// which is a different kind of thing to get wrong and is not covered by that
// file at all: the rules can be perfect while the host still reads `endedAt`
// under a key that is spelled `ended_at`, or sees a stale error message from a
// call two screens ago, or loses the store under a thread that was mid-read.
//
// The three things it is really here for:
//
//   1. The JSON keys and their shape. A host only ever reads yoai.h, so a
//      key that drifts from what the header documents is a silent breakage on
//      every platform at once.
//   2. last_error(). The screens act on it -- "an end in the future is refused"
//      is a sentence the UI turns into a different dialog -- so it has to be
//      this call's reason and not the previous one's.
//   3. Open/close. Reopening the same directory is a no-op, a failed open
//      leaves nothing open, and reads before an open fail rather than crash.
//
// Run it directly, against an EMPTY scratch directory:
//
//     build/yoai-timeline-abi-test <dir>
//
// Empty because several assertions are about what a fresh install looks like,
// and a directory with a yoai.db already in it fails them for a reason
// that has nothing to do with the code. That is checked below and reported as
// itself rather than as two mystifying failures.

#include "yoai.h"

#include <rapidjson/document.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

// A failure that should not have happened prints the core's own reason, so a
// rule failing for an unrelated cause still says why.
void did(int rc, const char* what) {
    if (rc != 0) {
        std::printf("FAIL  %s: %s\n", what, yoai_timeline_last_error());
        ++g_failures;
    } else {
        std::printf("ok    %s\n", what);
    }
}

// Parsing here rather than matching substrings: a test that greps for
// `"stateId"` passes on a payload that is not JSON at all.
bool parse(const char* json, rapidjson::Document* doc) {
    if (json == nullptr) {
        return false;
    }
    doc->Parse(json);
    return !doc->HasParseError();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: yoai-timeline-abi-test <scratch-dir>\n");
        return 2;
    }
    const std::string dir = argv[1];

    //--------------------------------------------------------------------------
    // Before anything is open
    //--------------------------------------------------------------------------

    check(yoai_timeline_opened() == 0, "nothing is open at startup");
    check(yoai_timeline_now() == nullptr, "a read before open fails rather than crashing");
    check(std::strlen(yoai_timeline_last_error()) > 0, "and says why");
    check(yoai_timeline_switch_to("work") != 0, "so does a write");

    // Pure arithmetic, so these answer with no log open at all.
    const int today = yoai_timeline_today();
    check(today > 20000, "today is a plausible day number");
    check(std::string(yoai_timeline_day_to_iso(0)) == "1970-01-01", "day 0 is the epoch");
    check(yoai_timeline_iso_to_day("1970-01-01") == 0, "and round-trips back");
    check(yoai_timeline_iso_to_day("not-a-date") == -1, "an unparseable date is refused");
    check(yoai_timeline_iso_to_day(nullptr) == -1, "so is no date at all");

    //--------------------------------------------------------------------------
    // Opening
    //--------------------------------------------------------------------------

    did(yoai_timeline_open(dir.c_str()), "open");
    check(yoai_timeline_opened() == 1, "it is open");

    // Before asserting anything about a fresh install, make sure this IS one.
    {
        rapidjson::Document fresh;
        const char* json = yoai_timeline_now();
        if (json == nullptr || !parse(json, &fresh) ||
            !fresh["current"].IsNull() || !fresh["lastEnded"].IsNull()) {
            std::fprintf(stderr,
                         "%s already holds a day log -- pass an EMPTY directory.\n",
                         dir.c_str());
            return 2;
        }
    }
    did(yoai_timeline_open(dir.c_str()), "opening the same directory again is a no-op");

    check(yoai_timeline_open((dir + "/does/not/exist").c_str()) != 0,
          "opening a directory that is not there fails");
    check(yoai_timeline_opened() == 0, "and leaves NOTHING open, rather than the old one");
    did(yoai_timeline_open(dir.c_str()), "reopen");

    //--------------------------------------------------------------------------
    // now(), on a fresh store and after a switch
    //--------------------------------------------------------------------------

    rapidjson::Document d;
    check(parse(yoai_timeline_now(), &d), "now() returns JSON on an empty store");
    check(d.IsObject() && d.HasMember("current") && d["current"].IsNull(),
          "a fresh store has no current state, spelled null");
    check(d.HasMember("lastEnded") && d["lastEnded"].IsNull(), "and nothing to undo");

    did(yoai_timeline_switch_to("work"), "switch to work");
    check(parse(yoai_timeline_now(), &d), "now() after switching");
    check(d["current"].IsObject(), "current is an object now");

    // Every key the header documents, by the name the header documents. This is
    // the assertion the rest of the app is built on top of.
    const rapidjson::Value& cur = d["current"];
    check(cur.HasMember("id") && cur["id"].IsInt64(), "entry.id");
    check(cur.HasMember("stateId") && std::string(cur["stateId"].GetString()) == "work",
          "entry.stateId is the state that was switched to");
    check(cur.HasMember("open") && cur["open"].GetBool(), "entry.open is true while it runs");
    check(cur.HasMember("startedAt") && cur["startedAt"].GetInt64() > 0, "entry.startedAt");
    check(cur.HasMember("startedTz") && cur["startedTz"].IsInt(), "entry.startedTz");
    check(cur.HasMember("endedAt") && cur["endedAt"].GetInt64() == 0,
          "entry.endedAt is 0 while open, not absent");
    check(cur.HasMember("endedTz") && cur["endedTz"].IsInt(), "entry.endedTz is present but unset");
    check(cur.HasMember("localDay") && cur["localDay"].GetInt() == today, "entry.localDay is today");
    check(cur.HasMember("localDayEnd") && cur["localDayEnd"].GetInt() == 0,
          "entry.localDayEnd is 0 while open");
    check(cur.HasMember("notes") && cur["notes"].IsArray() && cur["notes"].Empty(),
          "entry.notes is an empty array, not null");

    //--------------------------------------------------------------------------
    // Notes
    //--------------------------------------------------------------------------

    did(yoai_timeline_note_add("noodles"), "add a note");
    // Non-ASCII through the ABI, because every string on this path is UTF-8 and
    // the interface language is Chinese by default.
    did(yoai_timeline_note_add("面很咸"), "add a note with non-ASCII text");
    check(parse(yoai_timeline_now(), &d), "now() after the notes");
    const rapidjson::Value& notes = d["current"]["notes"];
    check(notes.Size() == 2, "both notes came back");
    check(std::string(notes[0]["text"].GetString()) == "noodles", "oldest first");
    check(std::string(notes[1]["text"].GetString()) == "面很咸", "UTF-8 survived the round trip");
    check(notes[0].HasMember("id") && notes[0].HasMember("at"), "a note carries id and at");

    //--------------------------------------------------------------------------
    // Switching, ending, undo -- the shape of what the home screen reads
    //--------------------------------------------------------------------------

    did(yoai_timeline_switch_to("eat"), "switch to eat");
    check(parse(yoai_timeline_now(), &d), "now() after the second switch");
    check(std::string(d["current"]["stateId"].GetString()) == "eat", "eat is current");
    check(std::string(d["lastEnded"]["stateId"].GetString()) == "work", "work is what just ended");
    check(d["lastEnded"]["endedAt"].GetInt64() == d["current"]["startedAt"].GetInt64(),
          "no gap between them, as read through the ABI");
    check(!d["lastEnded"]["open"].GetBool(), "the closed one reports open:false");

    did(yoai_timeline_end(), "end the current state");
    check(parse(yoai_timeline_now(), &d), "now() while idle");
    check(d["current"].IsNull(), "idle means current is null");
    check(d["lastEnded"].IsObject() && std::string(d["lastEnded"]["stateId"].GetString()) == "eat",
          "and lastEnded is what the undo button would restore");

    did(yoai_timeline_undo(), "undo");
    check(parse(yoai_timeline_now(), &d), "now() after undo");
    check(std::string(d["current"]["stateId"].GetString()) == "eat", "undo put eat back");

    //--------------------------------------------------------------------------
    // last_error() is THIS call's reason
    //--------------------------------------------------------------------------

    check(yoai_timeline_undo() != 0, "undo is refused while something is open");
    const std::string undo_err = yoai_timeline_last_error();
    check(!undo_err.empty(), "and the refusal has a reason");

    check(yoai_timeline_switch_to("not-a-state") != 0, "an unknown state id is refused");
    const std::string switch_err = yoai_timeline_last_error();
    check(!switch_err.empty() && switch_err != undo_err,
          "and reports ITS OWN reason, not the previous call's");

    check(yoai_timeline_end_at(1) != 0, "an end before the entry started is refused");
    check(std::string(yoai_timeline_last_error()) != switch_err, "with its own reason again");

    did(yoai_timeline_switch_to("eat"), "a successful call after the refusals");
    check(std::strlen(yoai_timeline_last_error()) == 0,
          "success clears the error, so a stale one is never read as fresh");

    //--------------------------------------------------------------------------
    // The list reads
    //--------------------------------------------------------------------------

    check(parse(yoai_timeline_recent(24), &d), "recent() returns JSON");
    check(d.IsArray(), "recent() is an array, not an object wrapping one");
    check(d.Size() >= 2, "it holds what was logged above");
    check(d[0].HasMember("stateId") && d[0].HasMember("notes"), "its members are entries");
    // Newest first, per the header.
    check(d[0]["startedAt"].GetInt64() >= d[d.Size() - 1]["startedAt"].GetInt64(),
          "recent() comes back newest first");
    check(yoai_timeline_recent(0) == nullptr, "a window of no hours is refused");
    check(std::strlen(yoai_timeline_last_error()) > 0, "and says why");

    rapidjson::Document day;
    check(parse(yoai_timeline_day(today), &day), "day() returns JSON");
    check(day.IsArray() && day.Size() >= 2, "today holds what was logged above");
    check(day[0]["startedAt"].GetInt64() <= day[day.Size() - 1]["startedAt"].GetInt64(),
          "day() comes back oldest first -- the opposite of recent()");

    rapidjson::Document empty;
    check(parse(yoai_timeline_day(0), &empty), "a day nobody lived in reads fine");
    check(empty.IsArray() && empty.Empty(), "and comes back as an empty array, not an error");

    //--------------------------------------------------------------------------
    // The buffers are per-function, as the header promises
    //--------------------------------------------------------------------------

    const char* recent_json = yoai_timeline_recent(24);
    const std::string recent_before = recent_json == nullptr ? "" : recent_json;
    yoai_timeline_now();
    yoai_timeline_day(today);
    check(recent_json != nullptr && recent_before == recent_json,
          "recent()'s result survives a now() and a day() beside it");

    //--------------------------------------------------------------------------
    // Closing
    //--------------------------------------------------------------------------

    yoai_timeline_close();
    check(yoai_timeline_opened() == 0, "closed");
    check(yoai_timeline_now() == nullptr, "and reads fail again afterwards");
    yoai_timeline_close();
    check(yoai_timeline_opened() == 0, "closing twice is safe");

    std::printf("\n%s\n", g_failures == 0 ? "the ABI holds" : "SOMETHING IS WRONG");
    return g_failures == 0 ? 0 : 1;
}
