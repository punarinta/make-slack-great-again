// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "file_dialog_utils.h"
#include "theme.h"

#include <QFileDialog>

namespace Ui {

void applyFileDialogTheme(QFileDialog *dlg) {
    dlg->setStyleSheet(Th::stockDialogQss());
}

QString getSaveFileName(QWidget *parent, const QString &title, const QString &pathWithName) {
    QFileDialog dlg(parent, title);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.selectFile(pathWithName); // absolute path also sets the directory
    applyFileDialogTheme(&dlg);
    if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty())
        return {};
    return dlg.selectedFiles().front();
}

QString getOpenFileName(QWidget *parent, const QString &title, const QString &filter) {
    QFileDialog dlg(parent, title);
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setFileMode(QFileDialog::ExistingFile);
    if (!filter.isEmpty())
        dlg.setNameFilter(filter);
    applyFileDialogTheme(&dlg);
    if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty())
        return {};
    return dlg.selectedFiles().front();
}

} // namespace Ui
