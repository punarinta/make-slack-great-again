// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "rename_conversation_dialog.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

// Slack caps channel names at 80; a group DM alias needs no more.
static constexpr int kMaxNameLen = 80;

RenameConversationDialog::RenameConversationDialog(
    const QString &currentName, const QString &derivedName, QWidget *parent, Kind kind
)
    : AppDialog(
          kind == Kind::AgentSession ? tr("Rename session") : tr("Name conversation"), parent
      ) {
    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;
    cl->setSpacing(sp.md);

    _sectionLabel = new QLabel(tr("Name"));
    QFont lf      = _sectionLabel->font();
    lf.setBold(true);
    _sectionLabel->setFont(lf);
    cl->addWidget(_sectionLabel);

    _edit = new StyledLineEdit;
    _edit->setMaxLength(kMaxNameLen);
    _edit->setPlaceholderText(derivedName);
    _edit->setText(currentName);
    _edit->lineEdit()->selectAll();
    cl->addWidget(_edit);

    _hint = new QLabel(
        kind == Kind::AgentSession
            ? tr("Only msga shows this name; Claude Code keeps its own. Leave it empty to use "
                 "Claude Code's name again.")
            : tr("Only you see this name. Leave it empty to show the members' names again.")
    );
    _hint->setWordWrap(true);
    cl->addWidget(_hint);

    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _saveBtn   = new StyledButton(tr("Save"), StyledButton::Variant::Primary);
    addButtonRow(_saveBtn, _cancelBtn); // Cancel → reject() wired by base

    connect(_saveBtn, &QPushButton::clicked, this, &AppDialog::accept);
    connect(_edit, &StyledLineEdit::returnPressed, this, &AppDialog::accept);

    applyTheme();
    updateCard();
    _edit->setFocus();
}

QString RenameConversationDialog::name() const {
    return _edit->text().trimmed();
}

void RenameConversationDialog::applyTheme() {
    AppDialog::applyTheme();
    if (_sectionLabel)
        _sectionLabel->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));
    if (_hint)
        _hint->setStyleSheet(QString("color: %1; font-size: %2px;")
                                 .arg(Th::qss(Th::c().text.secondary))
                                 .arg(Th::c().fonts.sm));
}
