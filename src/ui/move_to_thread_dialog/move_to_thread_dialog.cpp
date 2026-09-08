// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "move_to_thread_dialog.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "util/time_format.h"

#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

MoveToThreadDialog::MoveToThreadDialog(
    std::vector<ThreadChoice> threads, ImageCache *imgCache, QWidget *parent
)
    : AppDialog(tr("Move to thread"), parent, Scroll::Disabled), _threads(std::move(threads)),
      _imgCache(imgCache) {
    auto       *lay = contentLayout();
    const auto &sp  = Th::c().spacing;
    lay->setSpacing(sp.md);

    // What "move" means here has to be said up front: there is no API that
    // re-parents a message, so the copy is posted by the mover and the
    // original goes away.
    _note = new QLabel(
        tr("Pick a thread in this channel. The message is posted there again by you, "
           "and the original is deleted."),
        card()
    );
    _note->setWordWrap(true);
    lay->addWidget(_note);

    _searchEdit = new StyledLineEdit(card());
    _searchEdit->setPlaceholderText(tr("Filter threads…"));
    _searchEdit->setLeadingIcon(":/ui/search.svg");
    _searchEdit->lineEdit()->installEventFilter(this);
    lay->addWidget(_searchEdit);

    _list = new BrowseListView(_imgCache, card());
    _list->setObjectName("moveToThreadList");
    _list->setMinimumHeight(kListMinH);
    // A click highlights; it never confirms (see the class comment).
    _list->onActivated = [this](const QString &id) {
        for (int row = 0; row < _list->visibleCount(); ++row) {
            if (_list->idAt(row) == id) {
                _list->setSelectedRow(row);
                break;
            }
        }
        syncMoveButton();
    };
    lay->addWidget(_list, 1);

    _empty = new QLabel(card());
    _empty->setAlignment(Qt::AlignCenter);
    _empty->setWordWrap(true);
    _empty->setMinimumHeight(kListMinH);
    _empty->hide();
    lay->addWidget(_empty, 1);

    _noteBox = new QCheckBox(tr("Add a note with the original author and time"), card());
    _noteBox->setChecked(false);
    lay->addWidget(_noteBox);

    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _moveBtn   = new StyledButton(tr("Move"), StyledButton::Variant::Primary);
    _moveBtn->setEnabled(false);
    addButtonRow(_moveBtn, _cancelBtn);
    connect(_moveBtn, &QPushButton::clicked, this, [this] {
        if (!selectedRoot().isEmpty())
            accept();
    });

    buildItems();
    applyFilter({});

    connect(_searchEdit, &StyledLineEdit::textChanged, this, &MoveToThreadDialog::applyFilter);

    applyTheme();
    updateCard();
}

void MoveToThreadDialog::buildItems() {
    std::vector<BrowseListView::Item> items;
    items.reserve(_threads.size());
    for (const auto &t : _threads) {
        BrowseListView::Item it;
        it.id    = t.root;
        it.title = t.text.simplified();
        if (it.title.isEmpty())
            it.title = tr("(no text)");
        const QString replies =
            t.replyCount == 1 ? tr("1 reply") : tr("%1 replies").arg(t.replyCount);
        it.subtitle = t.author + QStringLiteral(" · ") +
                      TimeFmt::formatDateTime(t.date / 1'000'000) + QStringLiteral(" · ") + replies;
        it.isPerson  = true; // avatar disc of the root's author
        it.avatarUrl = t.avatarUrl;
        it.initial   = t.author.left(1);
        it.searchKey = (t.text + ' ' + t.author).toLower();
        items.push_back(std::move(it));
    }
    _list->setItems(std::move(items));
}

void MoveToThreadDialog::applyFilter(const QString &query) {
    _list->applyFilter(query);
    // Preselect the top match so Enter confirms the highlighted row — the same
    // commitment as the quick switcher, and the highlight is on screen.
    _list->setSelectedRow(0);

    const bool any = _list->visibleCount() > 0;
    _list->setVisible(any);
    _empty->setText(
        _threads.empty() ? tr("No threads in this channel's loaded history yet.")
                         : tr("No threads match.")
    );
    _empty->setVisible(!any);
    syncMoveButton();
}

void MoveToThreadDialog::syncMoveButton() {
    _moveBtn->setEnabled(!selectedRoot().isEmpty());
}

Ts MoveToThreadDialog::selectedRoot() const {
    return _list->selectedId(); // empty with no selection or no visible rows
}

bool MoveToThreadDialog::addNote() const {
    return _noteBox->isChecked();
}

void MoveToThreadDialog::showEvent(QShowEvent *e) {
    AppDialog::showEvent(e);
    QTimer::singleShot(0, this, [this] { _searchEdit->lineEdit()->setFocus(); });
}

bool MoveToThreadDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _searchEdit->lineEdit() && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Down:
            _list->moveSelection(1);
            syncMoveButton();
            return true;
        case Qt::Key_Up:
            _list->moveSelection(-1);
            syncMoveButton();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!selectedRoot().isEmpty())
                accept();
            return true;
        default:
            break;
        }
    }
    return AppDialog::eventFilter(obj, event);
}

void MoveToThreadDialog::applyTheme() {
    AppDialog::applyTheme();
    const auto &th = Th::c();
    if (_note)
        _note->setStyleSheet(QString("font-size: %1px; color: %2;")
                                 .arg(th.fonts.caption)
                                 .arg(Th::qss(th.text.secondary)));
    if (_noteBox)
        _noteBox->setStyleSheet(Th::checkBoxQss(th.fonts.md));
    if (_empty)
        _empty->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.base).arg(Th::qss(th.text.tertiary))
        );
}
