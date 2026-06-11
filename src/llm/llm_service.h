// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// App-wide LLM facade. Callers use chat() and never learn which vendor served
// the request; the settings UI manages connections and the default provider.
//
//   LlmService::instance().chat(request,
//       [](Llm::Response r) { … },
//       [](QString err)    { … });
#pragma once

#include <QObject>

#include "llm_provider.h"

class LlmService : public QObject {
    Q_OBJECT
public:
    static LlmService &instance();

    // All registered providers, connected or not (for the settings UI).
    [[nodiscard]] QList<LlmProvider *> providers() const { return _providers; }
    [[nodiscard]] LlmProvider         *provider(const QString &id) const;

    // The user's preferred provider. Persisted in QSettings.
    [[nodiscard]] QString defaultProviderId() const;
    void                  setDefaultProviderId(const QString &id);

    // The provider chat() routes to: the default one if connected, otherwise
    // any connected provider, otherwise nullptr.
    [[nodiscard]] LlmProvider *activeProvider() const;
    [[nodiscard]] bool         isAvailable() const { return activeProvider() != nullptr; }

    // Routes to activeProvider(). Calls onError immediately if none connected.
    void chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError);

signals:
    // Connection state or default-provider selection changed.
    void availabilityChanged();

private:
    LlmService();

    QList<LlmProvider *> _providers;
};
