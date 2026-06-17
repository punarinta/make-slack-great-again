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
#include <QGraphicsOpacityEffect>
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

    // Card: the visible panel (header + results). Has its own opacity effect so
    // the content fades in as a unit while the overlay alpha is animated separately.
    _card = new QWidget(this);
    _card->setObjectName("searchCard");
    auto *cardLayout = new QVBoxLayout(_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto *cardEffect = new QGraphicsOpacityEffect(_card);
    cardEffect->setOpacity(0.0);
    _card->setGraphicsEffect(cardEffect);

    // Header row: search icon + input + close button
    _header = new QWidget(_card);
    _header->setObjectName("searchHeader");
    auto *hRow = new QHBoxLayout(_header);
    hRow->setContentsMargins(12, 8, 8, 8);
    hRow->setSpacing(8);

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
    _cardAnim = new QPropertyAnimation(cardEffect, "opacity", this);
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
    static_cast<QGraphicsOpacityEffect *>(_card->graphicsEffect())->setOpacity(0.0);
}

void SearchWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, _overlayAlpha));
}

void SearchWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    auto *cardEffect = static_cast<QGraphicsOpacityEffect *>(_card->graphicsEffect());

    disconnect(_overlayAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    disconnect(_cardAnim, &QPropertyAnimation::finished, nullptr, nullptr);
    _overlayAnim->stop();
    _cardAnim->stop();

    _overlayAnim->setEasingCurve(QEasingCurve::OutCubic);
    _overlayAnim->setStartValue(_overlayAlpha);
    _overlayAnim->setEndValue(Th::c().surface.overlay.alpha());

    _cardAnim->setEasingCurve(QEasingCurve::OutCubic);
    _cardAnim->setStartValue(cardEffect->opacity());
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
    auto *cardEffect = static_cast<QGraphicsOpacityEffect *>(_card->graphicsEffect());

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
    _cardAnim->setStartValue(cardEffect->opacity());
    _cardAnim->setEndValue(0.0);

    _overlayAnim->start();
    _cardAnim->start();
}

// ── Event filter (keyboard nav + tooltips) ────────────────────────────────────

bool SearchWidget::eventFilter(QObject *obj, QEvent *event) {
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
            const bool isDm = (conv->kind == ConvKind::Im || conv->kind == ConvKind::Mpim);
            if (isDm && conv->dmUser) {
                if (const auto *user = _session->findUser(*conv->dmUser)) {
                    return user->displayName.isEmpty() ? user->name : user->displayName;
                }
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
                result += t.text.mid(e.offset, e.length);
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

    _header->setStyleSheet(QString(
                               "QWidget#searchHeader {"
                               "  background: %1;"
                               "  border-bottom: 1px solid %2;"
                               "}"
    )
                               .arg(Th::qss(th.surface.raised), Th::qss(th.divider.def)));
    _queryEdit->setStyleSheet(QString(
                                  "QLineEdit {"
                                  "  border: 1px solid %1;"
                                  "  border-radius: 4px;"
                                  "  padding: 4px 8px;"
                                  "  font-size: %3px;"
                                  "}"
                                  "QLineEdit:focus { border-color: %2; }"
    )
                                  .arg(Th::qss(th.divider.strong), Th::qss(th.text.link))
                                  .arg(th.fonts.base));
    _closeBtn->setStyleSheet(
        "QPushButton#searchCloseBtn { border: none; background: transparent; }"
    );
    _searchIconLabel->setPixmap(svgPixmap(":/ui/search.svg", QSize(16, 16), th.icon.def));

    _resultList->setStyleSheet(
        QString(
            "QListWidget#searchResultList {"
            "  border: none;"
            "  background: %1;"
            "  outline: 0;"
            "}"
            "QListWidget#searchResultList::item {"
            "  padding: 8px 12px;"
            "  border-bottom: 1px solid %2;"
            "  color: %6;"
            "}"
            "QListWidget#searchResultList::item:hover {"
            "  background: %3;"
            "}"
            "QListWidget#searchResultList::item:selected {"
            "  background: %4;"
            "  color: %6;"
            "}"
            "QScrollBar:vertical {"
            "  background: transparent;"
            "  width: 6px;"
            "  margin: 2px;"
            "}"
            "QScrollBar::handle:vertical {"
            "  background: %5;"
            "  border-radius: 3px;"
            "  min-height: 20px;"
            "}"
            "QScrollBar::add-line:vertical,"
            "QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical,"
            "QScrollBar::sub-page:vertical { background: transparent; }"
        )
            .arg(
                Th::qss(th.surface.raised),          // %1 list bg
                Th::qss(th.divider.subtle),          // %2 item separator
                Th::qss(th.surface.highlight),       // %3 hover
                Th::qss(th.surface.highlightStrong), // %4 keyboard-selected (no accent blue)
                Th::qss(th.divider.strong),          // %5 scrollbar handle
                Th::qss(th.text.primary)             // %6 item text
            )
    );
}
