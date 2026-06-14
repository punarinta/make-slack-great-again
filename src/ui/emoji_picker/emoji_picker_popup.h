// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFrame>
#include <QString>
#include <QVector>

class QLineEdit;
class QScrollArea;
class QToolButton;
class QWidget;
class Session;

// Floating emoji picker popup — reusable across the whole UI.
// Mount it once (parent = any widget) and call open() whenever you need it.
// Auto-dismisses on outside click (Qt::Popup window flag).
//
// Usage:
//   auto *picker = new EmojiPickerPopup(this);
//   connect(picker, &EmojiPickerPopup::emojiSelected,
//           this, [](const QString &name) { /* insert :name: */ });
//   picker->open(globalPos);
class EmojiPickerPopup : public QFrame {
    Q_OBJECT
public:
    explicit EmojiPickerPopup(QWidget *parent = nullptr);

    // Provide custom emoji from a Session (name → URL or "alias:name").
    // Call whenever a new session becomes available; pass nullptr to clear.
    void setSession(Session *session);

    // Show the picker at globalPos and focus the search field.
    void open(const QPoint &globalPos);

signals:
    // Emitted when the user clicks an emoji. name is the short code without colons,
    // e.g. "thumbsup". Callers wrap it as ":name:" if needed.
    void emojiSelected(const QString &name);

protected:
    // Routes arrow/enter/escape keys from the search field into grid navigation.
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void buildGrid(const QString &filter = {});
    void applyTheme();
    void setSelected(int idx);     // highlight + scroll the idx-th button into view
    void moveSelection(int delta); // move selection by delta buttons (clamped)

    QLineEdit             *_search  = nullptr;
    QScrollArea           *_scroll  = nullptr;
    QWidget               *_grid    = nullptr;
    Session               *_session = nullptr;
    QVector<QToolButton *> _btns;         // buttons in render order (rebuilt per filter)
    int                    _sel = -1;     // index into _btns of the keyboard selection
    QString                _btnBaseStyle; // unselected button stylesheet
    QString                _btnSelStyle;  // selected button stylesheet
};
