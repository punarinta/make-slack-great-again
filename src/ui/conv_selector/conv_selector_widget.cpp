// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "conv_selector_widget.h"
#include "session/session.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

static constexpr int kDropMaxH = 200;

ConvSelectorWidget::ConvSelectorWidget(Session *session, QWidget *parent)
    : QWidget(parent)
    , _session(session)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Input frame (styled text-input look) ─────────────────────────
    _inputFrame = new QFrame(this);
    _inputFrame->setObjectName("convInput");
    _inputFrame->setFixedHeight(36);
    _inputFrame->setStyleSheet(
        "QFrame#convInput {"
        "  border: 1px solid #CCCCCC;"
        "  border-radius: 4px;"
        "  background: white;"
        "}"
        "QFrame#convInput:focus-within {"
        "  border-color: #007A5A;"
        "}"
    );

    auto *inputLay = new QHBoxLayout(_inputFrame);
    inputLay->setContentsMargins(8, 0, 8, 0);
    inputLay->setSpacing(4);

    // Search edit (shown when no selection)
    _searchEdit = new QLineEdit(_inputFrame);
    _searchEdit->setPlaceholderText(tr("Search channels and people…"));
    _searchEdit->setFrame(false);
    _searchEdit->setStyleSheet("QLineEdit { background: transparent; border: none; }");
    inputLay->addWidget(_searchEdit);

    // Chip row (shown when item selected)
    _chip = new QWidget(_inputFrame);
    auto *chipLay = new QHBoxLayout(_chip);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(4);

    _chipLabel = new QLabel(_chip);
    _chipLabel->setStyleSheet(
        "QLabel { background: #E8F5FA; color: #1164A3;"
        " border-radius: 12px; padding: 2px 8px;"
        " font-weight: bold; }"
    );

    _chipClear = new QPushButton("×", _chip);
    _chipClear->setFixedSize(18, 18);
    _chipClear->setFlat(true);
    _chipClear->setCursor(Qt::PointingHandCursor);
    _chipClear->setStyleSheet(
        "QPushButton { border: none; color: #666; font-size: 14px; padding: 0; }"
        "QPushButton:hover { color: #333; }"
    );

    chipLay->addWidget(_chipLabel);
    chipLay->addWidget(_chipClear);
    chipLay->addStretch();
    _chip->hide();

    inputLay->addWidget(_chip);
    outer->addWidget(_inputFrame);

    // ── Dropdown — parented to window() so it overlays content ───────
    // Created lazily in openDropdown() once window() is valid.

    // ── Connections ───────────────────────────────────────────────────
    connect(_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        rebuildList(text);
        if (!text.isEmpty())
            openDropdown();
        else
            closeDropdown();
    });

    connect(_searchEdit, &QLineEdit::returnPressed, this, [this] {
        if (_dropList && _dropList->count() > 0 && _dropList->currentRow() < 0)
            _dropList->setCurrentRow(0);
        if (_dropList && _dropList->currentRow() >= 0)
            selectRow(_dropList->currentRow());
    });

    connect(_chipClear, &QPushButton::clicked, this, [this] {
        clearSelection();
    });

    qApp->installEventFilter(this);
}

ConvSelectorWidget::~ConvSelectorWidget() {
    qApp->removeEventFilter(this);
    if (_dropdown) _dropdown->deleteLater();
}

// ── Dropdown management ───────────────────────────────────────────────────────

void ConvSelectorWidget::openDropdown() {
    if (!_dropdown) {
        _dropdown = new QFrame(window(), Qt::Tool | Qt::FramelessWindowHint
                               | Qt::NoDropShadowWindowHint);
        _dropdown->setObjectName("convDropdown");
        _dropdown->setStyleSheet(
            "QFrame#convDropdown {"
            "  background: white;"
            "  border: 1px solid #CCCCCC;"
            "  border-radius: 4px;"
            "}"
        );

        auto *lay = new QVBoxLayout(_dropdown);
        lay->setContentsMargins(0, 2, 0, 2);
        lay->setSpacing(0);

        _dropList = new QListWidget(_dropdown);
        _dropList->setFrameShape(QFrame::NoFrame);
        _dropList->setStyleSheet(
            "QListWidget { border: none; }"
            "QListWidget::item { padding: 6px 12px; }"
            "QListWidget::item:hover { background: #F0F0F0; }"
            "QListWidget::item:selected { background: #E8F5FA; color: #1164A3; }"
        );
        _dropList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        lay->addWidget(_dropList);

        connect(_dropList, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
            selectRow(_dropList->currentRow());
        });
    }

    rebuildList(_searchEdit->text());
    positionDropdown();
    _dropdown->show();
    _dropdown->raise();
}

void ConvSelectorWidget::closeDropdown() {
    if (_dropdown) _dropdown->hide();
}

void ConvSelectorWidget::positionDropdown() {
    if (!_dropdown) return;

    const QPoint globalPos = _inputFrame->mapToGlobal(QPoint(0, _inputFrame->height()));
    const int w = _inputFrame->width();
    const int itemH = _dropList->sizeHintForRow(0);
    const int count = std::min(_dropList->count(), 6);
    const int listH = count > 0 ? std::min(kDropMaxH, itemH * count + 4) : 40;
    _dropdown->setGeometry(globalPos.x(), globalPos.y(), w, listH);
}

void ConvSelectorWidget::rebuildList(const QString &filter) {
    if (!_dropList) return;
    _dropList->clear();
    _listIds.clear();
    if (!_session) return;

    for (const auto &conv : _session->currentConversations()) {
        QString label;
        if (conv.kind == ConvKind::Im) {
            const auto *u = conv.dmUser ? _session->findUser(*conv.dmUser) : nullptr;
            label = u ? u->displayName : conv.name;
        } else {
            label = "#" + conv.name;
        }
        if (!filter.isEmpty() && !label.contains(filter, Qt::CaseInsensitive))
            continue;
        _dropList->addItem(label);
        _listIds.push_back(conv.id);
    }
    positionDropdown();
}

void ConvSelectorWidget::selectRow(int row) {
    if (row < 0 || row >= (int)_listIds.size()) return;
    _selectedId   = _listIds[row];
    _selectedName = _dropList->item(row)->text();
    closeDropdown();
    showChip();
    emit convSelected(_selectedId, _selectedName);
}

void ConvSelectorWidget::clearSelection() {
    _selectedId   = {};
    _selectedName = {};
    showSearch();
    emit convSelected({}, {});
}

void ConvSelectorWidget::showChip() {
    _chipLabel->setText(_selectedName);
    _searchEdit->hide();
    _chip->show();
}

void ConvSelectorWidget::showSearch() {
    _chip->hide();
    _searchEdit->clear();
    _searchEdit->show();
    _searchEdit->setFocus();
}

// ── Outside-click dismissal ───────────────────────────────────────────────────

bool ConvSelectorWidget::eventFilter(QObject *, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress && _dropdown && _dropdown->isVisible()) {
        const auto *me = static_cast<const QMouseEvent *>(event);
        const QPoint gpos = me->globalPosition().toPoint();
        if (!_dropdown->geometry().contains(gpos) && !_inputFrame->geometry().contains(
                _inputFrame->mapFromGlobal(gpos))) {
            closeDropdown();
        }
    }
    return false;
}
