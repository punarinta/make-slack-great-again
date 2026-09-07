// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// App-wide LLM facade and provider registry. Callers use chat() and never
// learn which endpoint served the request; the settings UI manages the
// registry (presets + user-added OpenAI-compatible servers) and the default.
//
//   LlmService::instance().chat(request,
//       [](Llm::Response r) { … },
//       [](QString err)    { … });
//
// Persistence (QSettings "msga"): llm/defaultProvider, llm/customProviders
// (ordered id list), llm/providers/<id>/{name,baseUrl,model}. Keys live in the
// secret store via LlmTokenStore.
#pragma once

#include <QObject>

#include "llm_provider.h"

class LlmService : public QObject {
    Q_OBJECT
public:
    static LlmService &instance();

    // Presets first (Anthropic, OpenAI), then custom entries in creation order.
    [[nodiscard]] QList<LlmProvider *> providers() const { return _providers; }
    [[nodiscard]] LlmProvider         *provider(const QString &id) const;

    // Custom (OpenAI-compatible) endpoints. `cfg` from LlmProviderConfig::newCustom()
    // with name/baseUrl/model filled; the key may be empty.
    LlmProvider *addCustom(const LlmProviderConfig &cfg, const QString &apiKey);
    void         removeCustom(const QString &id);
    // Persists `cfg` (custom: name/baseUrl/model; preset: model only) and the
    // key. An empty key on a custom provider keeps the stored one; on a preset
    // it is ignored (use disconnectProvider()).
    void updateProvider(const QString &id, const LlmProviderConfig &cfg, const QString &apiKey);
    // Preset: forget the key. Custom: same as removeCustom().
    void disconnectProvider(const QString &id);

    // The user's preferred provider. Persisted in QSettings.
    [[nodiscard]] QString defaultProviderId() const;
    void                  setDefaultProviderId(const QString &id);

    // The user's native language (ISO 639-1 code, e.g. "en") — the language AI
    // features should address the user in. Follows the app UI language until
    // the user explicitly picks one in Settings; only then is it persisted.
    [[nodiscard]] QString nativeLanguage() const;
    void                  setNativeLanguage(const QString &code);

    // The provider chat() routes to: the default one if connected, otherwise
    // any connected provider, otherwise nullptr.
    [[nodiscard]] LlmProvider *activeProvider() const;
    [[nodiscard]] bool         isAvailable() const { return activeProvider() != nullptr; }

    // Routes to activeProvider(). Calls onError immediately if none connected.
    void chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError);

signals:
    // Connection state, default selection, or registry membership changed.
    void availabilityChanged();
    // A provider was added, removed, renamed or repointed.
    void providersChanged();

private:
    LlmService();

    LlmProvider *registerProvider(const LlmProviderConfig &cfg);
    void         persistConfig(const LlmProvider *p) const;

    QList<LlmProvider *> _providers;
};
