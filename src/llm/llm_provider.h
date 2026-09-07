// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// One configured AI endpoint: a wire format + base URL + API key + model.
// Callers never talk to a provider directly — they go through LlmService,
// which owns the registry and routes chat() to the default connected one.
//
// Two kinds of provider share this class:
//   • presets  — Anthropic and OpenAI. Fixed id/URL/wire; the user supplies a
//                key (BYOK) and may pick a model. Cannot be removed, only
//                disconnected. Their ids ("anthropic", "openai") are also the
//                credential keys, unchanged from the pre-registry layout.
//   • custom   — any OpenAI-compatible server (vLLM, Ollama, LM Studio,
//                LiteLLM, OpenRouter, …). User-named, user URL, key optional.
#pragma once

#include "llm_types.h"
#include "llm_wire.h"

#include <QObject>

struct LlmProviderConfig {
    QString         id;   // "anthropic" | "openai" | "custom-<8 hex>"
    QString         name; // display name (product name for presets — not translated)
    LlmWire::Format wire = LlmWire::Format::OpenAiChat;
    QString         baseUrl;
    QString         model;                       // empty on a preset → its default model
    QString         lightModel;                  // presets only; custom falls back to `model`
    QString         lightReasoningEffort;        // OpenAI preset: "none" for the light tier
    bool            maxCompletionTokens = false; // OpenAI preset only
    bool            isPreset            = false;
    QString         apiKeyUrl;    // vendor console page; empty for custom
    QString         defaultModel; // presets: what `model` falls back to
    QStringList     knownModels;  // presets: curated picks offered before "Fetch models"

    static LlmProviderConfig anthropicPreset();
    static LlmProviderConfig openAiPreset();
    // A fresh custom entry with a new id; caller fills name/baseUrl/model.
    static LlmProviderConfig newCustom();
    // The preset matching `id`, or an empty (isPreset=false) config.
    static LlmProviderConfig presetById(const QString &id);
};

class LlmProvider : public QObject {
    Q_OBJECT
public:
    explicit LlmProvider(LlmProviderConfig cfg, QObject *parent = nullptr);

    [[nodiscard]] const LlmProviderConfig &config() const { return _cfg; }
    [[nodiscard]] QString                  id() const { return _cfg.id; }
    [[nodiscard]] QString                  displayName() const { return _cfg.name; }
    [[nodiscard]] bool                     isPreset() const { return _cfg.isPreset; }
    [[nodiscard]] QString                  apiKeyUrl() const { return _cfg.apiKeyUrl; }
    [[nodiscard]] QString                  baseUrl() const { return _cfg.baseUrl; }

    // The model requests run on when Llm::Request::model is empty.
    [[nodiscard]] QString model() const;
    // Cheaper model for short, frequent tasks (summaries); == model() for custom.
    [[nodiscard]] QString lightModel() const;

    // A preset is connected once it has a key; a custom endpoint is connected
    // by existing (its key is optional).
    [[nodiscard]] bool    isConnected() const;
    [[nodiscard]] bool    hasApiKey() const { return !_apiKey.isEmpty(); }
    [[nodiscard]] QString apiKey() const { return _apiKey; }
    // Masked key ("sk-an…f3a9") for the UI; never the full key.
    [[nodiscard]] QString accountLabel() const;

    // Persisted to the secret store. Empty clears. Emits authStateChanged().
    void setApiKey(const QString &key);
    // Custom: rename / repoint. Preset: only `model` is taken (empty → default).
    // Not persisted here — LlmService owns config persistence.
    void applyConfig(const LlmProviderConfig &cfg);

    // One-shot completion. Exactly one of onResponse/onError fires, on the GUI
    // thread. Local models are slow: the transfer timeout is generous.
    void chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError);
    // GET /models — doubles as the connection/key test.
    void listModels(Llm::OnModels onModels, Llm::OnError onError);

    // listModels() for a not-yet-saved configuration (the settings editor's
    // "Test connection" / "Fetch models"). `ctx` scopes the callbacks: they
    // are dropped if it is destroyed first.
    static void probe(
        const LlmProviderConfig &cfg,
        const QString           &apiKey,
        Llm::OnModels            onModels,
        Llm::OnError             onError,
        QObject                 *ctx
    );

    [[nodiscard]] LlmWire::Endpoint endpoint(const QString &forModel = {}) const;

signals:
    void authStateChanged(); // key set/cleared
    void configChanged();    // name / URL / model changed

private:
    LlmProviderConfig _cfg;
    QString           _apiKey;
};
