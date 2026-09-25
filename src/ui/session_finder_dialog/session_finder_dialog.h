// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/app_dialog/app_dialog.h"

#include <algorithm>
#include <vector>

class BrowseListView;
class IconButton;
class ImageCache;
class QLabel;
class QStackedWidget;
class StyledButton;
class StyledLineEdit;

// "Find a session" (an agent workspace's "Find a channel"): every session the
// agent has — the ones in the list and the rest, from every folder, like
// Claude Code's /resume — searchable by title, folder and prompts. The list
// arrives later (setSessions); picking one emits sessionActivated.
class SessionFinderDialog : public AppDialog {
    Q_OBJECT
public:
    explicit SessionFinderDialog(ImageCache *imgCache, QWidget *parent = nullptr);

    void setSessions(std::vector<FoundSession> sessions);

signals:
    void sessionActivated(QString id);
    void createSessionRequested();

protected:
    void applyTheme() override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    int  cardWidth(int availOverlayWidth) const override {
        return std::min(availOverlayWidth, kCardW);
    }
    int minCardHeight() const override { return kCardMinH; }

private:
    void applyFilter(const QString &query);

    StyledLineEdit *_searchEdit = nullptr;
    StyledButton   *_createBtn  = nullptr;
    IconButton     *_closeBtn   = nullptr;
    QStackedWidget *_stack      = nullptr;
    QLabel         *_status     = nullptr; // "Looking for sessions…" / no match
    BrowseListView *_list       = nullptr;
    QWidget        *_divider    = nullptr;
    bool            _loaded     = false;

    static constexpr int kCardW    = 720;
    static constexpr int kCardMinH = 520;
    static constexpr int kListMinH = 400;
};
