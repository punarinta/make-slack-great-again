// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "emoji_picker_popup.h"
#include "session/session.h"
#include "ui/image_cache.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/emoji.h"
#include "util/emoji_font.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>

// Ordered list of emoji names for grid display — resolved via Emoji::fromName.
static const QStringList kBaseEmojiNames{
    // Faces & emotions
    "smile",
    "grin",
    "laughing",
    "joy",
    "rofl",
    "sweat_smile",
    "wink",
    "heart_eyes",
    "kissing_heart",
    "stuck_out_tongue",
    "thinking_face",
    "raised_eyebrow",
    "neutral_face",
    "expressionless",
    "zipper_mouth",
    "grimacing",
    "sob",
    "tired_face",
    "sleepy",
    "mask",
    "sunglasses",
    "nerd_face",
    "monocle_face",
    "confused",
    "worried",
    "angry",
    "rage",
    "skull",
    "ghost",
    "alien",
    "poop",
    "clown_face",
    "partying_face",
    // Hands & people
    "wave",
    "raised_hand",
    "ok_hand",
    "thumbsup",
    "thumbsdown",
    "clap",
    "pray",
    "point_right",
    "point_left",
    "point_up",
    "point_down",
    "muscle",
    "handshake",
    "writing_hand",
    "selfie",
    // Hearts & symbols
    "heart",
    "orange_heart",
    "yellow_heart",
    "green_heart",
    "blue_heart",
    "purple_heart",
    "broken_heart",
    "sparkling_heart",
    "two_hearts",
    "100",
    "tada",
    "fire",
    "star",
    "star2",
    "sparkles",
    "zap",
    "boom",
    "eyes",
    "warning",
    // Animals
    "dog",
    "cat",
    "mouse",
    "hamster",
    "rabbit",
    "fox_face",
    "bear",
    "panda_face",
    "koala",
    "tiger",
    "lion",
    "cow",
    "pig",
    "frog",
    "monkey_face",
    "chicken",
    "penguin",
    "bird",
    "hatching_chick",
    "eagle",
    "owl",
    "snake",
    "turtle",
    "lizard",
    "whale",
    "dolphin",
    "shark",
    "octopus",
    "bee",
    "butterfly",
    "palm_tree",
    "deciduous_tree",
    "evergreen_tree",
    "cactus",
    "sunflower",
    "rose",
    // Food
    "apple",
    "banana",
    "watermelon",
    "grapes",
    "strawberry",
    "pizza",
    "hamburger",
    "fries",
    "hot_dog",
    "taco",
    "burrito",
    "sushi",
    "ramen",
    "spaghetti",
    "rice",
    "bread",
    "croissant",
    "cake",
    "cupcake",
    "cookie",
    "chocolate_bar",
    "candy",
    "lollipop",
    "ice_cream",
    "coffee",
    "tea",
    "beer",
    // Travel & places
    "rocket",
    "airplane",
    "car",
    "bus",
    "train",
    "bicycle",
    "boat",
    "house",
    "office",
    "school",
    "hospital",
    "bank",
    "sunrise",
    "city_sunset",
    "night_with_stars",
    "earth_americas",
    "earth_africa",
    "earth_asia",
    // Objects & misc
    "computer",
    "desktop_computer",
    "keyboard",
    "phone",
    "telephone",
    "email",
    "memo",
    "pencil",
    "paperclip",
    "scissors",
    "lock",
    "key",
    "hammer",
    "wrench",
    "gear",
    "bulb",
    "flashlight",
    "books",
    "moneybag",
    "credit_card",
    "chart",
    "trophy",
    "medal",
    "gift",
    "balloon",
    "confetti_ball",
    "musical_note",
    "headphones",
    "microphone",
    "camera",
    "hourglass_flowing_sand",
    "clock1",
    "calendar",
    "x",
    "question",
    "exclamation",
    "bell",
    "information_source",
    "white_check_mark",
    "warning",
};

// Full popup size; height shrinks below kFullHeight to fit a short filtered list.
static constexpr int kFullWidth  = 300;
static constexpr int kFullHeight = 340;

