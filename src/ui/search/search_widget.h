// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Search bar + results panel. Shown when the user clicks the search icon.
#pragma once

#include "backend/domain.h"
#include <QWidget>
#include <vector>

class QLineEdit;
class QListWidget;
class Session;

class SearchWidget : public QWidget {
    Q_OBJECT
public:
    explicit SearchWidget(QWidget *parent = nullptr);

    void setSession(Session *session);

signals:
    // Emitted when user clicks a result; caller opens the conversation.
    void resultSelected(ConversationId conv, Ts ts);
    void closeRequested();

private:
    void runSearch(const QString &query);
    void populateResults(const std::vector<SearchResult> &results);

    Session     *_session    = nullptr;
    QLineEdit   *_queryEdit  = nullptr;
    QListWidget *_resultList = nullptr;

    std::vector<SearchResult> _results;
};
