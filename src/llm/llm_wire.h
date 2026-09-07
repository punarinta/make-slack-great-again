// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Wire codecs for the two chat-completion formats msga speaks — pure functions
// (no network, no QObject) so request shaping and response parsing are unit-
// testable against captured payloads.
//
//   OpenAiChat         POST <base>/chat/completions, GET <base>/models,
//                      "Authorization: Bearer". Spoken by OpenAI itself and by
//                      every OpenAI-compatible server (vLLM, Ollama, LM Studio,
//                      LiteLLM, OpenRouter, …). <base> ends in "/v1" by
//                      convention (OpenRouter: "/api/v1").
//   AnthropicMessages  POST <base>/v1/messages, GET <base>/v1/models,
//                      "x-api-key" + "anthropic-version". Anthropic's native
//                      API — kept because its OpenAI-compatibility layer is
//                      documented as "not production-ready" and drops
//                      structured outputs / prompt caching / refusal detail.
#pragma once

#include "llm_types.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace LlmWire {

enum class Format { OpenAiChat, AnthropicMessages };

struct Endpoint {
    Format  format = Format::OpenAiChat;
    QString baseUrl;
    QString apiKey; // empty → no auth header (local servers without --api-key)
    // OpenAI proper wants `max_completion_tokens` (`max_tokens` is deprecated
    // and rejected by its reasoning models); compat servers universally
    // understand `max_tokens`, and sending both is an error. Never both.
    bool    maxCompletionTokens = false;
    // Sent as `reasoning_effort` when non-empty (OpenAI preset only — compat
    // servers may reject unknown fields). When reasoning is on, `temperature`
    // is dropped: OpenAI's reasoning models reject it.
    QString reasoningEffort;
};

struct HttpRequest {
    QUrl                                 url;
    QList<QPair<QByteArray, QByteArray>> headers; // Content-Type included for POSTs
    QByteArray                           body;    // empty → GET
};

struct ChatResult {
    bool          ok = false;
    Llm::Response response;
    QString       error; // human-readable, set when !ok
};

struct ModelsResult {
    bool        ok = false;
    QStringList models; // ids, server order
    QString     error;
};

HttpRequest buildChat(const Endpoint &ep, const Llm::Request &req);
HttpRequest buildListModels(const Endpoint &ep);

// httpStatus 0 = transport failure (body may hold the Qt error string).
ChatResult   parseChat(Format format, int httpStatus, const QByteArray &body);
ModelsResult parseModels(Format format, int httpStatus, const QByteArray &body);

// Tidies a user-typed OpenAI-compatible base URL: trims, drops a trailing "/",
// strips a pasted "/chat/completions", and appends "/v1" when there is no path
// at all ("http://host:8000" → "http://host:8000/v1"). Anything else is kept —
// OpenRouter's "/api/v1" is legitimate. Returns empty for an unusable URL.
QString normalizeOpenAiBaseUrl(const QString &raw);

// True for http:// URLs to a host that is not loopback / link-local / RFC 1918
// — the API key would travel in cleartext across a real network.
bool isCleartextRemote(const QString &url);

} // namespace LlmWire
