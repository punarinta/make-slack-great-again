// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

class QFrame;
class QTextEdit;
class QPushButton;
class QToolButton;
class QWidget;

// Slack-style composer: formatting toolbar + text area + bottom action bar.
// Enter sends; Shift+Enter inserts a newline.
class ComposerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ComposerWidget(QWidget *parent = nullptr);

    void setPlaceholderText(const QString &text);

signals:
    void sendRequested(const QString &text);
    void uploadRequested(const QString &filePath);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void trySend();
    void updateSendState();
    void adjustEditorHeight();
    void setFocused(bool focused);
    void applyInlineFormat(const QString &marker);

    QFrame      *_box     = nullptr;
    QWidget     *_toolbar = nullptr;
    QTextEdit   *_edit    = nullptr;
    QPushButton *_sendBtn = nullptr;
    QPushButton *_dropBtn = nullptr;
};
