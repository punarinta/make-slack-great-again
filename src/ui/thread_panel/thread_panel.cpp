// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "thread_panel.h"
#include "ui/message_list/message_list.h"
#include "ui/composer/composer_widget.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "session/session.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>

ThreadPanel::ThreadPanel(ImageCache *imgCache, QWidget *parent) : QWidget(parent) {
    setObjectName("threadPanel");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header bar
    _headerWidget = new QWidget(this);
    _headerWidget->setObjectName("threadHeader");
    _headerWidget->setFixedHeight(48);
    auto *headerLayout = new QHBoxLayout(_headerWidget);
    headerLayout->setContentsMargins(16, 0, 8, 0);
    headerLayout->setSpacing(8);

    _header = new QLabel(tr("Thread"), _headerWidget);
    headerLayout->addWidget(_header, 1);

    _closeBtn = new QPushButton("✕", _headerWidget);
    _closeBtn->setFixedSize(32, 32);
    _closeBtn->setFlat(true);
    _closeBtn->setCursor(Qt::PointingHandCursor);
    connect(_closeBtn, &QPushButton::clicked, this, &ThreadPanel::closeRequested);
    headerLayout->addWidget(_closeBtn);
    layout->addWidget(_headerWidget);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });

    _msgList = new MessageListWidget(nullptr, imgCache, this);
    layout->addWidget(_msgList, 1);
    connect(_msgList, &MessageListWidget::openDmRequested, this, &ThreadPanel::openDmRequested);

    _composer = new ComposerWidget(this);
    _composer->setEnabled(false);
    layout->addWidget(_composer);

    connect(_composer, &ComposerWidget::sendRequested, this, [this](const QString &text) {
        if (_session && !_conv.value.isEmpty() && !_rootTs.isEmpty())
            _session->sendMessage(_conv, text, _rootTs);
    });
    connect(
        _composer,
        &ComposerWidget::editRequested,
        this,
        [this](const Ts &ts, const QString &newText) {
            if (_session && !_conv.value.isEmpty())
                _session->editMessage(_conv, ts, newText);
        }
    );
    connect(_composer, &ComposerWidget::editLastRequested, this, [this] {
        if (!_session || !_msgList)
            return;
        const auto msg = _msgList->lastOwnMessage(_session->meUserId());
        if (!msg)
            return;
        const QString text = msg->rawText.isEmpty() ? msg->text.text : msg->rawText;
        _composer->enterEditMode(msg->ts, text, msg->files);
    });
    connect(_composer, &ComposerWidget::typingStarted, this, [this] {
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

void ThreadPanel::applyTheme() {
    setStyleSheet(QString("QWidget#threadPanel { border-left: 1px solid %1; background: %2; }")
                      .arg(Th::qss(Th::c().divider.def), Th::qss(Th::c().surface.raised)));
    _headerWidget->setStyleSheet(
        QString(
            "QWidget#threadHeader {"
            "  background: %1;"
            "  border-bottom: 1px solid %2;"
            "}"
        )
            .arg(Th::qss(Th::c().surface.sunken), Th::qss(Th::c().divider.def))
    );
    _header->setStyleSheet(QString("font-weight: bold; font-size: %1px; color: %2;")
                               .arg(Th::c().fonts.lg)
                               .arg(Th::qss(Th::c().text.primary)));
    _closeBtn->setStyleSheet(QString(
                                 "QPushButton { font-size: %1px; border-radius: 4px; }"
                                 "QPushButton:hover { background: %2; }"
    )
                                 .arg(Th::c().fonts.base)
                                 .arg(Th::qss(Th::c().divider.def)));
}
