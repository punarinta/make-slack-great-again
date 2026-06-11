// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "anthropic_provider.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// Auth: API key (BYOK) only. Anthropic does not run a third-party OAuth
// program — its OAuth clients are reserved for its own tooling, and consumer
// subscription tokens are forbidden (and blocked) in third-party apps. Users
// create a key in their own developer console; it is stored locally and sent
// only to api.anthropic.com. If Anthropic ever opens third-party OAuth,
// fill in oauthConfig() and the Connect button reappears.
static constexpr const char *kApiKeyUrl = "https://console.anthropic.com/settings/keys";

static constexpr const char *kMessagesUrl  = "https://api.anthropic.com/v1/messages";
static constexpr const char *kApiVersion   = "2023-06-01";
static constexpr const char *kDefaultModel = "claude-opus-4-8";

AnthropicProvider::AnthropicProvider(QObject *parent)
    : LlmProviderBase("anthropic", "Anthropic", parent) {}

QString AnthropicProvider::apiKeyUrl() const {
    return kApiKeyUrl;
}

OAuthConfig AnthropicProvider::oauthConfig() const {
    return {}; // empty clientId → supportsOAuth() == false
}

void AnthropicProvider::sendChat(
    const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError
) {
    QNetworkRequest netReq((QUrl(kMessagesUrl)));
    netReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    netReq.setRawHeader("anthropic-version", kApiVersion);
    netReq.setRawHeader("x-api-key", creds().apiKey.toUtf8());

    QJsonArray messages;
    for (const auto &m : req.messages) {
        messages.append(
            QJsonObject{
                {"role", m.role == Llm::Message::Role::User ? "user" : "assistant"},
                {"content", m.text},
            }
        );
    }
    QJsonObject body{
        {"model", req.model.isEmpty() ? kDefaultModel : req.model},
        {"max_tokens", req.maxTokens},
        {"messages", messages},
    };
    if (!req.system.isEmpty())
        body["system"] = req.system;

    auto *reply = nam()->post(netReq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [reply, onResponse = std::move(onResponse), onError = std::move(onError)] {
            reply->deleteLater();
            const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            if (obj.value("type").toString() == "error") {
                if (onError)
                    onError(obj.value("error").toObject().value("message").toString("unknown"));
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                if (onError)
                    onError(reply->errorString());
                return;
            }
            Llm::Response resp;
            resp.model      = obj.value("model").toString();
            resp.stopReason = obj.value("stop_reason").toString();
            // Safety classifiers can refuse with an empty content array —
            // surface that as an error rather than an empty completion.
            if (resp.stopReason == "refusal") {
                if (onError)
                    onError("refusal");
                return;
            }
            for (const auto &blockRef : obj.value("content").toArray()) {
                const QJsonObject block = blockRef.toObject();
                if (block.value("type").toString() == "text")
                    resp.text += block.value("text").toString();
            }
            if (onResponse)
                onResponse(std::move(resp));
        }
    );
}
