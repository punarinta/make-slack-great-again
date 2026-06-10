// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "delete_message_dialog.h"
#include "ui/message_list/message_render.h"
#include "ui/theme.h"
#include "session/session.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

DeleteMessageDialog::DeleteMessageDialog(const Message &msg, Session *session, QWidget *parent)
    : AppDialog(tr("Delete message"), parent) {
    auto *cl = contentLayout();

    _warnLabel = new QLabel(tr("This action cannot be undone."));
    cl->addWidget(_warnLabel);

    // ── Message preview card ───────────────────────────────────────────
    _msgCard = new QFrame;
    _msgCard->setObjectName("msgCard");
    auto *cardLay = new QVBoxLayout(_msgCard);
    cardLay->setContentsMargins(12, 10, 12, 10);
    cardLay->setSpacing(4);

    if (session) {
        const auto   *user = session->findUser(msg.author);
        const QString name = user ? user->displayName : msg.author.value;

        auto *headerRow = new QHBoxLayout;
        headerRow->setSpacing(8);

        auto *nameLabel = new QLabel(name.toHtmlEscaped(), _msgCard);
        QFont nameFnt   = nameLabel->font();
        nameFnt.setBold(true);
        nameLabel->setFont(nameFnt);

        _tsLabel = new QLabel(MsgRender::formatTs(msg.ts), _msgCard);

        headerRow->addWidget(nameLabel);
        headerRow->addWidget(_tsLabel);
        headerRow->addStretch();
        cardLay->addLayout(headerRow);
    }

    auto *preview = new QTextBrowser(_msgCard);
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
    cl->addWidget(_msgCard);

    // ── Buttons ────────────────────────────────────────────────────────
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addStretch();

    _cancelBtn = new QPushButton(tr("Cancel"));
    _deleteBtn = new QPushButton(tr("Delete"));

    btnRow->addWidget(_cancelBtn);
    btnRow->addWidget(_deleteBtn);
    cl->addLayout(btnRow);

    connect(_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(_deleteBtn, &QPushButton::clicked, this, &QDialog::accept);

    applyTheme();
    updateCard();
}

void DeleteMessageDialog::applyTheme() {
    AppDialog::applyTheme();
    _warnLabel->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.secondary)));
    _msgCard->setStyleSheet(
        QString(
            "QFrame#msgCard {"
            "  border: 1px solid %1;"
            "  border-radius: 6px;"
            "  background: %2;"
            "}"
        )
            .arg(Th::qss(Th::c().surface.highlightStrong), Th::qss(Th::c().message.fileChipBg))
    );
    if (_tsLabel)
        _tsLabel->setStyleSheet(QString("color: %1; font-size: %2px;")
                                    .arg(Th::qss(Th::c().text.secondary))
                                    .arg(Th::c().fonts.sm));
    _cancelBtn->setStyleSheet(QString(
                                  "QPushButton { border: 1px solid %1; border-radius: 4px;"
                                  " padding: 6px 18px; background: %2; }"
                                  "QPushButton:hover { background: %3; }"
    )
                                  .arg(
                                      Th::qss(Th::c().divider.strong),
                                      Th::qss(Th::c().surface.raised),
                                      Th::qss(Th::c().surface.sunken)
                                  ));
    _deleteBtn->setStyleSheet(QString(
                                  "QPushButton { background: %1; color: %2; border: none;"
                                  " border-radius: 4px; padding: 6px 18px; font-weight: bold; }"
                                  "QPushButton:hover { background: %3; }"
    )
                                  .arg(
                                      Th::qss(Th::c().danger.def),
                                      Th::qss(Th::c().accent.text),
                                      Th::qss(Th::c().danger.hover)
                                  ));
}
