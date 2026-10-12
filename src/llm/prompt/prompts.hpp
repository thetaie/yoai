// The text the core and its hosts send to a language model, kept in one place.
//
// Before this, each host carried its own copy, and the copies drifted: the
// spoken-style instruction existed only in the desktop client, the HarmonyOS
// chat sent no system prompt at all, and the text-axes scoring prompt with its
// grammar existed only in the HarmonyOS client. A host now lists and fetches
// prompts here instead of writing them.
//
// Each prompt is shaped like an MCP prompt -- a name, a description, declared
// arguments, and a text with {{argument}} placeholders -- so the same table can
// back MCP's prompts/list and prompts/get later without a second format. One
// field is not MCP's: `grammar`, the GBNF a prompt's reply must follow when the
// reply is a value rather than prose. An MCP listing leaves it out.
//
// Every prompt has a DEFAULT, compiled in, and a text that can be replaced at
// run time and reset to that default. The replacement is for tuning wording
// without rebuilding the core on every platform; a shipping host has no reason
// to call it.
//
// Errors are returned, never thrown. Thread-safe: one instance may be read and
// edited from several threads.

#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace yoai {
namespace llm {

// The built-in prompts' names. Stable: a host stores and sends them.
extern const char* const kPromptSystem;          // the assistant's default system prompt
extern const char* const kPromptSpokenStyle;     // how to write a reply that is read aloud
extern const char* const kPromptTextAxes;        // score a transcript's valence and dominance
extern const char* const kPromptContextTime;     // a message's time, in its annotation line
extern const char* const kPromptContextArousal;  // a message's arousal, in its annotation line

// The line turn_system_prompt() ends every system prompt with: all of it is
// internal. Not in the table, so a prompt the user writes cannot leave it out.
extern const char* const kInternalText;

struct PromptArgument {
    std::string name;
    std::string description;
    bool        required = false;
};

struct Prompt {
    std::string                 name;
    std::string                 description;
    std::vector<PromptArgument> arguments;
    std::string                 text;          // the current text, placeholders unfilled
    std::string                 grammar;       // GBNF for the reply, or empty
    bool                        overridden = false;  // `text` is not the default
};

class Prompts {
public:
    // The built-in table, shared by the whole process. Its defaults are in
    // prompts.cpp.
    static Prompts& instance();

    // A table of the caller's own, for tests. `defaults` keeps its order, and a
    // name that repeats keeps its first entry.
    explicit Prompts(const std::vector<Prompt>& defaults);

    // Every prompt, in table order, with replacements applied.
    std::vector<Prompt> list() const;

    // One prompt by name. False for a name not in the table.
    bool get(const std::string& name, Prompt* out) const;

    // Replace a prompt's text. False for a name not in the table. The arguments
    // and grammar stay the default's: a replacement changes wording, not shape.
    bool set_text(const std::string& name, const std::string& text);

    // Back to the default text. False for a name not in the table.
    bool reset(const std::string& name);

    // The text with every DECLARED argument's placeholder filled. A placeholder
    // naming an argument the prompt does not declare is left as written, so a
    // literal "{{" in a prompt never needs escaping. An optional argument not
    // passed becomes empty; a required one not passed is an error, and so is an
    // unknown name. Arguments passed but not declared are ignored, as MCP's
    // prompts/get allows. `err` says why on failure and may be null.
    bool render(const std::string&                        name,
                const std::map<std::string, std::string>& args,
                std::string*                              out,
                std::string*                              err) const;

private:
    Prompts(const Prompts&);
    Prompts& operator=(const Prompts&);

    const Prompt* find(const std::string& name) const;  // under mu_

    mutable std::mutex                 mu_;
    std::vector<Prompt>                defaults_;
    std::map<std::string, std::string> overrides_;
};

// The system prompt one turn is sent with: the host's `base` -- the one the user
// edits, which may be empty -- the spoken-style prompt after it when the reply
// will be read aloud, and kInternalText last, each a paragraph of its own. A
// part that is blank is left out, along with the blank line that would join it;
// with neither of the first two, the result is empty -- no system message.
//
// A local model prefills the front of this ahead of the turn, and that work is
// wasted unless the turn sends the same text to the token. So the turn and the
// warm-up must both build it here, from the same `base` and `reading`.
std::string turn_system_prompt(const std::string& base, bool reading);

// The same, with the table's system prompt as `base`: what a host sends until
// the user writes a prompt of their own. An empty `base` above is different --
// that is a user who cleared it, and gets no system prompt. A separate name, not
// an overload: a string literal converts to bool before std::string, so an
// overload on bool would take a mistyped call without a word.
std::string default_turn_system_prompt(bool reading);

}  // namespace llm
}  // namespace yoai
