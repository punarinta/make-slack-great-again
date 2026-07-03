// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "emoji_picker_popup.h"
#include "session/session.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/popup_placement.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/emoji.h"
#include "util/emoji_catalog.h"
#include "util/emoji_font.h"
#include "util/emoji_pixmap.h"

#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Popup chrome size — a fixed Slack-like panel; the grid scrolls inside it.
constexpr int kFullWidth  = 354;
constexpr int kFullHeight = 460;

constexpr int kRecentMax = 27; // 3 rows of 9

// Skin-tone modifier glyph for tone 2..6 ("" for 0/default).
QString toneGlyph(int tone) {
    return tone >= 2 && tone <= 6 ? Emoji::fromName(QStringLiteral("skin-tone-%1").arg(tone))
                                  : QString();
}

// Render an emoji glyph to a crisp pixmap (for icon-style use in buttons).
QPixmap glyphPixmap(const QString &glyph, int px, qreal dpr) {
    QImage img(QSize(px, px) * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter p(&img);
    QFont    f = emojiFont(px - 2);
    p.setFont(f);
    p.drawText(QRectF(0, 0, px, px), Qt::AlignCenter, glyph);
    p.end();
    QPixmap out = QPixmap::fromImage(img);
    out.setDevicePixelRatio(dpr);
    return out;
}

} // namespace

// ── EmojiGrid ────────────────────────────────────────────────────────────────