// ── EmojiGrid ────────────────────────────────────────────────────────────────

EmojiGrid::EmojiGrid(QWidget *parent) : VirtualListWidget(parent) {
    _emojiFont = emojiFont(20);
    verticalScrollBar()->setSingleStep(kRowH);
}

void EmojiGrid::setImageCache(ImageCache *cache) {
    if (_imgCache == cache)
        return;
    if (_imgCache)
        disconnect(_imgCache, nullptr, this, nullptr);
    _imgCache = cache;
    if (_imgCache) {
        // Repaint when a custom-emoji image finishes downloading. The popup is
        // tiny, so an unconditional viewport update is cheaper than tracking
        // which visible cell the url belongs to.
        connect(_imgCache, &ImageCache::loaded, this, [this](const QString &) {
            viewport()->update();
        });
    }
}

int EmojiGrid::rowCount() const {
    return (count() + kCols - 1) / kCols;
}

int EmojiGrid::contentHeight() const {
    const int rows = rowCount();
    if (rows == 0)
        return 0;
    return kMargin * 2 + rows * kCell + (rows - 1) * kSpacing;
}

QRect EmojiGrid::cellRect(int idx) const {
    const int row = idx / kCols;
    const int col = idx % kCols;
    return QRect(kMargin + col * kRowH, kMargin + row * kRowH, kCell, kCell);
}

int EmojiGrid::cellAt(const QPoint &vp) const {
    const int x = vp.x() - kMargin;
    const int y = vp.y() + verticalScrollBar()->value() - kMargin;
    if (x < 0 || y < 0)
        return -1;
    const int col = x / kRowH;
    const int row = y / kRowH;
    if (col >= kCols)
        return -1;
    // Reject the spacing gutters between cells.
    if (x % kRowH >= kCell || y % kRowH >= kCell)
        return -1;
    const int idx = row * kCols + col;
    return idx < count() ? idx : -1;
}

void EmojiGrid::updateScrollRange() {
    auto     *sb = verticalScrollBar();
    const int vh = viewport()->height();
    sb->setRange(0, std::max(0, contentHeight() - vh));
    sb->setPageStep(vh);
}

void EmojiGrid::ensureVisible(int idx) {
    if (idx < 0 || idx >= count())
        return;
    auto       *sb = verticalScrollBar();
    const QRect cr = cellRect(idx);
    const int   vh = viewport()->height();
    if (cr.top() - kMargin < sb->value())
        sb->setValue(cr.top() - kMargin);
    else if (cr.bottom() + kMargin > sb->value() + vh)
        sb->setValue(cr.bottom() + kMargin - vh);
}

void EmojiGrid::setCells(QVector<Cell> cells) {
    _cells = std::move(cells);
    _hover = -1;
    verticalScrollBar()->setValue(0);
    updateScrollRange();
    // Pre-select the first match so Enter picks the top hit while typing.
    _sel = _cells.isEmpty() ? -1 : 0;
    viewport()->update();
}

void EmojiGrid::setSelected(int idx) {
    if (idx < 0 || idx >= count()) {
        if (_sel != -1) {
            _sel = -1;
            viewport()->update();
        }
        return;
    }
    _sel = idx;
    ensureVisible(_sel);
    viewport()->update();
}

void EmojiGrid::moveSelection(int delta) {
    if (_cells.isEmpty())
        return;
    const int base = _sel < 0 ? 0 : _sel;
    setSelected(std::clamp(base + delta, 0, count() - 1));
}

void EmojiGrid::activateSelected() {
    if (_sel >= 0 && _sel < count())
        emit emojiActivated(_cells[_sel].name);
}

