// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "history_search_popup.h"
#include "ui/paint_utils.h"
#include "ui/popup_placement.h"
#include "ui/shortcuts.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTextLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <vector>

namespace {
constexpr int kMargins   = 4;
constexpr int kListMaxH  = 320;
constexpr int kPadX      = 10;
constexpr int kPadY      = 6;
constexpr int kMaxLines  = 2; // of a prompt, per row
constexpr int kPageRows  = 5; // PageUp / PageDown
// A match further into a long prompt than this (in characters) would be out of
// sight in two lines, so the row starts shortly before it instead.
constexpr int kLateMatch = 100;
constexpr int kLeadIn    = 30;

QStringList queryWords(const QString &query) {
    static const QRegularExpression kSpace(QStringLiteral("\\s+"));
    return query.split(kSpace, Qt::SkipEmptyParts);
}
} // namespace

QList<int> HistorySearch::filter(const QStringList &entries, const QString &query) {
    const QStringList words = queryWords(query);
    QList<int>        out;
    for (int i = 0; i < entries.size(); ++i) {
        const bool all = std::all_of(words.cbegin(), words.cend(), [&](const QString &w) {
            return entries[i].contains(w, Qt::CaseInsensitive);
        });
        if (all)
            out << i;
    }
    return out;
}

QList<QPair<int, int>> HistorySearch::matchRanges(const QString &text, const QString &query) {
    QList<QPair<int, int>> found;
    for (const QString &w : queryWords(query))
        for (int at = text.indexOf(w, 0, Qt::CaseInsensitive); at >= 0;
             at     = text.indexOf(w, at + w.size(), Qt::CaseInsensitive))
            found.append({at, int(w.size())});
    std::sort(found.begin(), found.end());
    QList<QPair<int, int>> merged;
    for (const auto &r : found) {
        if (!merged.isEmpty() && r.first <= merged.last().first + merged.last().second) {
            auto &last  = merged.last();
            last.second = std::max(last.first + last.second, r.first + r.second) - last.first;
        } else {
            merged << r;
        }
    }
    return merged;
}

// ── The list of matches — custom-painted, oldest at the top ───────────────────

