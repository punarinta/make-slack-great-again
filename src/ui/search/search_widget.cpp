// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "search_widget.h"
#include "session/session.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "util/time_format.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QLabel>
#include <QDateTime>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QShowEvent>
#include <QTimer>
#include <QPropertyAnimation>
#include <QPainter>
#include <QEasingCurve>

namespace {

// Takes Message::date (epoch micros) — the dedicated time field, not the id.
QString formatTs(qint64 dateMicros) {
    return TimeFmt::formatDateTime(dateMicros / 1000000);
}

} // namespace

SearchWidget::SearchWidget(QWidget *parent) : QWidget(parent) {
    setObjectName("searchWidget");
    // WA_NoSystemBackground: Qt skips erasing this widget's area before paintEvent,
    // leaving message list content in the backing store so the dark overlay
    // composites over it rather than over the parent's plain background.
    setAttribute(Qt::WA_NoSystemBackground);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Card: the visible panel (header + results). Its content fades in as a unit
    // while the overlay alpha is animated separately.
    _card = new QWidget(this);
    _card->setObjectName("searchCard");
    auto *cardLayout = new QVBoxLayout(_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // Qt Widgets can only fade a widget subtree with QGraphicsOpacityEffect, which
    // needs the whole Graphics View framework — which the static Linux build leaves
    // out (-no-feature-graphicsview, scripts/qt-features.sh). Instead a click-through veil over the
    // card paints what lies behind the card (the dimmed message area) at
    // 1 - cardOpacity: the same picture, and the query field still takes
    // keystrokes during the fade.
    _veil = new QWidget(this);
    _veil->setAttribute(Qt::WA_TransparentForMouseEvents);
    _veil->setFocusPolicy(Qt::NoFocus);
    _veil->installEventFilter(this);
    _veil->hide();
    _card->installEventFilter(this); // keeps the veil on the card's geometry

    // Header row: search icon + input + close button
    _header = new QWidget(_card);
    _header->setObjectName("searchHeader");
    auto       *hRow = new QHBoxLayout(_header);
    const auto &sp   = Th::c().spacing;
    hRow->setContentsMargins(sp.lg, sp.md, sp.md, sp.md);
    hRow->setSpacing(sp.md);

    _searchIconLabel = new QLabel(_header);
    _searchIconLabel->setFixedSize(20, 20);
    _searchIconLabel->setPixmap(svgPixmap(":/ui/search.svg", QSize(16, 16), Th::c().icon.def));
    _searchIconLabel->setAlignment(Qt::AlignCenter);
    _searchIconLabel->setAttribute(Qt::WA_Hover);
    _searchIconLabel->installEventFilter(this);
    hRow->addWidget(_searchIconLabel);

    _queryEdit = new QLineEdit(_header);
    _queryEdit->setPlaceholderText(tr("Search messages…"));
    _queryEdit->installEventFilter(this);
    connect(_queryEdit, &QLineEdit::returnPressed, this, [this] {
        runSearch(_queryEdit->text().trimmed());
    });
    hRow->addWidget(_queryEdit, 1);

    _closeBtn = new QPushButton(_header);
    _closeBtn->setObjectName("searchCloseBtn");
    _closeBtn->setFixedSize(24, 24);
    _closeBtn->setFlat(true);
    _closeBtn->setCursor(Qt::PointingHandCursor);
    _closeBtn->setIconSize(QSize(14, 14));
    _closeBtn->setIcon(svgIcon(":/ui/x.svg", QSize(14, 14), Th::c().icon.def));
    _closeBtn->installEventFilter(this);
    connect(_closeBtn, &QPushButton::clicked, this, &SearchWidget::closeRequested);
    hRow->addWidget(_closeBtn);

    cardLayout->addWidget(_header);

    _resultList = new QListWidget(_card);
    _resultList->setObjectName("searchResultList");
    _resultList->setFrameShape(QFrame::NoFrame);
    _resultList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _resultList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _resultList->setSelectionMode(QAbstractItemView::SingleSelection);
    _resultList->setFocusPolicy(Qt::NoFocus);
    _resultList->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    connect(_resultList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const int idx = item->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < (int)_results.size()) {
            emit resultSelected(_results[idx].conv, _results[idx].msg.ts);
            emit closeRequested();
        }
    });
    cardLayout->addWidget(_resultList);
    _resultList->hide(); // shown only once a search is run

    mainLayout->addWidget(_card);
    mainLayout->addStretch(1); // dark tinted area below the results card

    _searchIconTooltip = new PopupTooltip(this);
    _closeBtnTooltip   = new PopupTooltip(this);

    // overlayAlpha (0→target) drives paintEvent; cardAnim fades the panel content.
    _overlayAnim = new QPropertyAnimation(this, "overlayAlpha", this);
    _overlayAnim->setDuration(350);
    _cardAnim = new QPropertyAnimation(this, "cardOpacity", this);
    _cardAnim->setDuration(350);

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &SearchWidget::applyTheme
    );
}

