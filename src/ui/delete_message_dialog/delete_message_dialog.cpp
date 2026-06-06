// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "delete_message_dialog.h"
#include "ui/message_list/message_render.h"
#include "session/session.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

DeleteMessageDialog::DeleteMessageDialog(const Message &msg,
                                         Session *session,
                                         QWidget *parent)
    : AppDialog(tr("Delete message"), parent)
{
    auto *cl = contentLayout();

    auto *warnLabel = new QLabel(tr("This action cannot be undone."));
    warnLabel->setStyleSheet("color: #666;");
    cl->addWidget(warnLabel);

    // ── Message preview card ───────────────────────────────────────────
    auto *card = new QFrame;
    card->setObjectName("msgCard");
    card->setStyleSheet(
        "QFrame#msgCard {"
        "  border: 1px solid #E0E0E0;"
        "  border-radius: 6px;"
        "  background: #FAFAFA;"
        "}"
    );
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(12, 10, 12, 10);
    cardLay->setSpacing(4);

    if (session) {
        const auto *user = session->findUser(msg.author);
        const QString name = user ? user->displayName : msg.author.value;

        auto *headerRow = new QHBoxLayout;
        headerRow->setSpacing(8);

        auto *nameLabel = new QLabel(name.toHtmlEscaped(), card);
        QFont nameFnt = nameLabel->font();
        nameFnt.setBold(true);
        nameLabel->setFont(nameFnt);

        auto *tsLabel = new QLabel(MsgRender::formatTs(msg.ts), card);
        tsLabel->setStyleSheet("color: #888; font-size: 11px;");

        headerRow->addWidget(nameLabel);
        headerRow->addWidget(tsLabel);
        headerRow->addStretch();
        cardLay->addLayout(headerRow);
    }

    auto *preview = new QTextBrowser(card);
    preview->setReadOnly(true);
    preview->setMaximumHeight(160);
    preview->setFrameShape(QFrame::NoFrame);
    preview->setStyleSheet("QTextBrowser { background: transparent; }");
    preview->setOpenLinks(false);

    const QString html = MsgRender::buildMsgHtml(msg, session);
    if (html.trimmed().isEmpty())
        preview->setPlainText(msg.rawText);
    else
        preview->setHtml(html);

    cardLay->addWidget(preview);
    cl->addWidget(card);

    // ── Buttons ────────────────────────────────────────────────────────
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addStretch();

    auto *cancelBtn = new QPushButton(tr("Cancel"));
    cancelBtn->setStyleSheet(
        "QPushButton { border: 1px solid #CCC; border-radius: 4px;"
        " padding: 6px 18px; background: white; }"
        "QPushButton:hover { background: #F5F5F5; }"
    );

    auto *deleteBtn = new QPushButton(tr("Delete"));
    deleteBtn->setStyleSheet(
        "QPushButton { background: #E01E5A; color: white; border: none;"
        " border-radius: 4px; padding: 6px 18px; font-weight: bold; }"
        "QPushButton:hover { background: #C0184F; }"
    );

    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(deleteBtn);
    cl->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(deleteBtn, &QPushButton::clicked, this, &QDialog::accept);

    updateCard();
}
