// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_service.h"

#include "anthropic_provider.h"
#include "openai_provider.h"
#include "util/time_format.h"

#include <QSettings>

LlmService &LlmService::instance() {
    static LlmService service;
    return service;
}

LlmService::LlmService() {
    _providers << new AnthropicProvider(this) << new OpenAiProvider(this);
    for (auto *p : _providers)
        connect(p, &LlmProvider::authStateChanged, this, &LlmService::availabilityChanged);
}

LlmProvider *LlmService::provider(const QString &id) const {
    for (auto *p : _providers)
        if (p->id() == id)
            return p;
    return nullptr;
}

QString LlmService::defaultProviderId() const {
    return QSettings("msga", "msga").value("llm/defaultProvider").toString();
}

void LlmService::setDefaultProviderId(const QString &id) {
    QSettings("msga", "msga").setValue("llm/defaultProvider", id);
    emit availabilityChanged();
}

QString LlmService::nativeLanguage() const {
    const QString stored = QSettings("msga", "msga").value("llm/nativeLanguage").toString();
    if (!stored.isEmpty())
        return stored;
    // Never set → follow the UI language ("system" resolves to the OS locale).
    return QLocale::languageToCode(TimeFmt::locale().language());
}

void LlmService::setNativeLanguage(const QString &code) {
    QSettings("msga", "msga").setValue("llm/nativeLanguage", code);
}

LlmProvider *LlmService::activeProvider() const {
    auto *preferred = provider(defaultProviderId());
    if (preferred && preferred->authState() == LlmProvider::AuthState::Connected)
        return preferred;
    for (auto *p : _providers)
        if (p->authState() == LlmProvider::AuthState::Connected)
            return p;
    return nullptr;
}

void LlmService::chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError) {
    auto *p = activeProvider();
    if (!p) {
        if (onError)
            onError(tr("No AI provider connected — connect one in Settings → AI assistance"));
        return;
    }
    p->chat(req, std::move(onResponse), std::move(onError));
}
