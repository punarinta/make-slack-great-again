// One configured AI endpoint: a
// wire format, a base URL, an API key and a model.
//
// Two kinds share the struct:
//   presets — Anthropic and OpenAI: fixed id, URL and wire (the table in
//             provider.cpp, also what Settings → AI assistance offers); the
//             user brings a key and may pick a model. Connected once keyed.
//   custom  — any OpenAI-compatible server ("custom-N"): the user's name,
//             URL and model; the key is optional, so it is connected by
//             existing.
// The app's settings (shell::Settings::ai) hold what the user entered; the
// shell turns each entry into a Provider with fromSettings() and hands the
// list to the Service.
#pragma once

#include "app/llm/wire.h"

#include <string>
#include <string_view>

namespace llm {

struct Preset {
    const char *id, *name;
    Format      format;
    const char *baseUrl, *defaultModel;
    // The cheaper model for short, frequent tasks (summaries), and the
    // reasoning effort it runs with: GPT-5.6 reasons at medium by default and
    // max_completion_tokens counts the thinking, so a 512-token summary
    // budget would be eaten by it — the light tier turns reasoning off.
    const char *lightModel, *lightReasoningEffort;
    bool        maxCompletionTokens;
    const char *sttModel; // speech-to-text default; null: the API has none
    const char *keyUrl;   // where to get a key
    const char *models[4];
};

// The preset with this id, or null.
const Preset            *preset(std::string_view id);
// Speech-to-text model a custom server answers to by default.
extern const char *const kCustomSttModel;

struct Provider {
    std::string id, name, baseUrl, apiKey;
    std::string model;      // never empty on a usable provider
    std::string lightModel; // "" → model
    std::string lightReasoningEffort;
    std::string sttModel;
    Format      format              = Format::OpenAiChat;
    bool        maxCompletionTokens = false;
    bool        isPreset            = false;

    bool               connected() const { return isPreset ? !apiKey.empty() : !baseUrl.empty(); }
    const std::string &summaryModel() const { return lightModel.empty() ? model : lightModel; }
    // The wire endpoint for a request on `forModel` (the light model gets its
    // reasoning effort).
    Endpoint           endpoint(std::string_view forModel = {}) const;
};

// A provider from what the settings hold for it: a preset's fixed parts come
// from its table entry (model "" = its default), a custom entry is taken as
// typed.
Provider fromSettings(
    std::string_view id,
    std::string_view name,
    std::string_view url,
    std::string_view key,
    std::string_view model,
    std::string_view sttModel
);

} // namespace llm