class HistorySearchList : public QWidget {
public:
    explicit HistorySearchList(HistorySearchPopup *popup) : _popup(popup) {
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus); // the search field keeps the keyboard
        setCursor(Qt::PointingHandCursor);
    }

    // Lays the rows out for `width`, and takes their height.
    void rebuild(int width) {
        _rows.clear();
        _hover  = -1;
        QFont f = font();
        f.setPixelSize(Th::c().fonts.base);
        _font               = f;
        const qreal   textW = width - 2 * kPadX;
        const qreal   ellW  = QFontMetricsF(f).horizontalAdvance(QStringLiteral("…"));
        const QString query = _popup->query();
        const int     n     = int(_popup->_matches.size());
        int           y     = 0;
        for (int r = 0; r < n; ++r) {
            Row row;
            row.match    = n - 1 - r; // newest at the bottom, next to the search field
            QString text = _popup->_entries[_popup->_matches[row.match]].simplified();
            if (const auto m = HistorySearch::matchRanges(text, query);
                !m.isEmpty() && m.first().first > kLateMatch)
                text = QStringLiteral("…") + text.mid(m.first().first - kLeadIn);

            row.layout = std::make_unique<QTextLayout>(text, f);
            QList<QTextLayout::FormatRange> formats;
            for (const auto &[start, length] : HistorySearch::matchRanges(text, query)) {
                QTextLayout::FormatRange fr;
                fr.start  = start;
                fr.length = length;
                fr.format.setFontWeight(QFont::DemiBold);
                fr.format.setBackground(Th::c().accent.subtleBg);
                formats << fr;
            }
            row.layout->setFormats(formats);
            QTextOption opt;
            opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            row.layout->setTextOption(opt);
            row.layout->beginLayout();
            qreal     lineY = 0;
            QTextLine last;
            for (int lines = 0; lines < kMaxLines; ++lines) {
                QTextLine line = row.layout->createLine();
                if (!line.isValid())
                    break;
                // The last line leaves room for the "…" of a longer prompt.
                line.setLineWidth(lines == kMaxLines - 1 ? textW - ellW : textW);
                line.setPosition(QPointF(0, lineY));
                lineY += line.height();
                last = line;
            }
            row.layout->endLayout();
            if (last.isValid() && last.textStart() + last.textLength() < text.size()) {
                row.more       = true;
                row.ellipsisAt = QPointF(last.naturalTextWidth(), last.y() + last.ascent());
            }
            row.y = y;
            row.h = int(std::ceil(lineY)) + 2 * kPadY;
            y += row.h;
            _rows.push_back(std::move(row));
        }
        setFixedHeight(std::max(y, 1));
        update();
    }

    QRect rowRect(int match) const {
        for (const Row &row : _rows)
            if (row.match == match)
                return QRect(0, row.y, width(), row.h);
        return {};
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(_font);
        for (const Row &row : _rows) {
            const QRect r(0, row.y, width(), row.h);
            if (!r.intersects(e->rect()))
                continue;
            if (row.match == _popup->_selected)
                Paint::rowHighlight(p, r.adjusted(0, 1, 0, -1), Th::c().surface.highlightStrong);
            else if (row.match == _hover)
                Paint::rowHighlight(p, r.adjusted(0, 1, 0, -1), Th::c().surface.highlight);
            p.setPen(Th::c().text.primary);
            const QPointF origin(kPadX, row.y + kPadY);
            row.layout->draw(&p, origin);
            if (row.more)
                p.drawText(origin + row.ellipsisAt, QStringLiteral("…"));
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        const int m = matchAt(e->position().toPoint().y());
        if (m != _hover) {
            _hover = m;
            update();
        }
    }

    void leaveEvent(QEvent *) override {
        _hover = -1;
        update();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;
        if (const int m = matchAt(e->position().toPoint().y()); m >= 0) {
            _popup->select(m);
            _popup->pick();
        }
    }

private:
    struct Row {
        std::unique_ptr<QTextLayout> layout;
        bool                         more = false; // the prompt goes on past the last line
        QPointF                      ellipsisAt;   // where its "…" goes
        int                          y     = 0;
        int                          h     = 0;
        int                          match = 0; // index into the popup's matches
    };

    int matchAt(int y) const {
        for (const Row &row : _rows)
            if (y >= row.y && y < row.y + row.h)
                return row.match;
        return -1;
    }

    HistorySearchPopup *_popup;
    std::vector<Row>    _rows; // top to bottom: oldest match first
    QFont               _font;
    int                 _hover = -1;
};

// ── The panel ─────────────────────────────────────────────────────────────────

HistorySearchPopup::HistorySearchPopup(QWidget *parent) : QFrame(parent) {
    setObjectName(QStringLiteral("historySearchPopup"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kMargins, kMargins, kMargins, kMargins);
    outer->setSpacing(kMargins);

    _scroll = new QScrollArea(this);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setFocusPolicy(Qt::NoFocus);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->verticalScrollBar()->setFocusPolicy(Qt::NoFocus);
    _scroll->setWidgetResizable(true);
    _list = new HistorySearchList(this);
    _list->setStyleSheet(QStringLiteral("background:transparent;"));
    _scroll->setWidget(_list);
    outer->addWidget(_scroll);

    _empty = new QLabel(tr("No earlier prompt matches"), this);
    _empty->setObjectName(QStringLiteral("historySearchEmpty"));
    _empty->setAlignment(Qt::AlignCenter);
    outer->addWidget(_empty);

    _search = new StyledLineEdit(this);
    _search->setLeadingIcon(QStringLiteral(":/ui/search.svg"));
    _search->setPlaceholderText(tr("Search earlier prompts"));
    _search->lineEdit()->installEventFilter(this);
    outer->addWidget(_search);
    connect(_search, &StyledLineEdit::textChanged, this, &HistorySearchPopup::refilter);

    // A click anywhere else closes the panel; the focus stays where it went.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (isVisible() && now && !isAncestorOf(now))
            dismiss();
    });
    // The composer it hangs over moves with the window: start again from there.
    if (parent)
        parent->installEventFilter(this);

    hide();
    applyTheme();
    connect(
        &ThemeManager::instance(),
        &ThemeManager::themeChanged,
        this,
        &HistorySearchPopup::applyTheme
    );
}

