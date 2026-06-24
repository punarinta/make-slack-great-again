// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "conv_selector_widget.h"
#include "session/session.h"
#include "ui/popup_placement.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QVBoxLayout>

static constexpr int kDropMaxH = 200;

// Slack names a group DM "mpdm-alice--bob--carol-1"; pull the member usernames
// back out so we can show display names instead of the raw id. Mirrors the
// conversation list's helper of the same name.
static QStringList parseMpdmUsernames(const QString &name) {
    QString s = name;
    if (s.startsWith("mpdm-"))
        s = s.mid(5);
    // Strip a trailing numeric suffix like "-1".
    const int lastDash = s.lastIndexOf('-');
    if (lastDash > 0) {
        bool ok = false;
        s.mid(lastDash + 1).toInt(&ok);
        if (ok)
            s = s.left(lastDash);
    }
    return s.split("--", Qt::SkipEmptyParts);
}

ConvSelectorWidget::ConvSelectorWidget(Session *session, QWidget *parent)
    : QWidget(parent), _session(session) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Input frame (styled text-input look) ─────────────────────────
    _inputFrame = new QFrame(this);
    _inputFrame->setObjectName("convInput");
    _inputFrame->setFixedHeight(36);

    auto       *inputLay = new QHBoxLayout(_inputFrame);
    const auto &sp       = Th::c().spacing;
    inputLay->setContentsMargins(sp.md, 0, sp.md, 0);
    inputLay->setSpacing(sp.sm);

    // Search edit (shown when no selection)
    _searchEdit = new QLineEdit(_inputFrame);
    _searchEdit->setPlaceholderText(tr("Search channels and people…"));
    _searchEdit->setFrame(false);
    _searchEdit->setStyleSheet("QLineEdit { background: transparent; border: none; }");
    inputLay->addWidget(_searchEdit);

    // Chip row (shown when item selected)
    _chip         = new QWidget(_inputFrame);
    auto *chipLay = new QHBoxLayout(_chip);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(sp.sm);

    _chipLabel = new QLabel(_chip);

    _chipClear = new QPushButton("×", _chip);
    _chipClear->setFixedSize(18, 18);
    _chipClear->setFlat(true);
    _chipClear->setCursor(Qt::PointingHandCursor);

    chipLay->addWidget(_chipLabel);
    chipLay->addWidget(_chipClear);
    chipLay->addStretch();
    _chip->hide();

    inputLay->addWidget(_chip);
    outer->addWidget(_inputFrame);

    // ── Dropdown — parented to window() so it overlays content ───────
    // Created lazily in openDropdown() once window() is valid.

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });

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

    connect(_chipClear, &QPushButton::clicked, this, [this] { clearSelection(); });

    qApp->installEventFilter(this);
}

