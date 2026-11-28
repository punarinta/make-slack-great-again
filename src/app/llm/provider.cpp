#include "app/llm/provider.h"

namespace llm {

namespace {

const Preset kPresets[] = {
    {"anthropic",
     "Anthropic",
     Format::AnthropicMessages,
     "https://api.anthropic.com",
     "claude-opus-5",
     "claude-haiku-4-5",
     "",
     false,
     nullptr, // the Messages API has no audio endpoint
     "https://console.anthropic.com/settings/keys",
     {"claude-opus-5", "claude-sonnet-5", "claude-haiku-4-5", "claude-fable-5-1"}},
    {"openai",
     "OpenAI",
     Format::OpenAiChat,
     "https://api.openai.com/v1",
     "gpt-5.6-terra",
     "gpt-5.6-luna",
     "none",
     true,
     "gpt-transcribe",
     "https://platform.openai.com/api-keys",
     {"gpt-5.6-terra", "gpt-5.6-sol", "gpt-5.6-luna", "gpt-6-astra"}},
};

} // namespace

const char *const kCustomSttModel = "whisper-1";

const Preset *preset(std::string_view id) {
    for (const Preset &p : kPresets)
        if (id == p.id)
            return &p;
    return nullptr;
}

Endpoint Provider::endpoint(std::string_view forModel) const {
    Endpoint ep;
    ep.format              = format;
    ep.baseUrl             = baseUrl;
    ep.apiKey              = apiKey;
    ep.maxCompletionTokens = maxCompletionTokens;
    if (!forModel.empty() && forModel == lightModel)
        ep.reasoningEffort = lightReasoningEffort;
    return ep;
}

Provider fromSettings(
    std::string_view id,
    std::string_view name,
    std::string_view url,
    std::string_view key,
    std::string_view model,
    std::string_view sttModel
) {
    Provider p;
    p.id       = std::string(id);
    p.name     = std::string(name);
    p.apiKey   = std::string(key);
    p.model    = std::string(model);
    p.sttModel = std::string(sttModel);
    if (const Preset *pre = preset(id)) {
        p.isPreset             = true;
        p.format               = pre->format;
        p.baseUrl              = pre->baseUrl;
        p.lightModel           = pre->lightModel;
        p.lightReasoningEffort = pre->lightReasoningEffort;
        p.maxCompletionTokens  = pre->maxCompletionTokens;
        if (p.model.empty())
            p.model = pre->defaultModel;
        if (p.sttModel.empty() && pre->sttModel)
            p.sttModel = pre->sttModel;
    } else {
        p.baseUrl = std::string(url);
        if (p.sttModel.empty())
            p.sttModel = kCustomSttModel;
    }
    return p;
}

} // namespace llm