void HistorySearchPopup::applyTheme() {
    setStyleSheet(
        QStringLiteral(
            "QFrame#historySearchPopup {"
            "  background:%1;"
            "  border:1px solid %2;"
            "  border-radius:6px;"
            "}"
            "QScrollArea { background: transparent; border: none; }"
            "QLabel#historySearchEmpty { color:%3; padding:8px; }"
        )
            .arg(
                Th::qss(Th::c().surface.raised),
                Th::qss(Th::c().divider.strong),
                Th::qss(Th::c().text.secondary)
            ) +
        Th::popupScrollBarQss()
    );
    if (isVisible())
        place();
}

void HistorySearchPopup::open(
    const QStringList &entries, const QString &query, const QRect &anchor
) {
    _entries.clear();
    QSet<QString> seen;
    for (const QString &e : entries)
        if (!seen.contains(e)) {
            seen.insert(e);
            _entries << e;
        }
    _anchor = anchor;
    show();
    raise();
    _search->setText(query); // refilters when it changes the text…
    refilter();              // …and when it doesn't
    _search->lineEdit()->setFocus();
    _search->lineEdit()->end(false);
}

void HistorySearchPopup::dismiss() {
    hide();
}

QString HistorySearchPopup::query() const {
    return _search->text();
}

QStringList HistorySearchPopup::matches() const {
    QStringList out;
    for (int i : _matches)
        out << _entries[i];
    return out;
}

QString HistorySearchPopup::selectedEntry() const {
    return _selected < _matches.size() ? _entries[_matches[_selected]] : QString();
}

void HistorySearchPopup::refilter() {
    _matches  = HistorySearch::filter(_entries, query());
    _selected = 0;
    if (isVisible())
        place();
}

void HistorySearchPopup::place() {
    QWidget *par = parentWidget();
    if (!par)
        return;
    setFixedWidth(std::min(_anchor.width(), par->width()));
    const bool none = _matches.isEmpty();
    _scroll->setVisible(!none);
    _empty->setVisible(none);

    if (!none) {
        const int full = contentsRect().width() - 2 * kMargins; // inside the border
        _list->rebuild(full);
        if (_list->height() > kListMaxH) // a scroll bar takes its share of the width
            _list->rebuild(full - _scroll->verticalScrollBar()->sizeHint().width());
        _scroll->setFixedHeight(std::min(_list->height(), kListMaxH));
    }
    // The layout's own sum: margins, spacing, the border, the search field.
    layout()->invalidate();
    setFixedHeight(sizeHint().height());

    const QRect  bounds(0, 0, par->width(), par->height());
    const QPoint pos =
        Ui::placePopup(_anchor, size(), bounds, Ui::Edge::Above, 4, Ui::Align::Start);
    move(pos);
    if (layout())
        layout()->activate(); // the viewport's size, for the scroll below
    select(_selected);
}

void HistorySearchPopup::select(int match) {
    if (_matches.isEmpty())
        return;
    _selected       = std::clamp(match, 0, int(_matches.size()) - 1);
    const QRect row = _list->rowRect(_selected);
    _scroll->ensureVisible(0, row.center().y(), 0, row.height() / 2);
    _list->update();
}

void HistorySearchPopup::pick() {
    if (_matches.isEmpty())
        return;
    const QString text = selectedEntry();
    dismiss();
    emit picked(text);
}

bool HistorySearchPopup::eventFilter(QObject *obj, QEvent *event) {
    if (obj == parentWidget()) {
        if (event->type() == QEvent::Resize && isVisible())
            dismiss();
        return false;
    }
    if (obj != _search->lineEdit() || event->type() != QEvent::KeyPress)
        return QFrame::eventFilter(obj, event);
    auto *ke = static_cast<QKeyEvent *>(event);
    // Older is up, as in the list; Ctrl+R again goes on to the next older match.
    if (Ui::Shortcuts::matches(Ui::Shortcut::SearchPromptHistory, ke)) {
        select(_selected + 1);
        return true;
    }
    if (ke->modifiers() & ~Qt::KeypadModifier)
        return false;
    switch (ke->key()) {
    case Qt::Key_Up:
        select(_selected + 1);
        return true;
    case Qt::Key_Down:
        select(_selected - 1);
        return true;
    case Qt::Key_PageUp:
        select(_selected + kPageRows);
        return true;
    case Qt::Key_PageDown:
        select(_selected - kPageRows);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        pick();
        return true;
    case Qt::Key_Escape:
        dismiss();
        emit cancelled();
        return true;
    default:
        return false;
    }
}
