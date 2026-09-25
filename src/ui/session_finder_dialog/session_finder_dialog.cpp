// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "session_finder_dialog.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/icon_button/icon_button.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "util/relative_time.h"

#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QVBoxLayout>

static constexpr int kCardPadH = 24;
static constexpr int kCardPadT = 20;
static constexpr int kCardPadB = 20;

SessionFinderDialog::SessionFinderDialog(ImageCache *imgCache, QWidget *parent)
    : AppDialog(parent, Chrome::Custom) {
    auto *cardLayout = contentLayout();

    // ── Top bar: search + create + close, as in "Find a channel" ─────────────
    auto *topBar = new QWidget(card());
    topBar->setStyleSheet("background: transparent;");
    {
        auto       *lay = new QHBoxLayout(topBar);
        const auto &sp  = Th::c().spacing;
        lay->setContentsMargins(kCardPadH, kCardPadT, kCardPadH, sp.lg);
        lay->setSpacing(sp.md);

        _searchEdit = new StyledLineEdit(topBar);
        _searchEdit->setPlaceholderText(tr("Search for sessions"));
        _searchEdit->setLeadingIcon(":/ui/search.svg");
        _searchEdit->lineEdit()->setClearButtonEnabled(true);
        _searchEdit->lineEdit()->installEventFilter(this);
        _searchEdit->setMinimumWidth(200);
        lay->addWidget(_searchEdit, 1);

        _createBtn =
            new StyledButton(tr("Create a session"), StyledButton::Variant::Primary, topBar);
        _createBtn->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(_createBtn);

        _closeBtn = new IconButton(QStringLiteral(":/ui/x.svg"), 32, 14, topBar);
        lay->addWidget(_closeBtn, 0, Qt::AlignVCenter);
    }
    cardLayout->addWidget(topBar);

    auto *divider = new QFrame(card());
    divider->setFrameShape(QFrame::HLine);
    divider->setFixedHeight(1);
    _divider = divider;
    cardLayout->addWidget(divider);

    _stack = new QStackedWidget(card());
    _stack->setMinimumHeight(kListMinH);
    _status = new QLabel(tr("Looking for sessions…"), _stack);
    _status->setAlignment(Qt::AlignCenter);
    _status->setWordWrap(true);
    _stack->addWidget(_status);
    _list              = new BrowseListView(imgCache, _stack);
    _list->onActivated = [this](const QString &id) {
        accept();
        emit sessionActivated(id);
    };
    _stack->addWidget(_list);
    cardLayout->addWidget(_stack, 1);
    cardLayout->addSpacing(kCardPadB);

    connect(_closeBtn, &QPushButton::clicked, this, &AppDialog::reject);
    connect(_createBtn, &QPushButton::clicked, this, [this] {
        reject();
        emit createSessionRequested();
    });
    connect(_searchEdit, &StyledLineEdit::textChanged, this, &SessionFinderDialog::applyFilter);

    applyTheme();
    updateCard();
    _searchEdit->lineEdit()->setFocus();
}

void SessionFinderDialog::setSessions(std::vector<FoundSession> sessions) {
    const QString                     home = QDir::homePath();
    std::vector<BrowseListView::Item> items;
    items.reserve(sessions.size());
    for (const FoundSession &s : sessions) {
        BrowseListView::Item it;
        it.id          = s.id;
        it.isPerson    = true; // the agent's picture, no channel icon
        it.avatarUrl   = s.avatarUrl;
        // Untitled: what was asked first stands in for a title.
        it.title       = !s.title.isEmpty()         ? s.title
                         : !s.firstPrompt.isEmpty() ? s.firstPrompt
                                                    : s.lastPrompt;
        it.initial     = it.title.left(1).toUpper();
        QString folder = s.folder;
        if (folder == home || folder.startsWith(home + QLatin1Char('/')))
            folder = QLatin1Char('~') + folder.mid(home.size());
        QStringList sub;
        if (!folder.isEmpty())
            sub << folder;
        if (s.lastActiveMs > 0)
            sub << relativeTime(s.lastActiveMs / 1000);
        if (!s.lastPrompt.isEmpty() && s.lastPrompt != it.title)
            sub << s.lastPrompt;
        it.subtitle  = sub.join(QStringLiteral(" · "));
        it.badge     = s.listed.value.isEmpty() ? QString() : tr("In the list");
        it.searchKey = QStringList{s.title, s.folder, s.firstPrompt, s.lastPrompt, s.id}
                           .join(QLatin1Char('\n'))
                           .toLower();
        items.push_back(std::move(it));
    }
    _loaded = true;
    _list->setItems(std::move(items));
    applyFilter(_searchEdit->text());
}

void SessionFinderDialog::applyFilter(const QString &query) {
    if (!_loaded)
        return;
    _list->applyFilter(query.trimmed().toLower());
    if (_list->visibleCount() == 0) {
        _status->setText(
            _list->count() == 0 ? tr("There are no sessions yet.") : tr("No sessions match.")
        );
        _stack->setCurrentWidget(_status);
        return;
    }
    _stack->setCurrentWidget(_list);
    _list->setSelectedRow(0);
}

bool SessionFinderDialog::eventFilter(QObject *watched, QEvent *event) {
    // The search field drives the list: arrows move, Enter opens.
    if (watched == _searchEdit->lineEdit() && event->type() == QEvent::KeyPress &&
        _stack->currentWidget() == _list) {
        switch (static_cast<QKeyEvent *>(event)->key()) {
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
    return AppDialog::eventFilter(watched, event);
}

void SessionFinderDialog::applyTheme() {
    AppDialog::applyTheme();
    if (_divider)
        _divider->setStyleSheet(
            QString("background: %1; border: none;").arg(Th::qss(Th::c().divider.subtle))
        );
    if (_status)
        _status->setStyleSheet(
            QString("color: %1; background: transparent;").arg(Th::qss(Th::c().text.secondary))
        );
}
