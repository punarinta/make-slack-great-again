// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "openai_provider.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// Auth: API key (BYOK) only. OpenAI's "Sign in with ChatGPT" OAuth client is
// reserved for its own tooling (Codex), and there is no public OAuth-app
// registration for third parties. Users create a key in their own platform
// account; it is stored locally and sent only to api.openai.com. If OpenAI
// ever opens third-party OAuth, fill in oauthConfig() and the Connect button
// reappears.
static constexpr const char *kApiKeyUrl = "https://platform.openai.com/api-keys";

static constexpr const char *kChatUrl      = "https://api.openai.com/v1/chat/completions";
static constexpr const char *kDefaultModel = "gpt-5.1";

OpenAiProvider::OpenAiProvider(QObject *parent) : LlmProviderBase("openai", "OpenAI", parent) {}

QString OpenAiProvider::apiKeyUrl() const {
    return kApiKeyUrl;
}

OAuthConfig OpenAiProvider::oauthConfig() const {
    return {}; // empty clientId → supportsOAuth() == false
}

void OpenAiProvider::sendChat(
    const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError
) {
    QNetworkRequest netReq((QUrl(kChatUrl)));
    netReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    netReq.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(creds().apiKey).toUtf8());

    QJsonArray messages;
    if (!req.system.isEmpty())
        messages.append(QJsonObject{{"role", "system"}, {"content", req.system}});
    for (const auto &m : req.messages) {
        messages.append(
            QJsonObject{
                {"role", m.role == Llm::Message::Role::User ? "user" : "assistant"},
                {"content", m.text},
            }
        );
    }
    const QJsonObject body{
        {"model", req.model.isEmpty() ? kDefaultModel : req.model},
        {"max_completion_tokens", req.maxTokens},
        {"messages", messages},
    };

    auto *reply = nam()->post(netReq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [reply, onResponse = std::move(onResponse), onError = std::move(onError)] {
            reply->deleteLater();
            const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            if (obj.contains("error")) {
                if (onError)
                    onError(obj.value("error").toObject().value("message").toString("unknown"));
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                if (onError)
                    onError(reply->errorString());
                return;
            }
            const QJsonObject choice = obj.value("choices").toArray().at(0).toObject();
            Llm::Response     resp;
            resp.model      = obj.value("model").toString();
            resp.stopReason = choice.value("finish_reason").toString();
            resp.text       = choice.value("message").toObject().value("content").toString();
            if (onResponse)
                onResponse(std::move(resp));
        }
    );
}
