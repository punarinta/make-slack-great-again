// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "llm_provider_base.h"

class AnthropicProvider : public LlmProviderBase {
    Q_OBJECT
public:
    explicit AnthropicProvider(QObject *parent = nullptr);

    [[nodiscard]] QString apiKeyUrl() const override;

protected:
    [[nodiscard]] OAuthConfig oauthConfig() const override;
    void
    sendChat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError) override;
};