// ── Visibility ────────────────────────────────────────────────────────────────

void SearchWidget::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    // Reset animation state so the next showEvent always fades in from zero,
    // regardless of whether we were closed via closeSearch() or a direct hide().
    _overlayAnim->stop();
    _cardAnim->stop();
    disconnect(_overlayAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    disconnect(_cardAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    _overlayAlpha = 0;
    setCardOpacity(0.0);
}

void SearchWidget::setCardOpacity(qreal o) {
    _cardOpacity = o;
    if (o >= 1.0) {
        _veil->hide();
        return;
    }
    _veil->setGeometry(_card->geometry());
    _veil->raise();
    _veil->show();
    _veil->update();
}

// Renders what this overlay covers, without the overlay itself: the parent's
// background plus every sibling stacked below us. Done at each open and close, so
// the fade-out shows the message area as it is now, not as it was at open time.
void SearchWidget::captureBackdrop() {
    const qreal dpr = devicePixelRatioF();
    QPixmap     pm((QSizeF(size()) * dpr).toSize());
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    if (QWidget *p = parentWidget()) {
        QPainter painter(&pm);
        p->render(&painter, -pos(), QRegion(geometry()), QWidget::DrawWindowBackground);
        for (QObject *o : p->children()) { // children() is the stacking order, bottom first
            if (o == this)
                break;
            auto *w = qobject_cast<QWidget *>(o);
            if (!w || w->isWindow() || !w->isVisible())
                continue;
            w->render(
                &painter,
                w->pos() - pos(),
                QRegion(),
                QWidget::DrawWindowBackground | QWidget::DrawChildren
            );
        }
    }
    _backdrop = pm;
}

void SearchWidget::paintVeil() {
    // What shows through a transparent card is the backdrop under the dimming
    // overlay. Compose the two first, then fade them as one: painting each at the
    // same opacity separately would not add up to the same picture.
    const qreal dpr = _backdrop.isNull() ? devicePixelRatioF() : _backdrop.devicePixelRatio();
    QPixmap     under;
    if (!_backdrop.isNull())
        under = _backdrop.copy(
            QRectF(QPointF(_veil->pos()) * dpr, QSizeF(_veil->size()) * dpr).toAlignedRect()
        );
    else {
        under = QPixmap((QSizeF(_veil->size()) * dpr).toSize());
        under.fill(Qt::transparent);
    }
    under.setDevicePixelRatio(dpr);
    {
        QPainter c(&under);
        c.fillRect(QRect(QPoint(), _veil->size()), QColor(0, 0, 0, _overlayAlpha));
    }
    QPainter p(_veil);
    p.setOpacity(1.0 - _cardOpacity);
    p.drawPixmap(0, 0, under);
}

void SearchWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, _overlayAlpha));
}

void SearchWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    captureBackdrop();
    setCardOpacity(_cardOpacity);

    disconnect(_overlayAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    disconnect(_cardAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    _overlayAnim->stop();
    _cardAnim->stop();

    _overlayAnim->setEasingCurve(QEasingCurve::OutCubic);
    _overlayAnim->setStartValue(_overlayAlpha);
    _overlayAnim->setEndValue(Th::c().surface.overlay.alpha());

    _cardAnim->setEasingCurve(QEasingCurve::OutCubic);
    _cardAnim->setStartValue(_cardOpacity);
    _cardAnim->setEndValue(1.0);

    _overlayAnim->start();
    _cardAnim->start();

    QTimer::singleShot(0, this, [this] {
        _queryEdit->setFocus();
        _queryEdit->selectAll();
    });
}

void SearchWidget::closeSearch() {
    if (!isVisible())
        return;
    captureBackdrop();

    disconnect(_overlayAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    disconnect(_cardAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    _overlayAnim->stop();
    _cardAnim->stop();

    _overlayAnim->setEasingCurve(QEasingCurve::InCubic);
    _overlayAnim->setStartValue(_overlayAlpha);
    _overlayAnim->setEndValue(0);
    connect(
        _overlayAnim, &QPropertyAnimation::finished, this, &QWidget::hide, Qt::SingleShotConnection
    );

    _cardAnim->setEasingCurve(QEasingCurve::InCubic);
    _cardAnim->setStartValue(_cardOpacity);
    _cardAnim->setEndValue(0.0);

    _overlayAnim->start();
    _cardAnim->start();
}

// ── Event filter (keyboard nav + tooltips) ────────────────────────────────────

bool SearchWidget::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _veil && event->type() == QEvent::Paint) {
        paintVeil();
        return true;
    }
    if (obj == _card && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        _veil->setGeometry(_card->geometry());
    if (obj == _queryEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Escape:
            emit closeRequested();
            return true;
        case Qt::Key_Up:
            navigateBy(-1);
            return true;
        case Qt::Key_Down:
            navigateBy(1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (_selectedIdx >= 0) {
                activateSelected();
                return true;
            }
            return false;
        default:
            break;
        }
        return false;
    }

    if (obj == _searchIconLabel) {
        if (event->type() == QEvent::Enter)
            _searchIconTooltip->showAbove(
                tr("Search messages"),
                QRect(_searchIconLabel->mapToGlobal(QPoint(0, 0)), _searchIconLabel->size())
            );
        else if (event->type() == QEvent::Leave)
            _searchIconTooltip->hide();
        return false;
    }

    if (obj == _closeBtn) {
        if (event->type() == QEvent::Enter)
            _closeBtnTooltip->showAbove(
                tr("Close search"), QRect(_closeBtn->mapToGlobal(QPoint(0, 0)), _closeBtn->size())
            );
        else if (event->type() == QEvent::Leave)
            _closeBtnTooltip->hide();
        return false;
    }

    return QWidget::eventFilter(obj, event);
}

// ── Public interface ──────────────────────────────────────────────────────────

void SearchWidget::focusInput() {
    _queryEdit->setFocus();
    _queryEdit->selectAll();
}

void SearchWidget::setSession(Session *session) {
    _session = session;
    _queryEdit->clear();
    _resultList->clear();
    _resultList->hide();
    _results.clear();
    _selectedIdx = -1;

    // A late-resolved external user (Slack Connect / system) means a result's
    // conv title or a mention in its preview can now show a real name — re-render
    // the visible results so the raw-id placeholder is replaced.
    _sessionLifetime = rpl::lifetime();
    if (_session)
        _session->userInfoLoaded() | rpl::on_next(
                                         [this](UserId) {
                                             if (!_results.empty() && isVisible())
                                                 populateResults(_results);
                                         },
                                         _sessionLifetime
                                     );
}

// ── Internal helpers ──────────────────────────────────────────────────────────

void SearchWidget::navigateBy(int delta) {
    if (_results.empty())
        return;
    if (_selectedIdx < 0)
        _selectedIdx = (delta > 0) ? 0 : (int)_results.size() - 1;
    else
        _selectedIdx = qBound(0, _selectedIdx + delta, (int)_results.size() - 1);
    _resultList->setCurrentRow(_selectedIdx);
    if (auto *cur = _resultList->currentItem())
        _resultList->scrollToItem(cur);
}

void SearchWidget::activateSelected() {
    if (_selectedIdx < 0 || _selectedIdx >= (int)_results.size())
        return;
    emit resultSelected(_results[_selectedIdx].conv, _results[_selectedIdx].msg.ts);
    emit closeRequested();
}

void SearchWidget::runSearch(const QString &query) {
    if (!_session || query.isEmpty())
        return;
    _resultList->clear();
    _results.clear();
    _selectedIdx = -1;
    _resultList->show();

    auto *loadingItem = new QListWidgetItem(tr("Searching…"));
    loadingItem->setForeground(Th::c().text.tertiary);
    loadingItem->setData(Qt::UserRole, -1);
    loadingItem->setFlags(loadingItem->flags() & ~Qt::ItemIsSelectable);
    _resultList->addItem(loadingItem);

    _session->searchMessages(query, [this](std::vector<SearchResult> results) {
        _results = std::move(results);
        populateResults(_results);
    });
}

QString SearchWidget::resolveConvName(const SearchResult &r) const {
    if (_session) {
        if (const auto *conv = _session->findConversation(r.conv)) {
            // For DMs resolve the other person's display name, not the raw user ID.
            if (conv->kind == ConvKind::Im && conv->dmUser)
                return _session->userDisplayName(*conv->dmUser);
            if (const QString custom = groupDmCustomName(*conv); !custom.isEmpty())
                return custom;
            if (conv->kind == ConvKind::Mpim && conv->dmUser) {
                if (const auto *user = _session->findUser(*conv->dmUser))
                    return user->displayName.isEmpty() ? user->name : user->displayName;
            }
            if (!conv->name.isEmpty())
                return conv->name;
        }
    }
    if (!r.convName.isEmpty())
        return r.convName;
    return {};
}

QString SearchWidget::resolvePreview(const TextWithEntities &t) const {
    if (t.entities.empty() || !_session)
        return t.text.left(120).replace('\n', ' ');

    QString result;
    int     pos = 0;
    for (const auto &e : t.entities) {
        if (e.offset > pos)
            result += t.text.mid(pos, e.offset - pos);
        if (e.type == EntityType::UserMention) {
            UserId uid;
            uid.value = e.data;
            if (const auto *user = _session->findUser(uid)) {
                const QString name = user->displayName.isEmpty() ? user->name : user->displayName;
                result += "@" + name;
            } else {
                // Keep the parser's baked label if it's a real name (<@W|Name>);
                // only a bare "@U…/@W…" is a raw id worth resolving away.
                const QString baked  = t.text.mid(e.offset, e.length);
                QString       bareId = baked;
                if (bareId.startsWith('@'))
                    bareId.remove(0, 1);
                result += _session->isUnresolvedUserId(bareId)
                              ? ("@" + _session->userDisplayName(uid))
                              : baked;
            }
        } else {
            result += t.text.mid(e.offset, e.length);
        }
        pos = e.offset + e.length;
    }
    if (pos < (int)t.text.size())
        result += t.text.mid(pos);

    return result.left(120).replace('\n', ' ');
}

void SearchWidget::populateResults(const std::vector<SearchResult> &results) {
    _resultList->clear();
    _selectedIdx = -1;

    if (results.empty()) {
        auto *item = new QListWidgetItem(tr("No results found."));
        item->setForeground(Th::c().text.tertiary);
        item->setData(Qt::UserRole, -1);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        _resultList->addItem(item);
        return;
    }

    for (int i = 0; i < (int)results.size(); ++i) {
        const auto   &r    = results[i];
        const QString name = resolveConvName(r);

        QString convLabel;
        if (name.isEmpty()) {
            convLabel = tr("Unknown channel");
        } else {
            bool isDm = false;
            if (_session) {
                if (const auto *conv = _session->findConversation(r.conv))
                    isDm = (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);
            }
            convLabel = isDm ? name : "#" + name;
        }

        const QString tsLabel = formatTs(r.msg.date);
        const QString preview = resolvePreview(r.msg.text);

        auto *item = new QListWidgetItem(_resultList);
        item->setData(Qt::UserRole, i);
        item->setText(convLabel + "  " + tsLabel + "\n" + preview);
        item->setToolTip(r.msg.text.text);
    }
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void SearchWidget::applyTheme() {
    const auto &th = Th::c();

    Th::setStyleSheetIfChanged(
        _header,
        QString(
            "QWidget#searchHeader {"
            "  background: %1;"
            "  border-bottom: 1px solid %2;"
            "}"
        )
            .arg(Th::qss(th.surface.raised), Th::qss(th.divider.def))
    );
    // Borderless Spotlight field: the header frame + the separate leading search
    // icon (with its own hover tooltip) are the chrome — see .rules (UI § search).
    Th::setStyleSheetIfChanged(
        _queryEdit,
        QString(
            "QLineEdit { border: none; background: transparent; padding: 4px 0; "
            "font-size: %1px; color: %2; }"
        )
            .arg(th.fonts.base)
            .arg(Th::qss(th.text.primary))
    );
    Th::setStyleSheetIfChanged(
        _closeBtn, "QPushButton#searchCloseBtn { border: none; background: transparent; }"
    );
    _searchIconLabel->setPixmap(svgPixmap(":/ui/search.svg", QSize(16, 16), th.icon.def));

    Th::setStyleSheetIfChanged(
        _resultList,
        QString(
            "QListWidget#searchResultList {"
            "  border: none;"
            "  background: %1;"
            "  outline: 0;"
            "}"
            "QListWidget#searchResultList::item {"
            "  padding: 8px 12px;"
            "  border-bottom: 1px solid %2;"
            "  color: %5;"
            "}"
            "QListWidget#searchResultList::item:hover {"
            "  background: %3;"
            "}"
            "QListWidget#searchResultList::item:selected {"
            "  background: %4;"
            "  color: %5;"
            "}"
        )
                .arg(
                    Th::qss(th.surface.raised),          // %1 list bg
                    Th::qss(th.divider.subtle),          // %2 item separator
                    Th::qss(th.surface.highlight),       // %3 hover
                    Th::qss(th.surface.highlightStrong), // %4 keyboard-selected (no accent blue)
                    Th::qss(th.text.primary)             // %5 item text
                ) +
            Th::scrollBarQss() // fold the old drifted 6px/r3 bar to the standard 8px/r4
    );
}
