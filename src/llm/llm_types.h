// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Provider-neutral request/response types for the LLM service.
#pragma once

#include <QString>
#include <QList>
#include <functional>

namespace Llm {

struct Message {
    enum class Role { User, Assistant };
    Role    role = Role::User;
    QString text;
};

struct Request {
    QString        system;   // optional system prompt
    QList<Message> messages; // alternating user/assistant, first must be user
    QString        model;    // empty → provider default
    int            maxTokens = 4096;
};

struct Response {
    QString text;
    QString model;      // model that actually served the request
    QString stopReason; // provider-specific ("end_turn", "stop", "refusal", …)
};

using OnResponse = std::function<void(Response)>;
using OnError    = std::function<void(QString)>;

} // namespace Llm
