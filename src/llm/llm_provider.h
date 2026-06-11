// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Abstract LLM provider. Callers never see which vendor backs a request —
// they talk to LlmService, which routes to whichever provider is connected
// and selected as default.
#pragma once

#include <QObject>

#include "llm_types.h"

class LlmProvider : public QObject {
    Q_OBJECT
public:
    enum class AuthState { Disconnected, Connecting, Connected };
    enum class AuthMethod { None, OAuth, ApiKey };

    using QObject::QObject;

    // Stable id used for persistence ("anthropic", "openai").
    [[nodiscard]] virtual QString id() const          = 0;
    // Vendor name shown in the UI (not translated — product name).
    [[nodiscard]] virtual QString displayName() const = 0;

    [[nodiscard]] virtual AuthState  authState() const    = 0;
    [[nodiscard]] virtual AuthMethod authMethod() const   = 0;
    // "jane@example.com" for OAuth, masked key ("sk-…f3a9") for API keys.
    [[nodiscard]] virtual QString    accountLabel() const = 0;

    // True only when the vendor runs a third-party OAuth program we are
    // registered with. Neither Anthropic nor OpenAI offers one today, so
    // API keys (BYOK) are the supported auth method — the UI hides the
    // OAuth button when this is false.
    [[nodiscard]] virtual bool    supportsOAuth() const = 0;
    // Vendor console page where the user creates an API key.
    [[nodiscard]] virtual QString apiKeyUrl() const     = 0;

    // Browser-based OAuth (PKCE + loopback redirect). Emits authStateChanged()
    // on completion, authFailed(reason) on error or user cancel. No-op
    // (authFailed) when supportsOAuth() is false.
    virtual void connectOAuth()                    = 0;
    // Direct API-key auth. Validates shape only; first request proves the key.
    virtual void connectApiKey(const QString &key) = 0;
    virtual void disconnectAccount()               = 0;

    // One-shot completion. Exactly one of onResponse/onError fires, on the
    // provider's thread (the GUI thread). Refreshes OAuth tokens as needed.
    virtual void
    chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError) = 0;

signals:
    void authStateChanged();
    void authFailed(QString reason);
};
