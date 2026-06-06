// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "thread_panel.h"
#include "ui/message_list/message_list.h"
#include "ui/composer/composer_widget.h"
#include "session/session.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>

ThreadPanel::ThreadPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("threadPanel");
    setStyleSheet(
        "QWidget#threadPanel { border-left: 1px solid #E8E8E8; background: #FFFFFF; }");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header bar
    auto *header = new QWidget(this);
    header->setObjectName("threadHeader");
    header->setFixedHeight(48);
    header->setStyleSheet(
        "QWidget#threadHeader {"
        "  background: #F5F5F5;"
        "  border-bottom: 1px solid #E8E8E8;"
        "}");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 0, 8, 0);
    headerLayout->setSpacing(8);

    _header = new QLabel(tr("Thread"), header);
    _header->setStyleSheet("font-weight: bold; font-size: 15px; color: #1D1C1D;");
    headerLayout->addWidget(_header, 1);

    auto *closeBtn = new QPushButton("✕", header);
    closeBtn->setFixedSize(32, 32);
    closeBtn->setFlat(true);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(
        "QPushButton { font-size: 14px; border-radius: 4px; }"
        "QPushButton:hover { background: #E8E8E8; }");
    connect(closeBtn, &QPushButton::clicked, this, &ThreadPanel::closeRequested);
    headerLayout->addWidget(closeBtn);
    layout->addWidget(header);

    _msgList = new MessageListWidget(nullptr, nullptr, this);
    layout->addWidget(_msgList, 1);

    _composer = new ComposerWidget(this);
    _composer->setEnabled(false);
    layout->addWidget(_composer);

    connect(_composer, &ComposerWidget::sendRequested,
            this, [this](const QString &text) {
        if (_session && !_conv.value.isEmpty() && !_rootTs.isEmpty())
            _session->sendMessage(_conv, text, _rootTs);
    });
    connect(_composer, &ComposerWidget::editRequested,
            this, [this](const Ts &ts, const QString &newText) {
        if (_session && !_conv.value.isEmpty())
            _session->editMessage(_conv, ts, newText);
    });
    connect(_composer, &ComposerWidget::editLastRequested,
            this, [this] {
        if (!_session || !_msgList) return;
        const auto msg = _msgList->lastOwnMessage(_session->meUserId());
        if (!msg) return;
        const QString text = msg->rawText.isEmpty() ? msg->text.text : msg->rawText;
        _composer->enterEditMode(msg->ts, text, msg->files);
    });
    connect(_composer, &ComposerWidget::typingStarted,
            this, [this] {
        if (_session && !_conv.value.isEmpty())
            _session->sendTyping(_conv);
    });
}

void ThreadPanel::setSession(Session *session) {
    _session = session;
    _msgList->setSession(session);
    _composer->setSession(session);
}

void ThreadPanel::openThread(ConversationId conv, Ts rootTs) {
    _conv   = conv;
    _rootTs = rootTs;
    _msgList->openThread(conv, rootTs);
    _composer->setEnabled(true);
    _composer->setPlaceholderText(tr("Reply in thread…"));
}

void ThreadPanel::close() {
    _conv   = {};
    _rootTs = {};
    _msgList->clear();
    _composer->setEnabled(false);
}
