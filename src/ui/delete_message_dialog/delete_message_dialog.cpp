// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "delete_message_dialog.h"
#include "ui/message_list/message_render.h"
#include "ui/styled_button/styled_button.h"
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
    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;

    _warnLabel = new QLabel(tr("This action cannot be undone."));
    cl->addWidget(_warnLabel);

    // ── Message preview card ───────────────────────────────────────────
    _msgCard = new QFrame;
    _msgCard->setObjectName("msgCard");
    auto *cardLay = new QVBoxLayout(_msgCard);
    // No horizontal padding on the card: the preview is full-bleed so its
    // scrollbar hugs the right edge. Children that should be inset (the header)
    // carry their own sp.lg margins; the preview pads its text via the root
    // frame so left aligns with the header while the right reaches the edge.
    cardLay->setContentsMargins(0, sp.md, 0, sp.md);
    cardLay->setSpacing(sp.sm);

    if (session) {
        const auto   *user = session->findUser(msg.author);
        const QString name = user ? user->displayName : session->userDisplayName(msg.author);

        auto *headerRow = new QHBoxLayout;
        headerRow->setContentsMargins(sp.lg, 0, sp.lg, 0);
        headerRow->setSpacing(sp.md);

        auto *nameLabel = new QLabel(name.toHtmlEscaped(), _msgCard);
        QFont nameFnt   = nameLabel->font();
        nameFnt.setBold(true);
        nameLabel->setFont(nameFnt);

        _tsLabel = new QLabel(MsgRender::formatTs(msg.date), _msgCard);

        headerRow->addWidget(nameLabel);
        headerRow->addWidget(_tsLabel);
        headerRow->addStretch();
        cardLay->addLayout(headerRow);
    }

    auto *preview = new QTextBrowser(_msgCard);
    preview->setMaximumHeight(160);

    const QString html = MsgRender::buildMsgHtml(msg, session);
    if (html.trimmed().isEmpty())
        preview->setPlainText(msg.rawText);
    else
        preview->setHtml(html);

    // Shared preview chrome: thin scrollbar, full-bleed right edge, left text
    // padding that lines up with the header. Applied after content is set.
    MsgRender::configurePreviewBrowser(preview);
    cardLay->addWidget(preview);
    cl->addWidget(_msgCard);

    // ── Buttons ────────────────────────────────────────────────────────
    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _deleteBtn = new StyledButton(tr("Delete"), StyledButton::Variant::Danger);
    addButtonRow(_deleteBtn, _cancelBtn); // Cancel → reject() wired by base

    connect(_deleteBtn, &QPushButton::clicked, this, &AppDialog::accept);

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
    // Cancel/Delete buttons self-theme (StyledButton).
}
