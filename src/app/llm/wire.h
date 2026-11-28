// Wire codecs for the two chat formats msga speaks — pure
// functions, no network, so request shaping and response parsing are tested
// against captured payloads.
//
//   OpenAiChat         POST <base>/chat/completions, GET <base>/models,
//                      "Authorization: Bearer". OpenAI itself and every
//                      OpenAI-compatible server (vLLM, Ollama, LM Studio,
//                      LiteLLM, OpenRouter, …); <base> ends in "/v1" by
//                      convention (OpenRouter: "/api/v1").
//   AnthropicMessages  POST <base>/v1/messages, GET <base>/v1/models,
//                      "x-api-key" + "anthropic-version". Anthropic's native
//                      API — its OpenAI-compatibility layer is documented as
//                      not production-ready.
//
// Speech-to-text rides the OpenAI wire only (POST <base>/audio/transcriptions,
// multipart): Anthropic's Messages API has no audio endpoint.
#pragma once

#include "app/llm/types.h"
#include "net/net.h"

#include <string>
#include <string_view>
#include <vector>

namespace llm {

enum class Format : uint8_t { OpenAiChat, AnthropicMessages };

struct Endpoint {
    Format      format = Format::OpenAiChat;
    std::string baseUrl;
    std::string apiKey; // "" → no auth header (local servers without a key)
    // OpenAI proper wants max_completion_tokens (its reasoning models reject
    // max_tokens); compat servers all take max_tokens. Never both.
    bool        maxCompletionTokens = false;
    // Sent as reasoning_effort when set (the OpenAI preset only: compat
    // servers may reject unknown fields). With reasoning on, temperature is
    // dropped — OpenAI's reasoning models reject it.
    std::string reasoningEffort;
};

// Slow local models: a 512-token summary on a CPU-hosted 7B takes a minute.
constexpr int kTimeoutMs = 120000;

net::Request buildChat(const Endpoint &ep, const Request &req);
net::Request buildListModels(const Endpoint &ep);

// ── Speech-to-text ──────────────────────────────────────────────────────────
struct TranscriptionInput {
    std::string              audio;
    std::string              fileName; // with the real extension: servers sniff the format by it
    std::string              mimeType; // the part's Content-Type ("audio/mpeg"); "" → octet-stream
    std::string              model;    // "gpt-transcribe", "whisper-1", …
    // Context that steers recognition. `prompt` goes to every server (Whisper
    // reads it as the transcript preceding the audio; the gpt-* transcribe
    // family follows it as instructions). `keywords` (terms to spell
    // exactly) and `languages` (ISO 639-1 hints) exist only on OpenAI's
    // gpt-* models, so they are sent for those alone.
    std::string              prompt;
    std::vector<std::string> keywords;
    std::vector<std::string> languages;
};

struct TranscriptionResult {
    bool        ok = false;
    std::string text;
    std::string error;
};

// Whether the format has a transcription endpoint at all (OpenAiChat only).
bool         supportsTranscription(Format format);
// Multipart POST <base>/audio/transcriptions with `model`, `file` and
// `response_format=json`, plus `prompt` when set and — gpt-* models only —
// one `keywords[]` field per sanitised keyword and one `languages[]` field
// per language. `boundary` is generated when empty (tests pin it).
// Precondition: supportsTranscription(ep.format).
net::Request buildTranscription(
    const Endpoint &ep, const TranscriptionInput &in, std::string_view boundary = {}
);
// Speech-to-text keywords as OpenAI accepts them: trimmed, empty and
// case-insensitive duplicates dropped (first spelling wins), and any term
// containing '<', '>', CR or LF dropped — one such term makes OpenAI reject
// the whole request.
std::vector<std::string> sanitizeTranscriptionKeywords(const std::vector<std::string> &keywords);
// {"text": …} on success; a 2xx that isn't JSON is taken as the plain text
// (servers that ignore response_format). A 404 is a server without STT.
TranscriptionResult      parseTranscription(const net::Response &r);
// The part Content-Type for an audio file extension ("mp3" → "audio/mpeg");
// "" when unknown.
std::string              audioMimeForExtension(std::string_view ext);

// ── Custom servers ──────────────────────────────────────────────────────────

// What the user typed as an OpenAI-compatible server → the base URL to use:
// http:// added when no scheme is given, trailing '/' and a pasted
// "/chat/completions" dropped, "/v1" appended to a bare host. "" when the
// result is no usable http(s) URL.
std::string normalizeOpenAiBaseUrl(std::string_view raw);
// Plain http to a host that is not this machine or the local network
// (loopback, *.localhost, *.local, link-local, RFC 1918, fc00::/7 are
// local): the API key would cross the internet unencrypted.
bool        isCleartextRemote(std::string_view url);
// A sentence for net's "no HTTP answer" reasons ("dns", "timeout: …");
// "network error" for anything else.
std::string transportFailure(std::string_view netError);

// A finished net::Response (status 0: no HTTP answer, see its error).
ChatResult   parseChat(Format format, const net::Response &r);
ModelsResult parseModels(const net::Response &r);

} // namespace llm
