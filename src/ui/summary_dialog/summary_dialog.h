// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"

class QLabel;
class QScrollArea;
class StyledButton;

// The "Summarize down" result card. Three shapes, so the flow always resolves
// into something visible:
//  - Report:     the Markdown summary in a scrollable body, Copy as the only
//                action button (closing = header × / Escape, from AppDialog).
//  - Failure:    a short notice (LLM error, empty span), no buttons.
//  - NoProvider: the "connect an AI provider first" notice with an
//                "Open settings" button that emits openSettingsRequested() —
//                the host window routes it to Settings → AI assistance.
class SummaryDialog : public AppDialog {
    Q_OBJECT
public:
    enum class Kind { Report, Failure, NoProvider };

    explicit SummaryDialog(
        const QString &markdown, Kind kind = Kind::Report, QWidget *parent = nullptr
    );

signals:
    // "Open settings" clicked on the NoProvider notice (fires after the dialog
    // closes itself).
    void openSettingsRequested();

protected:
    void applyTheme() override;
    // 50% wider than the AppDialog default (560 → 840) for the report, still
    // clamped to the available overlay width so it never outgrows the window.
    // Notices keep the default width.
    int  cardWidth(int availOverlayWidth) const override;

private:
    Kind          _kind;
    QScrollArea  *_scroll  = nullptr; // Report only
    QLabel       *_body    = nullptr;
    StyledButton *_copyBtn = nullptr; // Report only
    QString       _markdown;
};