void EmojiGrid::doPaint(QPaintEvent *) {
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), Th::c().surface.raised);
    if (_cells.isEmpty())
        return;

    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();

    // Only the rows intersecting the viewport are painted.
    const int firstRow = std::max(0, (scrollY - kMargin) / kRowH);
    const int lastRow  = (scrollY + vh - kMargin) / kRowH;
    const int first    = firstRow * kCols;
    const int last     = std::min(count() - 1, (lastRow + 1) * kCols - 1);

    const qreal dpr = devicePixelRatioF();

    for (int i = first; i <= last; ++i) {
        const QRect cr = cellRect(i).translated(0, -scrollY);

        if (i == _sel || i == _hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().surface.highlight);
            p.drawRoundedRect(cr, 4, 4);
        }

        const Cell &c = _cells[i];
        if (!c.glyph.isEmpty()) {
            p.setFont(_emojiFont);
            p.setPen(Th::c().text.primary);
            p.drawText(cr, Qt::AlignCenter, c.glyph);
        } else if (!c.imageUrl.isEmpty() && _imgCache) {
            // Lazily fetched — only visible custom emojis ever hit the cache/network.
            const QPixmap px = _imgCache->get(c.imageUrl);
            if (!px.isNull()) {
                constexpr int side   = 22;
                QPixmap       scaled = px.scaled(
                    QSize(side, side) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation
                );
                scaled.setDevicePixelRatio(dpr);
                const QSizeF ls = scaled.deviceIndependentSize();
                const int    x  = cr.x() + qRound((cr.width() - ls.width()) / 2.0);
                const int    y  = cr.y() + qRound((cr.height() - ls.height()) / 2.0);
                p.drawPixmap(QPoint(x, y), scaled);
            }
        }
    }

    paintScrollThumb(p, contentHeight(), QColor(0, 0, 0, 80));
}

void EmojiGrid::doMousePress(QMouseEvent *event) {
    const QPoint pos = event->pos();

    // Scrollbar thumb drag (thin overlay thumb on the right edge).
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (pos.x() >= sbHitX && isOnScrollThumb(pos.y(), contentHeight())) {
        _sbDragging        = true;
        _sbDragStartY      = pos.y();
        _sbDragStartScroll = verticalScrollBar()->value();
        return;
    }

    const int idx = cellAt(pos);
    if (idx >= 0) {
        _sel = idx;
        activateSelected();
    }
}

void EmojiGrid::doMouseMove(QMouseEvent *event) {
    const QPoint pos = event->pos();

    if (_sbDragging) {
        const int vh     = viewport()->height();
        const int totalH = contentHeight();
        const int thumbH = std::max(20, totalH > 0 ? vh * vh / totalH : vh);
        const int denom  = vh - thumbH;
        if (denom > 0) {
            const int newScroll =
                _sbDragStartScroll + (pos.y() - _sbDragStartY) * (totalH - vh) / denom;
            verticalScrollBar()->setValue(std::clamp(newScroll, 0, verticalScrollBar()->maximum()));
        }
        return;
    }

    const int idx = cellAt(pos);
    if (idx != _hover) {
        _hover = idx;
        viewport()->update();
    }
}

void EmojiGrid::doMouseRelease(QMouseEvent *) {
    _sbDragging = false;
}

void EmojiGrid::doMouseLeave() {
    if (_hover != -1) {
        _hover = -1;
        viewport()->update();
    }
}

void EmojiGrid::scrollContentsBy(int, int) {
    // The hovered index was computed against the old scroll offset; drop it so a
    // wheel scroll doesn't leave a highlight stuck on a cell the cursor left.
    _hover = -1;
    viewport()->update();
}

void EmojiGrid::resizeEvent(QResizeEvent *event) {
    VirtualListWidget::resizeEvent(event);
    updateScrollRange();
}

// ── EmojiPickerPopup ───────────────────────────────────────────────────────-─

