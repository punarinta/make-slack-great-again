// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "forward_dialog.h"
#include "ui/conv_selector/conv_selector_widget.h"
#include "ui/dropdown/dropdown.h"
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
#include <algorithm>

ForwardDialog::ForwardDialog(
    const Message &msg, Session *session, std::vector<Workspace> workspaces, QWidget *parent
)
    : AppDialog(tr("Forward this message"), parent), _workspaces(std::move(workspaces)),
      _target(session) {
    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;

    // ── Target workspace (only when there is a choice) ────────────────
    if (_workspaces.size() > 1) {
        _wsPicker = new Dropdown;
        for (int i = 0; i < (int)_workspaces.size(); ++i) {
            _wsPicker->addItem(_workspaces[i].name);
            if (_workspaces[i].session == session)
                _wsPicker->setCurrentIndex(i);
        }
        if (_wsPicker->currentIndex() < 0) {
            _wsPicker->setCurrentIndex(0);
            _target = _workspaces.front().session;
        }
        cl->addWidget(_wsPicker);
    }

    // ── Conversation selector ──────────────────────────────────────────
    _selector = new ConvSelectorWidget(_target);
    cl->addWidget(_selector);

    // ── Optional comment via composer ─────────────────────────────────
    _composer = new ComposerWidget;
    _composer->setPlaceholderText(tr("Add a message, if you'd like."));
    // Mentions and emoji in the comment resolve in the workspace it's sent to.
    if (_target)
        _composer->setSession(_target);
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
    // No horizontal padding on the card: the preview is full-bleed so its
    // scrollbar hugs the right edge. Everything else is inset to sp.lg via
    // addPadded() so it lines up with the preview's left text margin.
    cardLay->setContentsMargins(0, sp.md, 0, sp.md);
    cardLay->setSpacing(sp.sm);

    auto addPadded = [&](QWidget *w) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(sp.lg, 0, sp.lg, 0);
        row->addWidget(w);
        row->addStretch();
        cardLay->addLayout(row);
    };

    if (session) {
        const auto   *user      = session->findUser(msg.author);
        const QString name      = user ? user->displayName : session->userDisplayName(msg.author);
        auto         *nameLabel = new QLabel(
            "<b>" + name.toHtmlEscaped() + "</b>" + "  <span style='color:" +
                Th::qss(Th::c().text.tertiary) + ";font-size:" + QString::number(Th::c().fonts.sm) +
                "px'>" + MsgRender::formatTs(msg.date) + "</span>",
            _previewCard
        );
        nameLabel->setTextFormat(Qt::RichText);
        addPadded(nameLabel);
    }

    auto *preview = new QTextBrowser(_previewCard);
    preview->setMaximumHeight(140);
    // Before setHtml: a resource added afterwards doesn't reach an already
    // laid-out <img>, and a message-link chip carries one.
    MsgRender::registerMessageLinkIcon(preview->document(), devicePixelRatioF());

    // Message text
    const QString html = MsgRender::buildMsgHtml(msg, session);
    if (!html.trimmed().isEmpty())
        preview->setHtml(html);
    else if (!msg.rawText.trimmed().isEmpty())
        preview->setPlainText(msg.rawText);
    else
        preview->hide(); // no text — skip the browser entirely

    // Shared preview chrome: thin scrollbar, full-bleed right edge, left text
    // padding that lines up with the name. Applied after content is set.
    MsgRender::configurePreviewBrowser(preview);
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
        addPadded(nameLabel);

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
        addPadded(imgLabel);

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
        addPadded(new FileChipWidget(f, _previewCard));
    }

    cl->addWidget(_previewCard);

    // ── Button bar ─────────────────────────────────────────────────────
    _copyLinkBtn = new StyledButton(tr("Copy Link"), StyledButton::Variant::Secondary);
    _cancelBtn   = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _fwdBtn      = new StyledButton(tr("Forward"), StyledButton::Variant::Primary);
    _fwdBtn->setEnabled(false);
    // [Copy Link]  →stretch→  [Cancel] [Forward];  Cancel → reject() wired by base.
    addButtonRow(_fwdBtn, _cancelBtn, _copyLinkBtn);

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

    connect(_fwdBtn, &QPushButton::clicked, this, &AppDialog::accept);

    if (_wsPicker)
        connect(_wsPicker, &Dropdown::currentIndexChanged, this, [this](int i) {
            if (i >= 0 && i < (int)_workspaces.size())
                setTargetSession(_workspaces[i].session);
        });

    // The modeless dialog outlives the caller's message (often a signal argument).
    connect(_copyLinkBtn, &QPushButton::clicked, this, [entities = msg.text.entities] {
        for (const auto &ent : entities) {
            if (ent.type == EntityType::Link && !ent.data.isEmpty()) {
                QApplication::clipboard()->setText(ent.data);
                return;
            }
            // A link to another message is a link too — its entity holds the
            // ref, so rebuild the permalink the user actually sees.
            if (ent.type == EntityType::MessageLink) {
                QApplication::clipboard()->setText(
                    SlackLinks::messagePermalink(SlackLinks::refFromToken(ent.data))
                );
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

void ForwardDialog::setTargetSession(Session *session) {
    if (session == _target)
        return;
    _target = session;
    // Clears the picked conversation (it belongs to the old workspace), which
    // disables Forward through convSelected until one is picked here.
    _selector->setSession(session);
    _composer->setSession(session);
}

bool ForwardDialog::usesSession(const Session *session) const {
    return session == _target || std::ranges::any_of(_workspaces, [session](const Workspace &w) {
               return w.session == session;
           });
}

ConversationId ForwardDialog::targetConv() const {
    return _selector->selectedConv();
}

QString ForwardDialog::comment() const {
    return _composer->currentText().trimmed();
}
