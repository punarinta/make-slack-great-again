// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFrame>

class Session;
class QScrollArea;
class QVBoxLayout;

// Floating @-mention autocomplete popup.
// Does NOT steal focus from the editor — feed keyboard events via handleKey().
class MentionPopup : public QFrame {
    Q_OBJECT
public:
    explicit MentionPopup(QWidget *parent = nullptr);

    void setSession(Session *session);

    // Show or update the popup above cursorGlobalBottomLeft.
    // query : text typed after '@' (empty = show all).
    // isDm  : true suppresses @channel / @everyone / @here aliases.
    void open(const QPoint &cursorGlobalBottomLeft, const QString &query, bool isDm);
    void dismiss();
    bool isOpen() const { return isVisible(); }

    // Feed key events from the editor's event filter.  Returns true if consumed.
    bool handleKey(int key);

signals:
    void selected(const QString &insertText);

private:
    void rebuild(const QString &query, bool isDm);
    void selectRow(int idx);
    void confirm();

    QScrollArea     *_scroll  = nullptr;
    QWidget         *_content = nullptr;
    QVBoxLayout     *_vbox    = nullptr;
    Session         *_session = nullptr;
    QList<QWidget *> _rows;
    QList<QString>   _inserts;
    int              _sel = 0;
};
