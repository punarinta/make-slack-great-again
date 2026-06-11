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

    // Show or update the popup with its bottom edge just above anchor —
    // the global position of the '@' that triggered it. Re-anchors on every
    // call so the popup hugs the '@' as filtering changes the list height.
    // query : text typed after '@' (empty = show all).
    // isDm  : true suppresses @channel / @everyone / @here aliases.
    void open(const QPoint &anchorGlobalBottomLeft, const QString &query, bool isDm);
    void dismiss();
    bool isOpen() const { return isVisible(); }

    // Feed key events from the editor's event filter.  Returns true if consumed.
    bool handleKey(int key);

signals:
    // display    : human-readable label of the chosen row (e.g. "@Maria").
    // insertText : what the outgoing message must contain — the raw token
    //              "<@U…>" for users, or the alias itself for @here/@channel.
    void selected(const QString &display, const QString &insertText);

private:
    void rebuild(const QString &query, bool isDm);
    void selectRow(int idx);
    void confirm();
    void applyTheme();

    QScrollArea     *_scroll  = nullptr;
    QWidget         *_content = nullptr;
    QVBoxLayout     *_vbox    = nullptr;
    Session         *_session = nullptr;
    QList<QWidget *> _rows;
    QList<QString>   _displays;
    QList<QString>   _inserts;
    int              _sel = 0;
};
