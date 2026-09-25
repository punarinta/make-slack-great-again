// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "ui/app_dialog/app_dialog.h"

#include <QColor>
#include <vector>

class QLabel;
class QPlainTextEdit;
class StyledButton;
class StyledLineEdit;
class GlyphSwatch;

// "Add a teammate" / "Edit teammate" in an agent workspace: a name, a line
// about it, its picture (a glyph on a colour, AvatarGlyphs) and the
// instructions it adds to the agent's own. Instructions apply to sessions
// started from then on — the dialog says so when editing.
//
// After exec() == Accepted: role() is what to save (Backend::saveAgentRole),
// unless restoreRequested() — "Restore default" on an edited built-in.
class TeammateDialog : public AppDialog {
    Q_OBJECT
public:
    // `role` with an empty id adds a teammate.
    explicit TeammateDialog(const AgentRole &role, QWidget *parent = nullptr);

    AgentRole role() const;
    bool      restoreRequested() const { return _restore; }

protected:
    void applyTheme() override;

private:
    void pickGlyph(const QString &id);
    void pickColor(const QColor &color);
    void updatePreview();
    void updateSaveEnabled();

    AgentRole                  _role;
    QString                    _glyph;
    QColor                     _color;
    bool                       _restore = false;
    QLabel                    *_preview = nullptr;
    StyledLineEdit            *_name    = nullptr;
    StyledLineEdit            *_desc    = nullptr;
    QPlainTextEdit            *_prompt  = nullptr;
    std::vector<QLabel *>      _labels;
    std::vector<QLabel *>      _hints;
    std::vector<GlyphSwatch *> _glyphs;
    std::vector<GlyphSwatch *> _colors;
    StyledButton              *_saveBtn = nullptr;
};

// "Remove teammate": asks before taking an added teammate off the team.
class RemoveTeammateDialog : public AppDialog {
    Q_OBJECT
public:
    RemoveTeammateDialog(const QString &name, QWidget *parent = nullptr);

protected:
    void applyTheme() override;

private:
    QLabel *_text = nullptr;
};