EmojiPickerPopup::EmojiPickerPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName("emojiPicker");
    setFixedWidth(kFullWidth);
    setFixedHeight(kFullHeight);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    _search = new QLineEdit(this);
    _search->setPlaceholderText(tr("Search emoji…"));
    // Fixed height so the popup's chrome is deterministic when we auto-size it
    // (QLineEdit::sizeHint doesn't reliably account for the stylesheet padding).
    {
        QFont sf = _search->font();
        sf.setPixelSize(Th::c().fonts.md);
        _search->setFont(sf);
        // 4px top/bottom padding + 1px top/bottom border from the stylesheet.
        _search->setFixedHeight(QFontMetrics(sf).height() + 8 + 2);
    }
    lay->addWidget(_search);

    _grid = new EmojiGrid(this);
    lay->addWidget(_grid, 1);

    // Drive grid navigation from keys typed in the search field.
    _search->installEventFilter(this);

    connect(_grid, &EmojiGrid::emojiActivated, this, [this](const QString &name) {
        hide();
        emit emojiSelected(name);
    });

    connect(_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        rebuild(text.trimmed());
    });

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &EmojiPickerPopup::applyTheme
    );

    rebuild();
}

void EmojiPickerPopup::applyTheme() {
    setStyleSheet(QString(
                      "QFrame#emojiPicker {"
                      "  background: %1;"
                      "  border: 1px solid %2;"
                      "  border-radius: 8px;"
                      "}"
                      "QLineEdit {"
                      "  border: 1px solid %2;"
                      "  border-radius: 4px;"
                      "  padding: 4px 8px;"
                      "  font-size: %5px;"
                      "  color: %3;"
                      "  background: %1;"
                      "}"
                      "QLineEdit:focus { border-color: %4; }"
    )
                      .arg(
                          Th::qss(Th::c().surface.raised),
                          Th::qss(Th::c().divider.strong),
                          Th::qss(Th::c().text.primary),
                          Th::qss(Th::c().accent.def)
                      )
                      .arg(Th::c().fonts.md));
}

void EmojiPickerPopup::setSession(Session *session) {
    _session = session;
}

void EmojiPickerPopup::setImageCache(ImageCache *cache) {
    _grid->setImageCache(cache);
}

void EmojiPickerPopup::open(const QPoint &globalPos) {
    _search->clear();
    rebuild();
    move(globalPos);
    show();
    raise();
    _search->setFocus();
}

void EmojiPickerPopup::rebuild(const QString &filter) {
    QVector<EmojiGrid::Cell> cells;

    // Custom emoji from the session — shown as downloaded images, pulled lazily
    // from the shared ImageCache as cells scroll into view.
    if (_session) {
        const auto &emap = _session->emojiMap();
        for (auto it = emap.begin(); it != emap.end(); ++it) {
            const QString &name = it.key();
            const QString &url  = it.value();
            if (url.startsWith("alias:"))
                continue;
            if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
                continue;
            cells.push_back({name, {}, url});
        }
    }

    // Base Unicode emoji. With no filter we show a curated, sensibly-ordered
    // subset (the default browse view); once the user types we search the whole
    // built-in database (matching the composer's ":code" completion), so e.g.
    // "pill" surfaces 💊 even though it's not in the curated list.
    const QStringList &baseNames = filter.isEmpty() ? kBaseEmojiNames : Emoji::allNames();
    for (const QString &name : baseNames) {
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
            continue;
        const QString ch = Emoji::fromName(name);
        if (ch.startsWith(':'))
            continue; // skip unknown names
        cells.push_back({name, ch, {}});
    }

    _grid->setCells(std::move(cells));

    // Auto-size the popup height to the rendered grid (capped at the full
    // height, where the scroll bar takes over) so a short filtered list
    // doesn't leave empty space below the last row.
    const int chrome   = 8 + _search->height() + 6 + 8;
    const int maxGridH = kFullHeight - chrome;
    const int gridH    = std::max(_grid->contentHeight(), 4);
    setFixedHeight(chrome + std::min(gridH, maxGridH));
}

bool EmojiPickerPopup::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _search && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            _grid->activateSelected();
            return true;
        case Qt::Key_Up:
            _grid->moveSelection(-_grid->columns());
            return true;
        case Qt::Key_Down:
            _grid->moveSelection(_grid->columns());
            return true;
        case Qt::Key_Left:
            _grid->moveSelection(-1);
            return true;
        case Qt::Key_Right:
            _grid->moveSelection(1);
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}