ConvSelectorWidget::~ConvSelectorWidget() {
    qApp->removeEventFilter(this);
    if (_dropdown)
        _dropdown->deleteLater();
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void ConvSelectorWidget::applyTheme() {
    _inputFrame->setStyleSheet(
        QString(
            "QFrame#convInput {"
            "  border: 1px solid %1;"
            "  border-radius: 4px;"
            "  background: white;"
            "}"
            "QFrame#convInput:focus-within {"
            "  border-color: %2;"
            "}"
        )
            .arg(Th::qss(Th::c().divider.strong), Th::qss(Th::c().accent.def))
    );
    _chipLabel->setStyleSheet(
        QString(
            "QLabel { background: %1; color: %2;"
            " border-radius: 12px; padding: 2px 8px;"
            " font-weight: bold; }"
        )
            .arg(Th::qss(Th::c().accent.subtleBg), Th::qss(Th::c().message.replyLink))
    );
    _chipClear->setStyleSheet(
        QString(
            "QPushButton { border: none; color: #666; font-size: %1px; padding: 0; }"
            "QPushButton:hover { color: #333; }"
        )
            .arg(Th::c().fonts.base)
    );
}

// ── Dropdown management ───────────────────────────────────────────────────────

void ConvSelectorWidget::openDropdown() {
    if (!_dropdown) {
        // In-window child overlay of the top-level window — NOT a Qt::Tool
        // top-level, which the Wayland compositor positions itself (ignoring our
        // setGeometry), making the list land at the screen's top-left. Same
        // reason AppDialog / PopupTooltip / the mention popups are child overlays.
        _dropdown = new QFrame(window());
        _dropdown->setAttribute(Qt::WA_StyledBackground, true);
        _dropdown->setObjectName("convDropdown");
        _dropdown->setStyleSheet(QString(
                                     "QFrame#convDropdown {"
                                     "  background: white;"
                                     "  border: 1px solid %1;"
                                     "  border-radius: 4px;"
                                     "}"
        )
                                     .arg(Th::qss(Th::c().divider.strong)));

        auto *lay = new QVBoxLayout(_dropdown);
        lay->setContentsMargins(0, Th::c().spacing.xs, 0, Th::c().spacing.xs);
        lay->setSpacing(0);

        _dropList = new QListWidget(_dropdown);
        _dropList->setFrameShape(QFrame::NoFrame);
        _dropList->setStyleSheet(QString(
                                     "QListWidget { border: none; background: transparent; }"
                                     "QListWidget::item { padding: 6px 12px; }"
                                     "QListWidget::item:hover { background: %1; }"
                                     "QListWidget::item:selected { background: %2; color: %3; }"
                                     // Thin rounded scrollbar, matching our other lists.
                                     "QScrollBar:vertical {"
                                     "  background: transparent; width: 8px; margin: 2px;"
                                     "}"
                                     "QScrollBar::handle:vertical {"
                                     "  background: %4; border-radius: 3px; min-height: 24px;"
                                     "}"
                                     "QScrollBar::add-line:vertical,"
                                     "QScrollBar::sub-line:vertical { height: 0; }"
                                     "QScrollBar::add-page:vertical,"
                                     "QScrollBar::sub-page:vertical { background: transparent; }"
        )
                                     .arg(
                                         Th::qss(Th::c().surface.highlight),
                                         Th::qss(Th::c().accent.subtleBg),
                                         Th::qss(Th::c().message.replyLink),
                                         Th::qss(Th::c().divider.strong)
                                     ));
        _dropList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        lay->addWidget(_dropList);

        connect(_dropList, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
            selectRow(_dropList->currentRow());
        });
    }

    rebuildList(_searchEdit->text());
    // An empty result set shows nothing rather than a stray empty box — e.g. "/"
    // (or any non-matching query) matches no conversation.
    if (_dropList->count() == 0) {
        _dropdown->hide();
        return;
    }
    positionDropdown();
    _dropdown->show();
    _dropdown->raise();
}

void ConvSelectorWidget::closeDropdown() {
    if (_dropdown)
        _dropdown->hide();
}

void ConvSelectorWidget::positionDropdown() {
    if (!_dropdown)
        return;

    // The dropdown is a child of the top-level window, so it is positioned in the
    // window's coordinate space (NOT screen-global): map the input's anchor and
    // clamp within the window rect.
    QWidget *win = window();
    if (!win)
        return;
    const QRect anchor(
        win->mapFromGlobal(_inputFrame->mapToGlobal(QPoint(0, 0))), _inputFrame->size()
    );
    const int w     = _inputFrame->width();
    const int itemH = _dropList->sizeHintForRow(0);
    const int count = std::min(_dropList->count(), 6);
    const int listH = count > 0 ? std::min(kDropMaxH, itemH * count + 4) : 40;

    // Drop below the input, flipping above / clamping within the window if there
    // is no room (previously this ran off-screen with no bounds check).
    const QRect  bounds(0, 0, win->width(), win->height());
    const QPoint pos =
        Ui::placePopup(anchor, QSize(w, listH), bounds, Ui::Edge::Below, 1, Ui::Align::Start);
    _dropdown->setGeometry(QRect(pos, QSize(w, listH)));
}

