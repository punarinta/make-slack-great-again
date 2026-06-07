// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "forward_dialog.h"
#include "ui/conv_selector/conv_selector_widget.h"
#include "ui/composer/composer_widget.h"
#include "ui/message_list/message_render.h"
#include "ui/theme.h"
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
    : AppDialog(tr("Forward this message"), parent) {
    auto *cl = contentLayout();

    // ── Conversation selector ──────────────────────────────────────────
    _selector = new ConvSelectorWidget(session);
    cl->addWidget(_selector);

    // ── Optional comment via composer ─────────────────────────────────
    _composer = new ComposerWidget;
    _composer->setPlaceholderText(tr("Add a message, if you'd like."));
    if (session)
        _composer->setSession(session);
    _composer->setMaximumHeight(120);
    if (auto *lay = _composer->layout())
        lay->setContentsMargins(
            0, lay->contentsMargins().top(), 0, lay->contentsMargins().bottom()
        );
    cl->addWidget(_composer);

    // ── Message preview card ───────────────────────────────────────────
    _previewCard = new QFrame;
    _previewCard->setObjectName("fwdCard");
    auto *cardLay = new QVBoxLayout(_previewCard);
    cardLay->setContentsMargins(12, 8, 12, 8);
    cardLay->setSpacing(4);

    if (session) {
        const auto   *user      = session->findUser(msg.author);
        const QString name      = user ? user->displayName : msg.author.value;
        auto         *nameLabel = new QLabel(
            "<b>" + name.toHtmlEscaped() + "</b>" + "  <span style='color:" +
                Th::qss(Th::c().text.tertiary) + ";font-size:" + QString::number(Th::c().fonts.sm) +
                "px'>" + MsgRender::formatTs(msg.ts) + "</span>",
            _previewCard
        );
        nameLabel->setTextFormat(Qt::RichText);
        cardLay->addWidget(nameLabel);
    }

    auto *preview = new QTextBrowser(_previewCard);
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
    cl->addWidget(_previewCard);

    // ── Button bar ─────────────────────────────────────────────────────
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);

    _copyLinkBtn = new QPushButton(tr("Copy Link"));
    btnRow->addWidget(_copyLinkBtn);
    btnRow->addStretch();

    _cancelBtn = new QPushButton(tr("Cancel"));

    _fwdBtn = new QPushButton(tr("Forward"));
    _fwdBtn->setEnabled(false);

    btnRow->addWidget(_cancelBtn);
    btnRow->addWidget(_fwdBtn);
    cl->addLayout(btnRow);

    connect(
        _selector,
        &ConvSelectorWidget::convSelected,
        this,
        [this](const ConversationId &id, const QString &) {
            _fwdBtn->setEnabled(!id.value.isEmpty());
        }
    );

    connect(_composer, &ComposerWidget::sendRequested, this, [this](const QString &) {
        if (_fwdBtn->isEnabled())
            accept();
    });

    connect(_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(_fwdBtn, &QPushButton::clicked, this, &QDialog::accept);

    connect(_copyLinkBtn, &QPushButton::clicked, this, [&msg] {
        for (const auto &ent : msg.text.entities) {
            if (ent.type == EntityType::Link && !ent.data.isEmpty()) {
                QApplication::clipboard()->setText(ent.data);
                return;
            }
        }
    });

    applyTheme();
    updateCard();
}

void ForwardDialog::applyTheme() {
    AppDialog::applyTheme();
    _previewCard->setStyleSheet(
        QString("QFrame#fwdCard {"
                "  border: 1px solid %1;"
                "  border-radius: 6px;"
                "  background: %2;"
                "}")
            .arg(Th::qss(Th::c().surface.highlightStrong), Th::qss(Th::c().message.fileChipBg))
    );
    _copyLinkBtn->setStyleSheet(QString("QPushButton { border: 1px solid %1; border-radius: 4px;"
                                        " padding: 6px 14px; background: %2; }"
                                        "QPushButton:hover { background: %3; }")
                                    .arg(
                                        Th::qss(Th::c().divider.strong),
                                        Th::qss(Th::c().surface.raised),
                                        Th::qss(Th::c().surface.sunken)
                                    ));
    _cancelBtn->setStyleSheet(QString("QPushButton { border: 1px solid %1; border-radius: 4px;"
                                      " padding: 6px 18px; background: %2; }"
                                      "QPushButton:hover { background: %3; }")
                                  .arg(
                                      Th::qss(Th::c().divider.strong),
                                      Th::qss(Th::c().surface.raised),
                                      Th::qss(Th::c().surface.sunken)
                                  ));
    _fwdBtn->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: none;"
                                   " border-radius: 4px; padding: 6px 18px; font-weight: bold; }"
                                   "QPushButton:hover { background: %3; }"
                                   "QPushButton:disabled { background: %4; }")
                               .arg(
                                   Th::qss(Th::c().accent.def),
                                   Th::qss(Th::c().accent.text),
                                   Th::qss(Th::c().accent.dark),
                                   Th::qss(Th::c().icon.dim)
                               ));
}

ConversationId ForwardDialog::targetConv() const {
    return _selector->selectedConv();
}

QString ForwardDialog::comment() const {
    return _composer->currentText().trimmed();
}
