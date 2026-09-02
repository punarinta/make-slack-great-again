// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "quick_switcher_dialog.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/shortcuts.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

static constexpr int kCardPadH = 24;
static constexpr int kCardPadT = 20;
static constexpr int kCardPadB = 16;

QuickSwitcherDialog::QuickSwitcherDialog(
    std::vector<NamedConversation> conversations, ImageCache *imgCache, QWidget *parent
)
    : AppDialog(parent, Chrome::Custom, Scroll::Disabled), _conversations(std::move(conversations)),
      _imgCache(imgCache) {
    // Card, backdrop, centring, Escape and the Cmd+W hand-off all come from
    // AppDialog; Chrome::Custom means no title header — the field is the header,
    // Spotlight-style.
    auto       *lay = contentLayout();
    const auto &sp  = Th::c().spacing;

    _searchEdit = new StyledLineEdit(card());
    _searchEdit->setPlaceholderText(tr("Jump to a conversation…"));
    _searchEdit->setLeadingIcon(":/ui/search.svg");
    _searchEdit->lineEdit()->installEventFilter(this);

    auto *fieldRow = new QVBoxLayout;
    fieldRow->setContentsMargins(kCardPadH, kCardPadT, kCardPadH, sp.lg);
    fieldRow->addWidget(_searchEdit);
    lay->addLayout(fieldRow);

    _list = new BrowseListView(_imgCache, card());
    _list->setObjectName("quickSwitcherList");
    _list->setMinimumHeight(kListMinH);
    _list->onActivated = [this](const QString &id) {
        accept();
        emit conversationActivated(ConversationId{id});
    };
    lay->addWidget(_list, 1);

    // Shown in the list's place when nothing matches — the empty virtual list
    // would otherwise just be a blank rectangle.
    _empty = new QLabel(tr("No conversations match."), card());
    _empty->setAlignment(Qt::AlignCenter);
    _empty->setMinimumHeight(kListMinH);
    _empty->hide();
    lay->addWidget(_empty, 1);

    // Arrow keys aren't discoverable on a field that looks like plain search.
    _hint = new QLabel(card());
    _hint->setAlignment(Qt::AlignCenter);
    _hint->setText(tr("%1 to move · %2 to open")
                       .arg(
                           QString(QChar(0x2191)) + QChar(0x2193),
                           Ui::Shortcuts::nativeKeys(Ui::Shortcut::SendMessage)
                       ));
    auto *hintRow = new QVBoxLayout;
    hintRow->setContentsMargins(kCardPadH, sp.md, kCardPadH, kCardPadB);
    hintRow->addWidget(_hint);
    lay->addLayout(hintRow);

    buildItems();
    applyFilter({});

    connect(_searchEdit, &StyledLineEdit::textChanged, this, &QuickSwitcherDialog::applyFilter);
    connect(_searchEdit, &StyledLineEdit::returnPressed, this, [this] {
        _list->activateSelected();
    });

    applyTheme();
    updateCard();
}

void QuickSwitcherDialog::buildItems() {
    std::vector<BrowseListView::Item> items;
    items.reserve(_conversations.size());
    for (const auto &conv : _conversations) {
        if (conv.name.isEmpty())
            continue; // an id we can't name yet is not something to offer

        const bool isDm = conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim;

        BrowseListView::Item it;
        it.id        = conv.id.value;
        it.title     = conv.name;
        // No subtitle: this list is names, not metadata. Group DMs share the
        // person treatment (an initial disc) — a "#" would read as a channel.
        it.isPerson  = isDm;
        it.isPrivate = conv.kind == ConvKind::PrivateChannel;
        it.avatarUrl = conv.avatarUrl;
        it.initial   = conv.name.left(1);
        it.searchKey = conv.name.toLower();
        items.push_back(std::move(it));
    }
    _list->setItems(std::move(items));
}

void QuickSwitcherDialog::applyFilter(const QString &query) {
    _list->applyFilter(query);
    // Preselect the top match so Enter always opens something: with no keyboard
    // selection the list would need an arrow press first.
    _list->setSelectedRow(0);

    const bool any = _list->visibleCount() > 0;
    _list->setVisible(any);
    _empty->setVisible(!any);
}

void QuickSwitcherDialog::showEvent(QShowEvent *e) {
    AppDialog::showEvent(e);
    // AppDialog hands focus to the first focusable child; make sure that is the
    // field and not the list, so typing filters immediately.
    QTimer::singleShot(0, this, [this] {
        _searchEdit->lineEdit()->setFocus();
        _searchEdit->lineEdit()->selectAll();
    });
}

bool QuickSwitcherDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _searchEdit->lineEdit() && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Down:
            _list->moveSelection(1);
            return true;
        case Qt::Key_Up:
            _list->moveSelection(-1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            _list->activateSelected();
            return true;
        default:
            break;
        }
    }
    return AppDialog::eventFilter(obj, event);
}

void QuickSwitcherDialog::applyTheme() {
    AppDialog::applyTheme(); // card + backdrop

    // StyledLineEdit themes itself; the two labels don't.
    const auto &th = Th::c();
    if (_hint)
        _hint->setStyleSheet(QString("font-size: %1px; color: %2;")
                                 .arg(th.fonts.caption)
                                 .arg(Th::qss(th.text.tertiary)));
    if (_empty)
        _empty->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.base).arg(Th::qss(th.text.tertiary))
        );
}
