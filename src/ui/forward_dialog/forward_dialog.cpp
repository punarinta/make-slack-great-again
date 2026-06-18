// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "forward_dialog.h"
#include "ui/conv_selector/conv_selector_widget.h"
#include "ui/composer/composer_widget.h"
#include "ui/message_list/file_chip_widget.h"
#include "ui/message_list/message_render.h"
#include "ui/styled_button/styled_button.h"
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
#include <QtMath>

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
                "px'>" + MsgRender::formatTs(msg.date) + "</span>",
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

    // ── Inline previews (filename above, image below — mirrors the message list) ──
    static constexpr int kThumbMaxH = 150;
    static constexpr int kThumbMaxW = 300;
    for (const auto &f : msg.files) {
        if (!f.hasPreview())
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
            const QString    url       = f.previewUrl(qCeil(kThumbMaxW * devicePixelRatioF()));
            const bool       hasDims   = f.imageWidth > 0 && f.imageHeight > 0;
            QPointer<QLabel> safeLabel = imgLabel;
            session->downloadFile(url, [safeLabel, hasDims, phW, phH](QByteArray data) {
                if (!safeLabel)
                    return;
                QPixmap px;
                if (!px.loadFromData(data))
                    return;
                // Display size comes from the original dimensions (the placeholder
                // size), not from whichever thumbnail resolution was fetched.
                QSize logical(phW, phH);
                if (!hasDims) {
                    const double scale = std::min(
                        1.0,
                        std::min((double)kThumbMaxW / px.width(), (double)kThumbMaxH / px.height())
                    );
                    logical = QSize((int)(px.width() * scale), (int)(px.height() * scale));
                }
                const qreal dpr = safeLabel->devicePixelRatioF();
                QPixmap     scaled =
                    px.scaled(logical * dpr, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                scaled.setDevicePixelRatio(dpr);
                safeLabel->setFixedSize(logical);
                safeLabel->setPixmap(scaled);
                safeLabel->setStyleSheet({});
            });
        }
    }

    // ── File chips for files without a preview — identical to the message list ─
    for (const auto &f : msg.files) {
        if (f.hasPreview())
            continue;
        cardLay->addWidget(new FileChipWidget(f, _previewCard));
    }

    cl->addWidget(_previewCard);

    // ── Button bar ─────────────────────────────────────────────────────
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);

    _copyLinkBtn = new StyledButton(tr("Copy Link"), StyledButton::Variant::Secondary);
    btnRow->addWidget(_copyLinkBtn);
    btnRow->addStretch();

    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);

    _fwdBtn = new StyledButton(tr("Forward"), StyledButton::Variant::Primary);
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
    // Copy Link / Cancel / Forward buttons self-theme (StyledButton).
}

ConversationId ForwardDialog::targetConv() const {
    return _selector->selectedConv();
}

QString ForwardDialog::comment() const {
    return _composer->currentText().trimmed();
}
