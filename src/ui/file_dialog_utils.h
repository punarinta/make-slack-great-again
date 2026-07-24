// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QString>

class QFileDialog;
class QWidget;

namespace Ui {

// Every QFileDialog must go through here (or call applyFileDialogTheme before
// showing): when Qt has no native dialog helper (e.g. a Qt6 build on a Qt5
// Plasma desktop) it falls back to its widget-based dialog, which inherits our
// ancestor `QWidget { background: … }` stylesheet but keeps the OS palette's
// text colors — unreadable whenever the OS theme's lightness differs from the
// app theme's. Harmless when the native dialog is used (palette is ignored).
void applyFileDialogTheme(QFileDialog *dlg);

// QFileDialog::getSaveFileName / getOpenFileName with the theme applied.
// `pathWithName` is the preselected target (directory + default file name).
QString getSaveFileName(QWidget *parent, const QString &title, const QString &pathWithName);
QString getOpenFileName(QWidget *parent, const QString &title, const QString &filter);

} // namespace Ui
