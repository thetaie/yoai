// The prompt table, through the class and through the C ABI a host reads.
//
//     build/yoai-prompts-test
//     build/yoai-prompts-test --dump
//
// With no arguments it checks; with --dump it prints yoai_prompts(), which is
// the quickest way to see exactly what a host will be handed.
//
// What it is here for:
//
//   1. The built-in names. A host stores and sends them, so renaming one is a
//      silent breakage on every platform at once.
//   2. Placeholders. The rules -- declared arguments only, required ones
//      enforced, a value never scanned again -- are checked on a table of the
//      test's own, where every case can be set up; the built-ins that take
//      arguments are rendered for real by tests/llm/annotations_test.cpp.
//   3. turn_system_prompt(). The local model's warm-up is spent on its result,
//      and is wasted unless the turn builds the same string, so the cases a host
//      actually sends -- the default, an empty base, a blank one, not reading --
//      are pinned.
//   4. The JSON. A key that drifts from what yoai.h documents breaks a host
//      that only ever reads the header.

#include "llm/prompt/prompts.hpp"
#include "yoai.h"

#include <rapidjson/document.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using yoai::llm::Prompt;
using yoai::llm::PromptArgument;
using yoai::llm::Prompts;
using yoai::llm::default_turn_system_prompt;
using yoai::llm::turn_system_prompt;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

std::vector<Prompt> test_table() {
    std::vector<Prompt> t;

    Prompt greet;
    greet.name = "greet";
    greet.text = "Hello {{ who }}, {{mood}}. Literal {{other}} and {{who}} again.";
    PromptArgument who;
    who.name     = "who";
    who.required = true;
    PromptArgument mood;
    mood.name = "mood";
    greet.arguments.push_back(who);
    greet.arguments.push_back(mood);
    t.push_back(greet);

    Prompt dup;
    dup.name = "greet";
    dup.text = "the second entry of a repeated name";
    t.push_back(dup);

    Prompt plain;
    plain.name    = "plain";
    plain.text    = "no placeholders {{";
    plain.grammar = "root ::= \"x\"";
    t.push_back(plain);

    return t;
}

void class_checks() {
    std::printf("-- the class, on a table of the test's own\n");
    Prompts p(test_table());

    const std::vector<Prompt> all = p.list();
    check(all.size() == 2, "a repeated name keeps only its first entry");
    check(all.size() == 2 && all[0].name == "greet" && all[1].name == "plain", "table order is kept");

    std::string out;
    std::string err;
    std::map<std::string, std::string> args;
    args["who"]  = "Ann {{mood}}";
    args["mood"] = "glad";
    args["extra"] = "ignored";
    check(p.render("greet", args, &out, &err), "renders with every argument");
    check(out == "Hello Ann {{mood}}, glad. Literal {{other}} and Ann {{mood}} again.",
          "fills declared placeholders, spaces inside allowed, values not scanned again, "
          "undeclared left as written");

    args.erase("mood");
    check(p.render("greet", args, &out, &err) && out.find(", .") != std::string::npos,
          "an optional argument not passed becomes empty");

    args.erase("who");
    check(!p.render("greet", args, &out, &err), "a required argument missing fails");
    check(err.find("who") != std::string::npos, "and the error names it");

    check(!p.render("nope", args, &out, &err), "an unknown name fails");
    check(p.render("plain", args, &out, &err) && out == "no placeholders {{",
          "an unclosed {{ is left alone");

    check(p.set_text("plain", "changed"), "set_text on a known name");
    Prompt got;
    check(p.get("plain", &got) && got.text == "changed" && got.overridden, "get sees the replacement");
    check(got.grammar == "root ::= \"x\"", "and the grammar is the default's");
    check(!p.set_text("nope", "x"), "set_text on an unknown name fails");
    check(p.reset("plain") && p.get("plain", &got) && got.text == "no placeholders {{" && !got.overridden,
          "reset brings the default back");
    check(!p.reset("nope"), "reset on an unknown name fails");
}

