// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFrame>
#include <QString>
#include <functional>

class QVBoxLayout;
class Session;

// Floating autocomplete list for @user, #channel, and :emoji: triggers.
// Appears above the trigger word in the editor; auto-dismisses on Escape or
// when the editor loses focus. A plain child widget of the conversation panel
// (NOT a window) so it never takes focus and positions reliably on Wayland.
class MentionCompleter : public QFrame {
    Q_OBJECT
public:
    explicit MentionCompleter(QWidget *parent = nullptr);

    enum class Mode { User, Channel, Emoji };

    using Callback = std::function<void(const QString &insertText)>;

    // Show the completer with its bottom edge just above globalPos (pass the
    // global top-left of the trigger character so the popup hugs the word).
    // Each item: display string, and the text to insert on confirmation.
    struct Item {
        QString display;
        QString insert;
    };
    void show(const QPoint &globalPos, const QList<Item> &items, Callback cb);
    void dismiss();

    // Keyboard navigation — call from editor's eventFilter.
    // Returns true if the event was consumed.
    bool handleKey(int key);

    bool isVisible() const;

private:
    void applyTheme();
    void rebuild(const QList<Item> &items);
    void selectRow(int row);
    void confirm();

    QVBoxLayout     *_layout = nullptr;
    QList<Item>      _items;
    int              _sel = 0;
    Callback         _cb;
    QList<QWidget *> _rows;
};
