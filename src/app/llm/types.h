// Provider-neutral request / response types of the LLM layer. Everything that asks a model
// something — summaries now, voice clean-up and triage later — builds a Request and reads a
// Response; which endpoint served it is the Service's business (service.h).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llm {

struct Message {
    enum class Role : uint8_t { User, Assistant };
    Role        role = Role::User;
    std::string text;
};

struct Request {
    std::string           system;   // optional system prompt
    std::vector<Message>  messages; // alternating user/assistant, the first a user one
    std::string           model;    // "" → the provider's model
    int                   maxTokens = 4096;
    // Unset → the provider's default. Reasoning models reject it, so the
    // wire leaves it out whenever reasoning is on (wire.h).
    std::optional<double> temperature;
};

struct Response {
    std::string text;
    std::string model;      // the model that actually answered
    std::string stopReason; // provider-specific ("end_turn", "stop", "length", …)
};

struct ChatResult {
    bool        ok = false;
    Response    response;
    std::string error; // human-readable, set when !ok
};

struct ModelsResult {
    bool                     ok = false;
    std::vector<std::string> models; // ids, in the server's order
    std::string              error;
};

} // namespace llm