void builtin_checks() {
    std::printf("-- the built-in table\n");
    const std::vector<Prompt> all = Prompts::instance().list();
    check(all.size() == 5, "five built-in prompts");
    check(all.size() == 5 && all[0].name == "system" && all[1].name == "spoken_style" &&
              all[2].name == "text_axes" && all[3].name == "context_time" &&
              all[4].name == "context_arousal",
          "named system, spoken_style, text_axes, context_time, context_arousal, in that order");
    check(all.size() == 5 && all[3].arguments.size() == 1 && all[3].arguments[0].required &&
              all[4].arguments.size() == 1 && all[4].arguments[0].required,
          "the two context prompts each take one required argument");

    std::map<std::string, std::string> none;
    std::string out;
    check(!Prompts::instance().render(yoai::llm::kPromptContextTime, none, &out, nullptr),
          "context_time refuses to render without its time");

    Prompt axes;
    check(Prompts::instance().get(yoai::llm::kPromptTextAxes, &axes) && !axes.grammar.empty(),
          "text_axes carries its grammar");
    check(axes.grammar.find("score ::= \"-1\" | \"-0.5\" | \"0\" | \"0.5\" | \"1\"") != std::string::npos,
          "and the grammar is the five-point scale");

    Prompt spoken;
    Prompts::instance().get(yoai::llm::kPromptSpokenStyle, &spoken);
    const std::string internal = std::string("\n\n") + yoai::llm::kInternalText;
    const std::string base     = "You are a helper.";
    check(turn_system_prompt(base, false) == base + internal, "not reading: the base, then the internal line");
    check(turn_system_prompt("", false).empty(), "not reading, no base: nothing at all");
    check(turn_system_prompt("  ", false).empty(), "not reading, a blank base: nothing at all");
    check(turn_system_prompt(base, true) == base + "\n\n" + spoken.text + internal,
          "reading: the base, the spoken style, the internal line, a blank line between each");
    check(turn_system_prompt("", true) == spoken.text + internal,
          "reading, no base: the spoken style and the internal line");
    check(turn_system_prompt(" \n\xE3\x80\x80", true) == spoken.text + internal,
          "reading, a base of whitespace and U+3000: the spoken style and the internal line");

    Prompt system;
    Prompts::instance().get(yoai::llm::kPromptSystem, &system);
    check(system.text.find(yoai::llm::kInternalText) == std::string::npos,
          "the system prompt does not carry the internal line itself");
    check(default_turn_system_prompt(false) == system.text + internal,
          "no base given, not reading: the system prompt and the internal line");
    check(default_turn_system_prompt(true) == system.text + "\n\n" + spoken.text + internal,
          "no base given, reading: the system prompt, the spoken style, the internal line");
}

void abi_checks() {
    std::printf("-- the C ABI\n");
    rapidjson::Document doc;
    doc.Parse(yoai_prompts());
    check(!doc.HasParseError() && doc.IsArray() && doc.Size() == 5, "yoai_prompts is a JSON array of five");
    if (!doc.HasParseError() && doc.IsArray() && doc.Size() == 5) {
        const rapidjson::Value& p = doc[2];
        check(p.HasMember("name") && p["name"].IsString() && std::strcmp(p["name"].GetString(), "text_axes") == 0,
              "each entry has its name");
        check(p.HasMember("description") && p["description"].IsString(), "a description");
        check(p.HasMember("arguments") && p["arguments"].IsArray(), "an arguments array");
        check(p.HasMember("text") && p["text"].IsString(), "the text");
        check(p.HasMember("grammar") && p["grammar"].IsString() && p["grammar"].GetStringLength() > 0,
              "the grammar");
        check(p.HasMember("overridden") && p["overridden"].IsBool() && !p["overridden"].GetBool(),
              "and whether it is overridden");
    }

    const char* sys = yoai_prompt_get("system", nullptr);
    check(sys != nullptr && std::strlen(sys) > 0, "yoai_prompt_get returns a built-in's text");
    check(std::strlen(yoai_prompt_last_error()) == 0, "and leaves no error");

    check(yoai_prompt_get("nope", nullptr) == nullptr, "an unknown name is NULL");
    check(std::strstr(yoai_prompt_last_error(), "nope") != nullptr, "and the error names it");
    check(yoai_prompt_get("system", "[1]") == nullptr, "arguments that are not an object are refused");
    check(yoai_prompt_get("system", "{\"a\":1}") == nullptr, "and so is a value that is not a string");
    check(yoai_prompt_get("system", "{\"a\":\"b\"}") != nullptr, "an undeclared string argument is ignored");

    const std::string internal = std::string("\n\n") + yoai::llm::kInternalText;
    check(yoai_prompt_set("spoken_style", "SPEAK") == 0, "yoai_prompt_set replaces a text");
    check(std::string(yoai_prompt_turn_system("base", 1)) == "base\n\nSPEAK" + internal,
          "and yoai_prompt_turn_system uses the replacement");
    doc.Parse(yoai_prompts());
    check(!doc.HasParseError() && doc.IsArray() && doc.Size() == 5 && doc[1]["overridden"].GetBool(),
          "the listing shows it as overridden");
    check(yoai_prompt_set("spoken_style", nullptr) == 0, "NULL resets it");
    check(std::string(yoai_prompt_turn_system("base", 1)) != "base\n\nSPEAK" + internal,
          "and the default is back");
    check(yoai_prompt_set("nope", "x") != 0, "an unknown name is refused");
    check(std::string(yoai_prompt_turn_system(nullptr, 0)) == yoai_prompt_get("system", nullptr) + internal,
          "a NULL base is the system prompt, then the internal line");
    check(std::string(yoai_prompt_turn_system("", 0)).empty(), "an empty base, not reading, is none");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        std::printf("%s\n", yoai_prompts());
        return 0;
    }

    class_checks();
    builtin_checks();
    abi_checks();

    std::printf("\n%s\n", g_failures == 0 ? "all prompt checks passed" : "SOME PROMPT CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
