// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QWidget>
#include <vector>

class QFrame;
class QLineEdit;
class QLabel;
class QPushButton;
class QListWidget;
class Session;

// Combobox-style conversation picker.
// Shows a search field; typing filters a dropdown list.
// After selection, the field shows a chip with the name and an ×-clear button.
// Emits convSelected() on selection and convSelected({}, {}) when cleared.
class ConvSelectorWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConvSelectorWidget(Session *session, QWidget *parent = nullptr);
    ~ConvSelectorWidget();

    ConversationId selectedConv() const { return _selectedId; }
    QString        selectedName() const { return _selectedName; }

signals:
    void convSelected(const ConversationId &conv, const QString &name);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void applyTheme();
    void openDropdown();
    void closeDropdown();
    void positionDropdown();
    void rebuildList(const QString &filter);
    void selectRow(int row);
    void clearSelection();
    void showChip();
    void showSearch();

    Session       *_session;
    ConversationId _selectedId;
    QString        _selectedName;

    // Input frame (always visible)
    QFrame    *_inputFrame = nullptr;
    QLineEdit *_searchEdit = nullptr;

    // Chip shown when selection made (inside _inputFrame)
    QWidget     *_chip      = nullptr;
    QLabel      *_chipLabel = nullptr;
    QPushButton *_chipClear = nullptr;

    // Dropdown — parented to window() so it overlays siblings
    QFrame                     *_dropdown = nullptr;
    QListWidget                *_dropList = nullptr;
    std::vector<ConversationId> _listIds;
};