void ConvSelectorWidget::rebuildList(const QString &filter) {
    if (!_dropList)
        return;
    _dropList->clear();
    _listIds.clear();
    if (!_session)
        return;

    // A leading "#" scopes to channels, "@" to people/DMs (and is stripped from
    // the text query); anything else is a plain name search across both.
    enum class Scope { Any, Channels, People } scope = Scope::Any;
    QString query                                    = filter;
    if (query.startsWith('#')) {
        scope = Scope::Channels;
        query = query.mid(1);
    } else if (query.startsWith('@')) {
        scope = Scope::People;
        query = query.mid(1);
    }

    // Lowercased username → user, for resolving group-DM members the API names
    // only by username (built once; the user list is stable during this call).
    QHash<QString, const User *> userByName;
    for (const auto &u : _session->currentUsers())
        userByName.insert(u.name.toLower(), &u);

    for (const auto &conv : _session->currentConversations()) {
        const bool isChannel =
            conv.kind == ConvKind::PublicChannel || conv.kind == ConvKind::PrivateChannel;
        const bool isDm = conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim;
        if (scope == Scope::Channels && !isChannel)
            continue;
        if (scope == Scope::People && !isDm)
            continue;

        QString label;
        if (conv.kind == ConvKind::Im) {
            const auto *u = conv.dmUser ? _session->findUser(*conv.dmUser) : nullptr;
            label         = u ? u->displayName
                              : (conv.dmUser ? _session->userDisplayName(*conv.dmUser) : conv.name);
        } else if (conv.kind == ConvKind::Mpim) {
            // Group DM: list the members' names (minus self) instead of the raw
            // "mpdm-alice--bob-1" id. The API often omits conv.members, so fall
            // back to parsing the usernames out of the name and resolving them.
            QStringList  names;
            const UserId me = _session->meUserId();
            if (!conv.members.empty()) {
                for (const auto &uid : conv.members) {
                    if (!me.value.isEmpty() && uid == me)
                        continue;
                    const QString n = _session->userDisplayName(uid);
                    if (!n.isEmpty())
                        names.append(n);
                }
            } else {
                for (const QString &uname : parseMpdmUsernames(conv.name)) {
                    const User *u = userByName.value(uname.toLower(), nullptr);
                    if (u && !me.value.isEmpty() && u->id == me)
                        continue;
                    // Unresolved username still beats the full mpdm id.
                    names.append(u && !u->displayName.isEmpty() ? u->displayName : uname);
                }
            }
            label = names.isEmpty() ? "#" + conv.name : names.join(QStringLiteral(", "));
        } else {
            label = "#" + conv.name;
        }
        // Match the bare name (without the leading '#') so "#gen" finds "#general".
        const QString hay = label.startsWith('#') ? label.mid(1) : label;
        if (!query.isEmpty() && !hay.contains(query, Qt::CaseInsensitive))
            continue;
        _dropList->addItem(label);
        _listIds.push_back(conv.id);
    }
    positionDropdown();
}

void ConvSelectorWidget::selectRow(int row) {
    if (row < 0 || row >= (int)_listIds.size())
        return;
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
        const auto  *me   = static_cast<const QMouseEvent *>(event);
        const QPoint gpos = me->globalPosition().toPoint();
        // _dropdown is a child overlay now, so its geometry() is in window-local
        // coords — compare in global space via each widget's own mapToGlobal.
        const QRect  dropG(_dropdown->mapToGlobal(QPoint(0, 0)), _dropdown->size());
        const QRect  inputG(_inputFrame->mapToGlobal(QPoint(0, 0)), _inputFrame->size());
        if (!dropG.contains(gpos) && !inputG.contains(gpos))
            closeDropdown();
    }
    return false;
}
