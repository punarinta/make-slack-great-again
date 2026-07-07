// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/summary_dialog/summary_dialog.h"

#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"
#include "util/clipboard.h"

#include <QLabel>
#include <QScrollArea>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
// Report card: 50% wider/taller than the AppDialog defaults (card 560 wide,
// content-sized height). Width is clamped to the overlay by cardWidth(); the
// body height to the host window here (the overlay caps the card at
// window - 80 anyway, so a too-tall minimum would clip the button row instead
// of scrolling).
constexpr int kCardMinW          = 480;
constexpr int kCardMaxW          = 840;
constexpr int kBodyH             = 420;
// Vertical card chrome around the scroll area (paddings + header + button
// row), deliberately over-estimated: the scroll minimum plus this must stay
// under AppDialog's window - 80 card cap or the button row gets clipped.
constexpr int kCardChromeH       = 260;
// Relaxed body line height (% of the font's natural line). QLabel has no
// line-height knob for Markdown, so the text goes through a QTextDocument
// whose blocks carry the height, serialized back to rich text.
constexpr int kBodyLineHeightPct = 130;

QString markdownToSpacedHtml(const QString &markdown) {
    QTextDocument doc;
    doc.setMarkdown(markdown);
    QTextCursor cursor(&doc);
    cursor.select(QTextCursor::Document);
    QTextBlockFormat bf;
    bf.setLineHeight(kBodyLineHeightPct, QTextBlockFormat::ProportionalHeight);
    cursor.mergeBlockFormat(bf);
    return doc.toHtml();
}
} // namespace

SummaryDialog::SummaryDialog(const QString &markdown, Kind kind, QWidget *parent)
    : AppDialog(tr("Discussion summary"), parent), _kind(kind), _markdown(markdown) {
    auto *cl = contentLayout();

    _body = new QLabel;
    _body->setTextFormat(Qt::RichText);
    _body->setText(markdownToSpacedHtml(markdown));
    _body->setWordWrap(true);
    _body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _body->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    if (_kind == Kind::Report) {
        // The report scrolls instead of growing the card past the window (see
        // the settings-dialog rule: give tall content room to scroll, never
        // squeeze it). Notices are one-liners — no scroll area for them.
        _scroll = new QScrollArea;
        _scroll->setWidgetResizable(true);
        _scroll->setFrameShape(QFrame::NoFrame);
        _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        _scroll->viewport()->setAutoFillBackground(false);
        _scroll->setWidget(_body);
        _body->setAutoFillBackground(false); // setWidget() force-enables it
        const int hostH = parentWidget() ? parentWidget()->height() : 0;
        _scroll->setMinimumHeight(
            hostH > 0 ? std::min(kBodyH, std::max(120, hostH - kCardChromeH)) : kBodyH
        );
        cl->addWidget(_scroll, 1);

        // Escape and the header × close the dialog (AppDialog); no Close button.
        _copyBtn = new StyledButton(tr("Copy"), StyledButton::Variant::Primary);
        connect(_copyBtn, &QPushButton::clicked, this, [this] {
            Clipboard::setText(_markdown);
            _copyBtn->setText(tr("Copied"));
            QTimer::singleShot(1400, _copyBtn, [this] { _copyBtn->setText(tr("Copy")); });
        });
        addButtonRow(nullptr, nullptr, _copyBtn);
    } else {
        cl->addWidget(_body);
        if (_kind == Kind::NoProvider) {
            auto *settingsBtn =
                new StyledButton(tr("Open settings"), StyledButton::Variant::Primary);
            connect(settingsBtn, &QPushButton::clicked, this, [this] {
                accept();
                emit openSettingsRequested();
            });
            addButtonRow(settingsBtn);
        }
        // Kind::Failure: no buttons — the header × / Escape close it.
    }

    applyTheme();
    updateCard();
}

int SummaryDialog::cardWidth(int availOverlayWidth) const {
    if (_kind != Kind::Report)
        return AppDialog::cardWidth(availOverlayWidth);
    return std::clamp(availOverlayWidth, kCardMinW, kCardMaxW);
}

void SummaryDialog::applyTheme() {
    AppDialog::applyTheme();
    _body->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));
    if (_scroll)
        _scroll->setStyleSheet(
            QStringLiteral("QScrollArea { background: transparent; }") + Th::scrollBarQss()
        );
}
