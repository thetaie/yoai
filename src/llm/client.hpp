#pragma once

// Uniform streaming-LLM client interface.
//
// Every concrete client in this directory exposes the same
// ainvoke(messages, system_prompt, on_event) shape, but they are unrelated
// types: an OpenAI-compatible HTTP client and an on-device llama.cpp engine
// have nothing in common internally. The provider registry
// (platform/c_api.cpp) has to hold whichever one a model resolves to behind a
// single type and pick it by id at runtime.
//
// `Client` is that common interface; `ClientAdapter<T>` wraps any of the
// concrete clients without modifying them (they stay standalone and directly
// usable). Build one with make_client<T>(options):
//
//     std::shared_ptr<llm::Client> c =
//         llm::make_client<llm::OpenAIChatClient>(opts);
//     c->ainvoke(messages, prompt, on_event);

#include "llm/types.hpp"   // ChatMessage, EventHandler

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bodyclock { namespace llm {

//------------------------------------------------------------------------------

class Client {
public:
    virtual ~Client() = default;

    // Stream a chat completion, firing `on_event` per event on the calling
    // thread. Returns true when the request completed (including server-side
    // error events delivered through the handler), false on transport failure
    // or if the handler aborted the stream. Same contract as the concrete
    // clients' ainvoke().
    //
    // `user` carries the caller's identity for the turn; a client that runs
    // tools locally forwards it to its tool executor (others ignore it).
    virtual bool ainvoke(const std::vector<ChatMessage>& messages,
                         const std::string&              system_prompt,
                         const EventHandler&             on_event,
                         const UserContext&              user = UserContext()) = 0;
};

//------------------------------------------------------------------------------

// Adapts any concrete client (a type with the matching ainvoke signature) to
// the Client interface, constructing the wrapped instance in place. The
// concrete clients are non-copyable and non-movable, so the adapter forwards
// constructor arguments straight into the member rather than accepting a
// prebuilt instance by value.
template <typename T>
class ClientAdapter : public Client {
public:
    template <typename... Args>
    explicit ClientAdapter(Args&&... args) : impl_(std::forward<Args>(args)...) {}

    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string&              system_prompt,
                 const EventHandler&             on_event,
                 const UserContext&              user = UserContext()) override {
        return impl_.ainvoke(messages, system_prompt, on_event, user);
    }

    T&       inner()       { return impl_; }
    const T& inner() const { return impl_; }

private:
    T impl_;
};

// Construct a concrete client from its options and return it behind a Client
// pointer. `T` is e.g. OpenAIChatClient; `Options` is its options struct, moved
// into T's constructor (which takes options by value).
template <typename T, typename Options>
std::shared_ptr<Client> make_client(Options options) {
    return std::make_shared<ClientAdapter<T> >(std::move(options));
}

}
}
