// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/app_dialog/app_dialog.h"

#include <vector>

class BrowseListView;
class ImageCache;
class QCheckBox;
class QLabel;
class StyledButton;
class StyledLineEdit;

// One thread the user can move a message into — a root message reduced to
// what the picker shows. Built by the host (MainWindow) from the channel's
// loaded roots, with names already resolved, so the dialog needs no Session.
struct ThreadChoice {
    Ts      root;
    QString text;           // root's plain text (or a file name when it has none)
    QString author;         // display name of the root's author
    QString avatarUrl;      // author avatar; empty → initial disc
    qint64  date       = 0; // root's date in epoch microseconds
    int     replyCount = 0;
};

// "Move to thread" picker: the threads of the message's own channel (newest
// first), a filter field over their text and author, and a Move button.
// Clicking a row only selects it — the move deletes the original message, so a
// bare click must not fire it; Move or Enter confirms the highlighted row.
// Accepted → selectedRoot().
class MoveToThreadDialog : public AppDialog {
    Q_OBJECT
public:
    MoveToThreadDialog(
        std::vector<ThreadChoice> threads, ImageCache *imgCache, QWidget *parent = nullptr
    );

    // Root ts of the highlighted thread; empty when nothing is selected.
    Ts   selectedRoot() const;
    // Whether the copy should open with a note naming the original author and
    // time (Session::movedMessageText). Off by default: the mover usually wants
    // the message as it was, not a banner about the move.
    bool addNote() const;

protected:
    void applyTheme() override;
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void buildItems();
    void applyFilter(const QString &query);
    void syncMoveButton();

    StyledLineEdit *_searchEdit = nullptr;
    BrowseListView *_list       = nullptr;
    QLabel         *_note       = nullptr;
    QLabel         *_empty      = nullptr;
    QCheckBox      *_noteBox    = nullptr;
    StyledButton   *_cancelBtn  = nullptr;
    StyledButton   *_moveBtn    = nullptr;

    std::vector<ThreadChoice> _threads;
    ImageCache               *_imgCache = nullptr;

    static constexpr int kListMinH = 280;
};
