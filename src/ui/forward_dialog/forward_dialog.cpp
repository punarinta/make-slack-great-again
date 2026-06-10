// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "forward_dialog.h"
#include "ui/conv_selector/conv_selector_widget.h"
#include "ui/composer/composer_widget.h"
#include "ui/message_list/file_chip_widget.h"
#include "ui/message_list/message_render.h"
#include "ui/theme.h"
#include "session/session.h"

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
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

    // Message text
    const QString html = MsgRender::buildMsgHtml(msg, session);
    if (!html.trimmed().isEmpty())
        preview->setHtml(html);
    else if (!msg.rawText.trimmed().isEmpty())
        preview->setPlainText(msg.rawText);
    else
        preview->hide(); // no text — skip the browser entirely

    cardLay->addWidget(preview);

    // ── Inline image thumbnails (filename above, image below — mirrors the message list) ──
    static constexpr int kThumbMaxH = 150;
    static constexpr int kThumbMaxW = 300;
    for (const auto &f : msg.files) {
        if (!f.isImage())
            continue;

        // Filename label
        auto *nameLabel = new QLabel(f.name, _previewCard);
        {
            QFont nf = nameLabel->font();
            nf.setPointSizeF(nf.pointSizeF() * 0.82);
            nameLabel->setFont(nf);
        }
        nameLabel->setStyleSheet("color:" + Th::qss(Th::c().message.fileNameDim) + ";");
        cardLay->addWidget(nameLabel);

        // Compute placeholder size from metadata (avoids layout jump when image loads)
        int phW = kThumbMaxW, phH = kThumbMaxH;
        if (f.imageWidth > 0 && f.imageHeight > 0) {
            const double scale = std::min(
                1.0, std::min((double)kThumbMaxW / f.imageWidth, (double)kThumbMaxH / f.imageHeight)
            );
            phW = (int)(f.imageWidth * scale);
            phH = (int)(f.imageHeight * scale);
        }

        auto *imgLabel = new QLabel(_previewCard);
        imgLabel->setFixedSize(phW, phH);
        imgLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        imgLabel->setStyleSheet(
            "QLabel { background:" + Th::qss(Th::c().message.imagePlaceholderBg) +
            "; border: 1px solid " + Th::qss(Th::c().message.imagePlaceholderBorder) + "; }"
        );
        cardLay->addWidget(imgLabel);

        if (session) {
            const QString    url       = f.thumbUrl.isEmpty() ? f.urlPrivate : f.thumbUrl;
            QPointer<QLabel> safeLabel = imgLabel;
            session->downloadFile(url, [safeLabel](QByteArray data) {
                if (!safeLabel)
                    return;
                QPixmap px;
                if (!px.loadFromData(data))
                    return;
                const double scale = std::min(
                    1.0, std::min((double)kThumbMaxW / px.width(), (double)kThumbMaxH / px.height())
                );
                px = px.scaled(
                    (int)(px.width() * scale),
                    (int)(px.height() * scale),
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation
                );
                safeLabel->setFixedSize(px.size());
                safeLabel->setPixmap(px);
                safeLabel->setStyleSheet({});
            });
        }
    }

    // ── Non-image file chips — identical appearance to the message list ────────
    for (const auto &f : msg.files) {
        if (f.isImage())
            continue;
        cardLay->addWidget(new FileChipWidget(f, _previewCard));
    }

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
        QString(
            "QFrame#fwdCard {"
            "  border: 1px solid %1;"
            "  border-radius: 6px;"
            "  background: %2;"
            "}"
        )
            .arg(Th::qss(Th::c().surface.highlightStrong), Th::qss(Th::c().message.fileChipBg))
    );
    _copyLinkBtn->setStyleSheet(QString(
                                    "QPushButton { border: 1px solid %1; border-radius: 4px;"
                                    " padding: 6px 14px; background: %2; }"
                                    "QPushButton:hover { background: %3; }"
    )
                                    .arg(
                                        Th::qss(Th::c().divider.strong),
                                        Th::qss(Th::c().surface.raised),
                                        Th::qss(Th::c().surface.sunken)
                                    ));
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
    _fwdBtn->setStyleSheet(QString(
                               "QPushButton { background: %1; color: %2; border: none;"
                               " border-radius: 4px; padding: 6px 18px; font-weight: bold; }"
                               "QPushButton:hover { background: %3; }"
                               "QPushButton:disabled { background: %4; }"
    )
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
