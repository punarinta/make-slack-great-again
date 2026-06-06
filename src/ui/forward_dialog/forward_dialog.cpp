// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "forward_dialog.h"
#include "ui/conv_selector/conv_selector_widget.h"
#include "ui/composer/composer_widget.h"
#include "ui/message_list/message_render.h"
#include "session/session.h"

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

ForwardDialog::ForwardDialog(const Message &msg, Session *session, QWidget *parent)
    : AppDialog(tr("Forward this message"), parent)
{
    auto *cl = contentLayout();

    // ── Conversation selector ──────────────────────────────────────────
    _selector = new ConvSelectorWidget(session);
    cl->addWidget(_selector);

    // ── Optional comment via composer ─────────────────────────────────
    _composer = new ComposerWidget;
    _composer->setPlaceholderText(tr("Add a message, if you'd like."));
    if (session) _composer->setSession(session);
    _composer->setMaximumHeight(120);
    if (auto *lay = _composer->layout())
        lay->setContentsMargins(0, lay->contentsMargins().top(),
                                0, lay->contentsMargins().bottom());
    cl->addWidget(_composer);

    // ── Message preview card ───────────────────────────────────────────
    auto *previewCard = new QFrame;
    previewCard->setObjectName("fwdCard");
    previewCard->setStyleSheet(
        "QFrame#fwdCard {"
        "  border: 1px solid #E0E0E0;"
        "  border-radius: 6px;"
        "  background: #FAFAFA;"
        "}"
    );
    auto *cardLay = new QVBoxLayout(previewCard);
    cardLay->setContentsMargins(12, 8, 12, 8);
    cardLay->setSpacing(4);

    if (session) {
        const auto *user = session->findUser(msg.author);
        const QString name = user ? user->displayName : msg.author.value;
        auto *nameLabel = new QLabel("<b>" + name.toHtmlEscaped() + "</b>"
                                     + "  <span style='color:#888;font-size:11px'>"
                                     + MsgRender::formatTs(msg.ts) + "</span>",
                                     previewCard);
        nameLabel->setTextFormat(Qt::RichText);
        cardLay->addWidget(nameLabel);
    }

    auto *preview = new QTextBrowser(previewCard);
    preview->setReadOnly(true);
    preview->setMaximumHeight(140);
    preview->setFrameShape(QFrame::NoFrame);
    preview->setStyleSheet("QTextBrowser { background: transparent; }");
    preview->setOpenLinks(false);

    const QString html = MsgRender::buildMsgHtml(msg, session);
    if (html.trimmed().isEmpty())
        preview->setPlainText(msg.rawText);
    else
        preview->setHtml(html);

    cardLay->addWidget(preview);
    cl->addWidget(previewCard);

    // ── Button bar ─────────────────────────────────────────────────────
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);

    auto *copyLinkBtn = new QPushButton(tr("Copy Link"));
    copyLinkBtn->setStyleSheet(
        "QPushButton { border: 1px solid #CCC; border-radius: 4px;"
        " padding: 6px 14px; background: white; }"
        "QPushButton:hover { background: #F5F5F5; }"
    );

    btnRow->addWidget(copyLinkBtn);
    btnRow->addStretch();

    auto *cancelBtn = new QPushButton(tr("Cancel"));
    cancelBtn->setStyleSheet(
        "QPushButton { border: 1px solid #CCC; border-radius: 4px;"
        " padding: 6px 18px; background: white; }"
        "QPushButton:hover { background: #F5F5F5; }"
    );

    _fwdBtn = new QPushButton(tr("Forward"));
    _fwdBtn->setEnabled(false);
    _fwdBtn->setStyleSheet(
        "QPushButton { background: #007A5A; color: white; border: none;"
        " border-radius: 4px; padding: 6px 18px; font-weight: bold; }"
        "QPushButton:hover { background: #006348; }"
        "QPushButton:disabled { background: #CCCCCC; }"
    );

    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(_fwdBtn);
    cl->addLayout(btnRow);

    connect(_selector, &ConvSelectorWidget::convSelected,
            this, [this](const ConversationId &id, const QString &) {
        _fwdBtn->setEnabled(!id.value.isEmpty());
    });

    connect(_composer, &ComposerWidget::sendRequested,
            this, [this](const QString &) {
        if (_fwdBtn->isEnabled()) accept();
    });

    connect(cancelBtn,    &QPushButton::clicked, this, &QDialog::reject);
    connect(_fwdBtn,      &QPushButton::clicked, this, &QDialog::accept);

    connect(copyLinkBtn, &QPushButton::clicked, this, [&msg] {
        for (const auto &ent : msg.text.entities) {
            if (ent.type == EntityType::Link && !ent.data.isEmpty()) {
                QApplication::clipboard()->setText(ent.data);
                return;
            }
        }
    });

    updateCard();
}

ConversationId ForwardDialog::targetConv() const {
    return _selector->selectedConv();
}

QString ForwardDialog::comment() const {
    return _composer->currentText().trimmed();
}