EmojiGrid::EmojiGrid(QWidget *parent) : VirtualListWidget(parent) {
    verticalScrollBar()->setSingleStep(kCell);
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

int EmojiGrid::contentHeight() const {
    return _contentH;
}

QString EmojiGrid::emittedName(const Cell &c) const {
    if (_skinTone && c.skinnable)
        return c.name + QStringLiteral("::skin-tone-%1").arg(_skinTone);
    return c.name;
}

QString EmojiGrid::displayGlyph(const Cell &c) const {
    if (_skinTone && c.skinnable && !c.glyph.isEmpty())
        return c.glyph + _toneGlyph;
    return c.glyph;
}

void EmojiGrid::setSkinTone(int tone) {
    if (_skinTone == tone)
        return;
    _skinTone  = tone;
    _toneGlyph = toneGlyph(tone);
    viewport()->update();
}

void EmojiGrid::relayout() {
    _rows.clear();
    int y = kMargin;
    for (int s = 0; s < _sections.size(); ++s) {
        const Section &sec = _sections[s];
        if (sec.cellCount <= 0)
            continue;
        if (!sec.label.isEmpty()) {
            _rows.push_back({y, kHeaderH, true, sec.label, s, 0, 0});
            y += kHeaderH;
        }
        for (int off = 0; off < sec.cellCount; off += kCols) {
            const int n = std::min(kCols, sec.cellCount - off);
            _rows.push_back({y, kCell, false, {}, s, sec.firstCell + off, n});
            y += kCell;
        }
    }
    _contentH = y + kMargin;
}

void EmojiGrid::setContent(QVector<Cell> cells, QVector<Section> sections) {
    _cells    = std::move(cells);
    _sections = std::move(sections);
    _hover    = -1;
    _sel      = -1;
    relayout();
    verticalScrollBar()->setValue(0);
    updateScrollRange();
    _topSection = -1;
    emitTopSection();
    viewport()->update();
}

int EmojiGrid::rowOfCell(int idx) const {
    for (int r = 0; r < _rows.size(); ++r) {
        const Row &row = _rows[r];
        if (!row.header && idx >= row.cellStart && idx < row.cellStart + row.cellCount)
            return r;
    }
    return -1;
}

QRect EmojiGrid::cellRect(int idx) const {
    const int r = rowOfCell(idx);
    if (r < 0)
        return {};
    const int col = idx - _rows[r].cellStart;
    return QRect(kMargin + col * kCell, _rows[r].y, kCell, kCell);
}

int EmojiGrid::cellAt(const QPoint &vp) const {
    const int yc = vp.y() + verticalScrollBar()->value();
    for (const Row &row : _rows) {
        if (yc < row.y || yc >= row.y + row.h)
            continue;
        if (row.header)
            return -1;
        const int x = vp.x() - kMargin;
        if (x < 0)
            return -1;
        const int col = x / kCell;
        if (col < 0 || col >= row.cellCount)
            return -1;
        return row.cellStart + col;
    }
    return -1;
}

void EmojiGrid::updateScrollRange() {
    auto     *sb = verticalScrollBar();
    const int vh = viewport()->height();
    sb->setRange(0, std::max(0, contentHeight() - vh));
    sb->setPageStep(vh);
}

void EmojiGrid::ensureVisible(int idx) {
    const int r = rowOfCell(idx);
    if (r < 0)
        return;
    auto      *sb  = verticalScrollBar();
    const int  vh  = viewport()->height();
    const Row &row = _rows[r];
    // Reveal the section header too when the cell is in the section's first row.
    int        top = row.y;
    if (r > 0 && _rows[r - 1].header && _rows[r - 1].section == row.section)
        top = _rows[r - 1].y;
    if (top < sb->value())
        sb->setValue(top);
    else if (row.y + row.h > sb->value() + vh)
        sb->setValue(row.y + row.h - vh);
}

void EmojiGrid::scrollToSection(int sectionIdx) {
    for (const Row &row : _rows) {
        if (row.section == sectionIdx) {
            verticalScrollBar()->setValue(
                std::min(row.y - kMargin, verticalScrollBar()->maximum())
            );
            return;
        }
    }
}

void EmojiGrid::emitTopSection() {
    const int scrollY = verticalScrollBar()->value();
    int       sec     = _rows.isEmpty() ? -1 : _rows.front().section;
    for (const Row &row : _rows) {
        if (row.y + row.h > scrollY) {
            sec = row.section;
            break;
        }
    }
    if (sec != _topSection) {
        _topSection = sec;
        emit topSectionChanged(sec);
    }
}

void EmojiGrid::setSelected(int idx) {
    if (idx < 0 || idx >= _cells.size()) {
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

void EmojiGrid::moveSelection(int dCol, int dRow) {
    if (_cells.isEmpty())
        return;
    if (_sel < 0) {
        setSelected(0);
        return;
    }
    if (dCol != 0) { // left/right walks the flat cell list (crosses sections)
        setSelected(std::clamp(_sel + dCol, 0, static_cast<int>(_cells.size()) - 1));
        return;
    }
    // up/down: move to the adjacent emoji row, preserving the column.
    const int r = rowOfCell(_sel);
    if (r < 0)
        return;
    const int col  = _sel - _rows[r].cellStart;
    const int step = dRow > 0 ? 1 : -1;
    for (int rr = r + step; rr >= 0 && rr < _rows.size(); rr += step) {
        if (_rows[rr].header)
            continue;
        const int c = std::min(col, _rows[rr].cellCount - 1);
        setSelected(_rows[rr].cellStart + c);
        return;
    }
}

void EmojiGrid::activateSelected() {
    if (_sel >= 0 && _sel < _cells.size())
        emit emojiActivated(emittedName(_cells[_sel]));
}

void EmojiGrid::doPaint(QPaintEvent *) {
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), Th::c().surface.raised);
    if (_cells.isEmpty())
        return;

    const int   scrollY = verticalScrollBar()->value();
    const int   vh      = viewport()->height();
    const qreal dpr     = devicePixelRatioF();

    QFont headerFont = font();
    headerFont.setPixelSize(Th::c().fonts.sm);
    headerFont.setBold(true);

    for (const Row &row : _rows) {
        const int ry = row.y - scrollY;
        if (ry + row.h <= 0 || ry >= vh)
            continue;

        if (row.header) {
            p.setFont(headerFont);
            p.setPen(Th::c().text.secondary);
            p.drawText(
                QRect(kMargin + 2, ry, viewport()->width() - kMargin * 2, row.h),
                Qt::AlignVCenter | Qt::AlignLeft,
                row.label
            );
            continue;
        }

        for (int col = 0; col < row.cellCount; ++col) {
            const int   idx = row.cellStart + col;
            const QRect cr(kMargin + col * kCell, ry, kCell, kCell);

            if (idx == _sel || idx == _hover) {
                p.setPen(Qt::NoPen);
                p.setBrush(Th::c().surface.highlight);
                p.drawRoundedRect(cr.adjusted(2, 2, -2, -2), 5, 5);
            }

            const Cell &c = _cells[idx];
            if (!c.glyph.isEmpty()) {
                // Cached pixmap — shaping a full grid of color-emoji glyphs per
                // frame decodes a PNG per glyph (see util/emoji_pixmap.h).
                EmojiPix::draw(p, cr, displayGlyph(c), kGlyphPx, Th::c().text.primary);
            } else if (!c.imageUrl.isEmpty() && _imgCache) {
                const QPixmap px = _imgCache->get(c.imageUrl);
                if (!px.isNull()) {
                    constexpr int side   = 24;
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
    }

    paintScrollThumb(p, contentHeight(), Th::c().divider.strong);
}

void EmojiGrid::doMousePress(QMouseEvent *event) {
    const QPoint pos = event->pos();

    const int sbHitX = scrollThumbHitX();
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
    emitTopSection();
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

    _skinTone = QSettings("msga", "msga").value(QStringLiteral("emoji/skinTone"), 0).toInt();
    if (_skinTone != 0 && (_skinTone < 2 || _skinTone > 6))
        _skinTone = 0;

    auto       *lay = new QVBoxLayout(this);
    const auto &sp  = Th::c().spacing;
    lay->setContentsMargins(sp.md, sp.md, sp.md, sp.md);
    lay->setSpacing(sp.md);

    // Category icon bar.
    _catBar      = new QWidget(this);
    auto *catLay = new QHBoxLayout(_catBar);
    catLay->setContentsMargins(0, 0, 0, 0);
    catLay->setSpacing(0);
    lay->addWidget(_catBar);

    // Search field.
    _search = new StyledLineEdit(this);
    _search->setPlaceholderText(tr("Search all emoji"));
    _search->setLeadingIcon(QStringLiteral(":/ui/search.svg"));
    lay->addWidget(_search);

    _grid = new EmojiGrid(this);
    _grid->setSkinTone(_skinTone);
    lay->addWidget(_grid, 1);

    // Bottom bar: skin-tone selector (right-aligned). The expanded swatches are
    // an inline row (not a nested Qt::Popup — those don't honour move() on
    // Wayland, so a popover selector would be unreliable / invisible there).
    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(sp.xs, 0, sp.xs, 0);
    bottom->setSpacing(sp.xs);
    bottom->addStretch(1);
    _toneRow      = new QWidget(this);
    auto *toneLay = new QHBoxLayout(_toneRow);
    toneLay->setContentsMargins(0, 0, 0, 0);
    toneLay->setSpacing(sp.xs);
    _toneRow->hide();
    bottom->addWidget(_toneRow);
    _skinBtn = new QPushButton(tr("Skin Tone"), this);
    _skinBtn->setCursor(Qt::PointingHandCursor);
    _skinBtn->setFlat(true);
    _skinBtn->setIconSize(QSize(18, 18));
    bottom->addWidget(_skinBtn);
    lay->addLayout(bottom);
    buildSkinToneRow();

    _search->lineEdit()->installEventFilter(this);

    connect(_grid, &EmojiGrid::emojiActivated, this, [this](const QString &name) {
        // Record the base (tone-stripped) name for the Frequently Used section.
        const int sep = name.indexOf(QLatin1String("::"));
        recordUse(sep > 0 ? name.left(sep) : name);
        hide();
        emit emojiSelected(name);
    });
    connect(_grid, &EmojiGrid::topSectionChanged, this, &EmojiPickerPopup::setActiveTab);
    connect(_search, &StyledLineEdit::textChanged, this, [this](const QString &text) {
        rebuild(text.trimmed());
    });
    connect(_skinBtn, &QPushButton::clicked, this, &EmojiPickerPopup::toggleSkinToneRow);

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &EmojiPickerPopup::applyTheme
    );

    updateSkinToneButton();
}

void EmojiPickerPopup::applyTheme() {
    // The search field is a StyledLineEdit (self-themed); only the frame, the
    // category QToolButtons and the skin-tone QPushButton are styled here.
    setStyleSheet(QString(
                      "QFrame#emojiPicker {"
                      "  background: %1;"
                      "  border: 1px solid %2;"
                      "  border-radius: 8px;"
                      "}"
                      "QToolButton {"
                      "  border: none; background: transparent; padding: 5px;"
                      "  border-bottom: 2px solid transparent;"
                      "}"
                      "QToolButton:hover { background: %4; border-radius: 4px; }"
                      "QToolButton:checked { border-bottom: 2px solid %3; }"
                      "QPushButton {"
                      "  border: none; background: transparent; color: %5;"
                      "  padding: 3px 6px; font-size: %7px;"
                      "}"
                      "QPushButton:hover { color: %6; }"
    )
                      .arg(
                          Th::qss(Th::c().surface.raised),
                          Th::qss(Th::c().divider.strong),
                          Th::qss(Th::c().accent.def),
                          Th::qss(Th::c().surface.highlight),
                          Th::qss(Th::c().text.secondary),
                          Th::qss(Th::c().text.primary)
                      )
                      .arg(Th::c().fonts.sm));
    buildCategoryBar(); // re-tint category icons
    updateSkinToneButton();
}

void EmojiPickerPopup::setSession(Session *session) {
    _session = session;
}

void EmojiPickerPopup::setImageCache(ImageCache *cache) {
    _grid->setImageCache(cache);
}

void EmojiPickerPopup::open(const QPoint &globalPos) {
    _search->clear();
    _toneRow->hide();
    _skinBtn->show();
    rebuild();
    // Clamp so the panel stays on the screen the anchor is on.
    QPoint pos = globalPos;
    if (QScreen *scr = QGuiApplication::screenAt(globalPos))
        pos = Ui::clampInto(size(), globalPos, scr->availableGeometry());
    move(pos);
    show();
    raise();
    _search->lineEdit()->setFocus();
}

// Build a grid cell for a built-in or custom emoji given its base short-name.
static EmojiGrid::Cell makeBuiltinCell(const QString &name) {
    EmojiGrid::Cell c;
    c.name      = name;
    c.glyph     = Emoji::fromName(name);
    c.skinnable = Emoji::supportsSkinTone(name);
    return c;
}

void EmojiPickerPopup::rebuild(const QString &filter) {
    QVector<EmojiGrid::Cell>    cells;
    QVector<EmojiGrid::Section> sections;
    _tabs.clear();
    _searching = !filter.isEmpty();

    // Resolvable custom (workspace) emoji map, used for both the Custom section
    // and to render recents/search hits that point at workspace emoji.
    const QHash<QString, QString> *custom = _session ? &_session->emojiMap() : nullptr;

    auto addSection = [&](const QString                  &id,
                          const QString                  &label,
                          const QString                  &icon,
                          const QVector<EmojiGrid::Cell> &secCells) {
        if (secCells.isEmpty())
            return;
        EmojiGrid::Section s{
            id, label, static_cast<int>(cells.size()), static_cast<int>(secCells.size())
        };
        sections.push_back(s);
        if (!icon.isEmpty())
            _tabs.push_back({id, icon, static_cast<int>(sections.size() - 1)});
        cells += secCells;
    };

    if (_searching) {
        // Flat "Search Results" section: custom emoji first (workspace-specific),
        // then the whole built-in database (matching the composer's completion).
        QVector<EmojiGrid::Cell> hits;
        if (custom) {
            for (auto it = custom->begin(); it != custom->end(); ++it) {
                if (it.value().startsWith(QLatin1String("alias:")))
                    continue;
                if (it.key().contains(filter, Qt::CaseInsensitive))
                    hits.push_back({it.key(), {}, it.value(), false});
            }
        }
        for (const QString &name : Emoji::allNames()) {
            if (!name.contains(filter, Qt::CaseInsensitive))
                continue;
            const QString g = Emoji::fromName(name);
            if (g.startsWith(':'))
                continue;
            hits.push_back(makeBuiltinCell(name));
        }
        addSection(QStringLiteral("search"), tr("Search Results"), {}, hits);
        _grid->setContent(std::move(cells), std::move(sections));
        _grid->setSelected(0); // Enter picks the top hit
        _catBar->hide();
        return;
    }

    // ── Browse mode ──────────────────────────────────────────────────────────
    // Frequently Used (persisted MRU of base names).
    {
        const QStringList recents =
            QSettings("msga", "msga").value(QStringLiteral("emoji/recent")).toStringList();
        QVector<EmojiGrid::Cell> freq;
        for (const QString &name : recents) {
            const QString g = Emoji::fromName(name);
            if (g.startsWith(':')) {
                // Not a built-in — maybe a workspace custom emoji.
                if (custom) {
                    const auto it = custom->constFind(name);
                    if (it != custom->constEnd() && !it->startsWith(QLatin1String("alias:"))) {
                        freq.push_back({name, {}, *it, false});
                        continue;
                    }
                }
                continue;
            }
            freq.push_back(makeBuiltinCell(name));
        }
        addSection(
            QStringLiteral("frequent"),
            tr("Frequently Used"),
            QStringLiteral(":/ui/clock.svg"),
            freq
        );
    }

    // Standard categories from the cached catalog.
    static const QHash<QString, QString> kCatIcon = {
        {QStringLiteral("people"), QStringLiteral(":/ui/smile.svg")},
        {QStringLiteral("nature"), QStringLiteral(":/ui/leaf.svg")},
        {QStringLiteral("food"), QStringLiteral(":/ui/apple.svg")},
        {QStringLiteral("travel"), QStringLiteral(":/ui/plane.svg")},
        {QStringLiteral("activity"), QStringLiteral(":/ui/volleyball.svg")},
        {QStringLiteral("objects"), QStringLiteral(":/ui/lightbulb.svg")},
        {QStringLiteral("symbols"), QStringLiteral(":/ui/shapes.svg")},
        {QStringLiteral("flags"), QStringLiteral(":/ui/flag.svg")},
    };
    for (const Emoji::Category &cat : Emoji::categories()) {
        QVector<EmojiGrid::Cell> catCells;
        catCells.reserve(cat.names.size());
        for (const QString &name : cat.names)
            catCells.push_back(makeBuiltinCell(name));
        addSection(cat.id, cat.label, kCatIcon.value(cat.id), catCells);
    }

    // Custom (workspace) emoji.
    if (custom) {
        QVector<EmojiGrid::Cell> customCells;
        for (auto it = custom->begin(); it != custom->end(); ++it) {
            if (it.value().startsWith(QLatin1String("alias:")))
                continue;
            customCells.push_back({it.key(), {}, it.value(), false});
        }
        std::sort(customCells.begin(), customCells.end(), [](const auto &a, const auto &b) {
            return a.name < b.name;
        });
        addSection(
            QStringLiteral("custom"),
            tr("Custom"),
            QStringLiteral(":/ui/slack-mark.svg"),
            customCells
        );
    }

    _grid->setContent(std::move(cells), std::move(sections));
    _catBar->show();
    buildCategoryBar();
}

void EmojiPickerPopup::buildCategoryBar() {
    if (!_catBar)
        return;
    auto *lay = static_cast<QHBoxLayout *>(_catBar->layout());
    // Clear existing buttons.
    while (QLayoutItem *item = lay->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    for (int i = 0; i < _tabs.size(); ++i) {
        const Tab &t   = _tabs[i];
        auto      *btn = new QToolButton(_catBar);
        btn->setCheckable(true);
        btn->setAutoExclusive(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setIcon(svgIcon(t.icon, QSize(18, 18), Th::c().icon.def));
        btn->setIconSize(QSize(18, 18));
        const int section = t.section;
        connect(btn, &QToolButton::clicked, this, [this, section] {
            _grid->scrollToSection(section);
        });
        lay->addWidget(btn, 1);
    }
    setActiveTab(_grid->selected() >= 0 ? -1 : 0);
}

void EmojiPickerPopup::setActiveTab(int sectionIdx) {
    if (_searching)
        return;
    auto *lay = static_cast<QHBoxLayout *>(_catBar->layout());
    for (int i = 0; i < _tabs.size() && i < lay->count(); ++i) {
        auto *btn = qobject_cast<QToolButton *>(lay->itemAt(i)->widget());
        if (btn)
            btn->setChecked(_tabs[i].section == sectionIdx);
    }
}

void EmojiPickerPopup::recordUse(const QString &baseName) {
    QSettings   s("msga", "msga");
    QStringList recents = s.value(QStringLiteral("emoji/recent")).toStringList();
    recents.removeAll(baseName);
    recents.prepend(baseName);
    while (recents.size() > kRecentMax)
        recents.removeLast();
    s.setValue(QStringLiteral("emoji/recent"), recents);
}

void EmojiPickerPopup::applySkinTone(int tone) {
    _skinTone = tone;
    QSettings("msga", "msga").setValue(QStringLiteral("emoji/skinTone"), tone);
    _grid->setSkinTone(tone);
    updateSkinToneButton();
}

void EmojiPickerPopup::updateSkinToneButton() {
    if (!_skinBtn)
        return;
    const QString hand = Emoji::fromName(QStringLiteral("hand")) + toneGlyph(_skinTone);
    _skinBtn->setIcon(QIcon(glyphPixmap(hand, 18, devicePixelRatioF())));
}

void EmojiPickerPopup::buildSkinToneRow() {
    auto *l = static_cast<QHBoxLayout *>(_toneRow->layout());
    while (QLayoutItem *item = l->takeAt(0)) {
        if (item->widget())
            delete item->widget();
        delete item;
    }
    const int tones[] = {0, 2, 3, 4, 5, 6}; // 0 = default (yellow)
    for (int tone : tones) {
        const QString glyph = Emoji::fromName(QStringLiteral("hand")) + toneGlyph(tone);
        auto         *b     = new QToolButton(_toneRow);
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(QIcon(glyphPixmap(glyph, 18, devicePixelRatioF())));
        b->setIconSize(QSize(18, 18));
        b->setCheckable(true);
        b->setChecked(tone == _skinTone);
        connect(b, &QToolButton::clicked, this, [this, tone] {
            applySkinTone(tone);
            _toneRow->hide();
            _skinBtn->show();
            buildSkinToneRow(); // refresh checked state for next time
        });
        l->addWidget(b);
    }
}

void EmojiPickerPopup::toggleSkinToneRow() {
    const bool show = !_toneRow->isVisible();
    _toneRow->setVisible(show);
    _skinBtn->setVisible(!show);
}

bool EmojiPickerPopup::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _search->lineEdit() && event->type() == QEvent::KeyPress) {
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
            _grid->moveSelection(0, -1);
            return true;
        case Qt::Key_Down:
            _grid->moveSelection(0, 1);
            return true;
        case Qt::Key_Left:
            if (_search->lineEdit()->cursorPosition() == 0) {
                _grid->moveSelection(-1, 0);
                return true;
            }
            break;
        case Qt::Key_Right:
            if (_search->lineEdit()->cursorPosition() == _search->text().size()) {
                _grid->moveSelection(1, 0);
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}
