// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mention_completer.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSvgRenderer>
#include <QVBoxLayout>

// Hard cap on rows built (relevance/perf); the visible window scrolls within it.
static constexpr int kMaxRows      = 50;
// Rows shown before the list starts scrolling.
static constexpr int kCmdVisible   = 5; // slash commands
static constexpr int kPlainVisible = 8; // users / channels / emoji

namespace {

// ── Slash-command row ────────────────────────────────────────────────────────
// Slack-style two-line row: app/Slack avatar, a bold title with a dimmed
// argument hint, and a "source · description" subtitle. Two highlight states
// mirror the official client — mouse hover paints an accent (blue) fill with
// light text, keyboard selection paints a subtle (gray) fill and reveals an
// "Enter" affordance on the right.
class CommandRow : public QWidget {
public:
    std::function<void()> onClick;

    static constexpr int kRowH = 56;
    static constexpr int kIcon = 36;
    static constexpr int kPadX = 14;
    static constexpr int kGap  = 12;

    CommandRow(const MentionCompleter::Item &item, ImageCache *cache, QWidget *parent)
        : QWidget(parent), _item(item), _cache(cache) {
        setFixedHeight(kRowH);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);

        if (_cache && _item.isApp && !_item.iconUrl.isEmpty()) {
            _cache->get(_item.iconUrl); // kick off the download
            connect(_cache, &ImageCache::loaded, this, [this](const QString &url) {
                if (url == _item.iconUrl)
                    update();
            });
        }
    }

    void setSelected(bool s) {
        if (_selected == s)
            return;
        _selected = s;
        update();
    }

    QSize sizeHint() const override { return {460, kRowH}; }

protected:
    void enterEvent(QEnterEvent *) override {
        _hovered = true;
        update();
    }
    void leaveEvent(QEvent *) override {
        _hovered = false;
        update();
    }
    void mousePressEvent(QMouseEvent *) override {
        if (onClick)
            onClick();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);

        const qreal dpr   = devicePixelRatioF();
        const bool  light = _hovered; // hover → accent fill + light text

        // ── Background ──────────────────────────────────────────────────
        const QRect r = rect();
        if (_hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().text.link);
            p.drawRoundedRect(r, 6, 6);
        } else if (_selected) {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().surface.highlight);
            p.drawRoundedRect(r, 6, 6);
        }

        // ── Avatar ──────────────────────────────────────────────────────
        const QRect iconR(kPadX, (kRowH - kIcon) / 2, kIcon, kIcon);
        paintAvatar(p, iconR, dpr);

        // ── Text columns ────────────────────────────────────────────────
        int textLeft  = iconR.right() + 1 + kGap;
        int textRight = r.right() - kPadX;

        // Reserve room for the "Enter" affordance on the keyboard-selected row.
        QString enterText;
        QRect   enterRect;
        if (_selected && !_hovered) {
            enterText = QCoreApplication::translate("MentionCompleter", "Enter");
            QFont ef  = font();
            ef.setPixelSize(Th::c().fonts.caption);
            const QFontMetrics efm(ef);
            const int          ew = efm.horizontalAdvance(enterText) + 18;
            const int          eh = 22;
            enterRect             = QRect(textRight - ew, (kRowH - eh) / 2, ew, eh);
            textRight             = enterRect.left() - kGap;
            p.setPen(QPen(Th::c().divider.strong, 1));
            p.setBrush(Th::c().surface.raised);
            p.drawRoundedRect(QRectF(enterRect).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
            p.setFont(ef);
            p.setPen(Th::c().text.secondary);
            p.drawText(enterRect, Qt::AlignCenter, enterText);
        }

        const int textW = qMax(0, textRight - textLeft);

        // Title: bold command name, then a dimmed usage hint.
        QFont titleF = font();
        titleF.setPixelSize(Th::c().fonts.base);
        QFont titleBold = titleF;
        titleBold.setBold(true);

        const QColor titleC = light ? Th::c().text.onDark : Th::c().text.primary;
        const QColor dimC   = light ? Th::c().text.onDarkDim : Th::c().text.tertiary;

        drawTwoTone(
            p,
            QRect(textLeft, 10, textW, 20),
            titleBold,
            titleC,
            _item.title,
            titleF,
            dimC,
            _item.usage.isEmpty() ? QString() : QStringLiteral(" ") + _item.usage
        );

        // Subtitle: bold source, then a dimmed description.
        QFont subF = font();
        subF.setPixelSize(Th::c().fonts.caption);
        QFont subBold = subF;
        subBold.setBold(true);

        const QColor srcC  = light ? Th::c().text.onDark : Th::c().text.secondary;
        const QColor descC = light ? Th::c().text.onDarkDim : Th::c().text.tertiary;

        QString source = _item.source;
        if (_item.isApp) {
            const QString app = QCoreApplication::translate("MentionCompleter", "App");
            // "App · Giphy", or just "App" when the app name is unknown.
            source            = source.isEmpty() ? app : app + QStringLiteral(" · ") + source;
        }
        const QString descPart =
            _item.desc.isEmpty() ? QString() : QStringLiteral("  ·  ") + _item.desc;

        drawTwoTone(
            p, QRect(textLeft, 30, textW, 18), subBold, srcC, source, subF, descC, descPart
        );
    }

private:
    // Draws `head` in (headFont, headColor) immediately followed by `tail` in
    // (tailFont, tailColor) on one elided line within `box`.
    static void drawTwoTone(
        QPainter      &p,
        const QRect   &box,
        const QFont   &headFont,
        const QColor  &headColor,
        const QString &head,
        const QFont   &tailFont,
        const QColor  &tailColor,
        const QString &tail
    ) {
        const QFontMetrics hfm(headFont);
        const QString      headE = hfm.elidedText(head, Qt::ElideRight, box.width());
        p.setFont(headFont);
        p.setPen(headColor);
        p.drawText(box, Qt::AlignVCenter | Qt::AlignLeft, headE);
        const int headW = hfm.horizontalAdvance(headE);
        if (tail.isEmpty() || headW >= box.width())
            return;
        const QRect        tailBox = box.adjusted(headW, 0, 0, 0);
        const QFontMetrics tfm(tailFont);
        const QString      tailE = tfm.elidedText(tail, Qt::ElideRight, tailBox.width());
        p.setFont(tailFont);
        p.setPen(tailColor);
        p.drawText(tailBox, Qt::AlignVCenter | Qt::AlignLeft, tailE);
    }

    void paintAvatar(QPainter &p, const QRect &iconR, qreal dpr) {
        // Built-in Slack commands: the multicolor brand mark, drawn as-is.
        if (!_item.isApp) {
            QSvgRenderer r(QStringLiteral(":/ui/slack-mark.svg"));
            if (r.isValid()) {
                const QRect inset = iconR.adjusted(1, 1, -1, -1);
                r.render(&p, QRectF(inset));
            }
            return;
        }

        // App commands: rounded-square icon, falling back to an initial chip.
        const QPixmap px =
            (_cache && !_item.iconUrl.isEmpty()) ? _cache->get(_item.iconUrl) : QPixmap();
        QPainterPath clip;
        clip.addRoundedRect(QRectF(iconR), 8, 8);
        if (!px.isNull()) {
            p.save();
            p.setClipPath(clip);
            QPixmap scaled = px.scaled(
                iconR.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
            );
            scaled.setDevicePixelRatio(dpr);
            p.drawPixmap(iconR, scaled);
            p.restore();
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().presence.away);
            p.drawPath(clip);
            const QString initial =
                _item.source.isEmpty() ? QStringLiteral("A") : _item.source.left(1).toUpper();
            QFont f = font();
            f.setBold(true);
            f.setPixelSize(qRound(iconR.height() * 0.42));
            p.setFont(f);
            p.setPen(Th::c().text.onDark);
            p.drawText(iconR, Qt::AlignCenter, initial);
        }
    }

    MentionCompleter::Item _item;
    ImageCache            *_cache    = nullptr;
    bool                   _selected = false;
    bool                   _hovered  = false;
};

// ── Channel row ──────────────────────────────────────────────────────────────
// Slack-style single line mirroring the @-mention rows: a hashtag (public) or
// padlock (private) icon, then the bold channel name. The selected/hovered row
// paints a subtle gray fill and reveals an "Enter" affordance on the right.
class ChannelRow : public QWidget {
public:
    std::function<void()> onClick;
    std::function<void()> onHover;

    static constexpr int kRowH = 38;
    static constexpr int kIcon = 22;
    static constexpr int kPadX = 12;
    static constexpr int kGap  = 12;

    ChannelRow(const MentionCompleter::Item &item, QWidget *parent) : QWidget(parent), _item(item) {
        setFixedHeight(kRowH);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }

    void setSelected(bool s) {
        if (_selected == s)
            return;
        _selected = s;
        update();
    }

    QSize sizeHint() const override { return {360, kRowH}; }

protected:
    void enterEvent(QEnterEvent *) override {
        if (onHover)
            onHover();
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && onClick)
            onClick();
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);

        const QRect r = rect();

        // ── Selection fill (subtle gray, not the blue accent) ───────────────
        if (_selected) {
            p.setPen(Qt::NoPen);
            p.setBrush(Th::c().surface.highlight);
            p.drawRoundedRect(r, 6, 6);
        }

        // ── Icon (hashtag / padlock) ────────────────────────────────────────
        const QRect   iconR(kPadX, (kRowH - kIcon) / 2, kIcon, kIcon);
        const QString icon = _item.channelPrivate ? QStringLiteral(":/ui/lock.svg")
                                                  : QStringLiteral(":/ui/hash.svg");
        const QPixmap px   = svgPixmap(icon, {kIcon, kIcon}, Th::c().text.primary);
        if (!px.isNull())
            p.drawPixmap(iconR, px);

        int       x         = iconR.right() + 1 + kGap;
        int       textRight = r.right() - kPadX;
        const int midY      = kRowH / 2;

        // ── "Enter" affordance on the selected row (reserved on the right) ──
        if (_selected) {
            const QString enterText = QCoreApplication::translate("MentionCompleter", "Enter");
            QFont         ef        = font();
            ef.setPixelSize(Th::c().fonts.caption);
            const QFontMetrics efm(ef);
            const int          ew = efm.horizontalAdvance(enterText) + 18;
            const int          eh = 22;
            const QRect        enterRect(textRight - ew, midY - eh / 2, ew, eh);
            textRight = enterRect.left() - kGap;
            p.setPen(QPen(Th::c().divider.strong, 1));
            p.setBrush(Th::c().surface.raised);
            p.drawRoundedRect(QRectF(enterRect).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
            p.setFont(ef);
            p.setPen(Th::c().text.secondary);
            p.drawText(enterRect, Qt::AlignCenter, enterText);
        }

        // ── Channel name (bold) ─────────────────────────────────────────────
        QFont nameF = font();
        nameF.setPixelSize(Th::c().fonts.base);
        nameF.setBold(true);
        const QFontMetrics nfm(nameF);
        const QString      nameE = nfm.elidedText(_item.title, Qt::ElideRight, textRight - x);
        p.setFont(nameF);
        p.setPen(Th::c().text.primary);
        p.drawText(QRect(x, 0, textRight - x, kRowH), Qt::AlignVCenter | Qt::AlignLeft, nameE);
    }

private:
    MentionCompleter::Item _item;
    bool                   _selected = false;
};

// Stylesheet for the plain one-line rows (users / channels / emoji).
QString plainRowStyle(bool selected) {
    if (selected)
        return QString(
                   "QPushButton {"
                   "  text-align: left; padding: 4px 10px;"
                   "  font-size: %2px; color: %1;"
                   "  background: %3; border: none; border-radius: 4px;"
                   "}"
        )
            .arg(Th::qss(Th::c().text.primary))
            .arg(Th::c().fonts.md)
            .arg(Th::qss(Th::c().accent.subtleBg));
    return QString(
               "QPushButton {"
               "  text-align: left; padding: 4px 10px;"
               "  font-size: %3px; color: %1;"
               "  background: transparent; border: none; border-radius: 4px;"
               "}"
               "QPushButton:hover { background: %2; }"
    )
        .arg(Th::qss(Th::c().text.primary), Th::qss(Th::c().surface.highlight))
        .arg(Th::c().fonts.md);
}

} // namespace

MentionCompleter::MentionCompleter(QWidget *parent) : QFrame(parent) {
    // Plain child widget of the container, like MentionPopup — a real window
    // (Qt::Tool) cannot be positioned by the client on Wayland and is
    // activated on show by some window managers, which steals the editor's
    // focus and instantly dismisses the completer via FocusOut.
    setObjectName("mentionCompleter");
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(0);

    _scroll = new QScrollArea(this);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scroll->setWidgetResizable(true);

    _content = new QWidget(_scroll);
    _content->setStyleSheet("background: transparent;");
    _layout = new QVBoxLayout(_content);
    _layout->setContentsMargins(0, 0, 0, 0);
    _layout->setSpacing(1);
    _scroll->setWidget(_content);
    outer->addWidget(_scroll);

    hide();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void MentionCompleter::applyTheme() {
    setStyleSheet(QString(
                      "QFrame#mentionCompleter {"
                      "  background: %1;"
                      "  border: 1px solid %2;"
                      "  border-radius: 6px;"
                      "}"
                      "QScrollArea { background: transparent; border: none; }"
                      // Thin rounded scrollbar, matching the conversation list.
                      "QScrollBar:vertical {"
                      "  background: transparent; width: 8px; margin: 2px;"
                      "}"
                      "QScrollBar::handle:vertical {"
                      "  background: %3; border-radius: 3px; min-height: 24px;"
                      "}"
                      "QScrollBar::add-line:vertical,"
                      "QScrollBar::sub-line:vertical { height: 0; }"
                      "QScrollBar::add-page:vertical,"
                      "QScrollBar::sub-page:vertical { background: transparent; }"
    )
                      .arg(
                          Th::qss(Th::c().surface.raised),
                          Th::qss(Th::c().divider.strong),
                          Th::qss(Th::c().divider.strong)
                      ));
}

void MentionCompleter::show(const QPoint &globalPos, const QList<Item> &items, Callback cb) {
    _cb  = std::move(cb);
    _sel = 0;
    rebuild(items);

    // Sizes are stale until the first show — drive them from the layout.
    _layout->activate();
    _content->adjustSize();

    const bool commands   = !_items.isEmpty() && _items.front().command;
    const int  maxVisible = commands ? kCmdVisible : kPlainVisible;
    const int  spacing    = _layout->spacing();
    const int  n          = _rows.size();
    const bool scroll     = n > maxVisible;

    // Cap the viewport to maxVisible rows; the rest scrolls within.
    int viewportH = _content->sizeHint().height();
    if (scroll) {
        int h = 0;
        for (int i = 0; i < maxVisible; ++i)
            h += _rows[i]->sizeHint().height();
        viewportH = h + spacing * (maxVisible - 1);
    }
    _scroll->setFixedHeight(viewportH);

    const int margins = 8; // outer 4px each side
    int       w       = _content->sizeHint().width() + margins;
    if (scroll)
        w += 10; // leave room for the scrollbar so text isn't clipped
    const int h = viewportH + margins;
    resize(w, h);

    // Bottom edge sits just above the anchor (the trigger character), in the
    // parent's coordinates; keep the popup inside the parent horizontally.
    QPoint pos = globalPos - QPoint(0, h + 4);
    if (QWidget *par = parentWidget()) {
        const QPoint local = par->mapFromGlobal(globalPos);
        pos                = local - QPoint(0, h + 4);
        pos.setX(qBound(0, pos.x(), qMax(0, par->width() - w)));
    }
    move(pos);
    QFrame::show();
    raise();
}

void MentionCompleter::dismiss() {
    hide();
    _items.clear();
    _rows.clear();
}

bool MentionCompleter::handleKey(int key) {
    if (!isVisible())
        return false;
    if (key == Qt::Key_Escape) {
        dismiss();
        return true;
    }
    if (_rows.isEmpty())
        return false;
    if (key == Qt::Key_Up) {
        selectRow((_sel - 1 + _rows.size()) % _rows.size());
        return true;
    }
    if (key == Qt::Key_Down) {
        selectRow((_sel + 1) % _rows.size());
        return true;
    }
    if (key == Qt::Key_Tab || key == Qt::Key_Return) {
        confirm();
        return true;
    }
    return false;
}

bool MentionCompleter::isVisible() const {
    return QFrame::isVisible();
}

void MentionCompleter::rebuild(const QList<Item> &items) {
    // Clear
    while (_layout->count())
        delete _layout->takeAt(0)->widget();
    _rows.clear();
    _items = items.mid(0, kMaxRows);

    for (int i = 0; i < _items.size(); ++i) {
        const int idx = i;
        if (_items[i].channel) {
            auto *row    = new ChannelRow(_items[i], _content);
            row->onClick = [this, idx] {
                _sel = idx;
                confirm();
            };
            row->onHover = [this, idx] { selectRow(idx); };
            _layout->addWidget(row);
            row->show();
            _rows.append(row);
            continue;
        }
        if (_items[i].command) {
            auto *row    = new CommandRow(_items[i], _imgCache, _content);
            row->onClick = [this, idx] {
                _sel = idx;
                confirm();
            };
            _layout->addWidget(row);
            row->show();
            _rows.append(row);
            continue;
        }

        auto *row = new QPushButton(_content);
        row->setFlat(true);
        row->setFocusPolicy(Qt::NoFocus);
        row->setCursor(Qt::PointingHandCursor);
        row->setText(_items[i].display);
        row->setStyleSheet(plainRowStyle(false));
        connect(row, &QPushButton::clicked, this, [this, idx] {
            _sel = idx;
            confirm();
        });
        _layout->addWidget(row);
        // Children added to an already-visible parent stay hidden until shown
        // explicitly; without this the popup collapses on rebuild-while-open.
        row->show();
        _rows.append(row);
    }

    selectRow(0);
}

void MentionCompleter::selectRow(int row) {
    if (_rows.isEmpty())
        return;
    const auto style = [this](int i, bool selected) {
        if (i < 0 || i >= _rows.size())
            return;
        if (_items[i].channel)
            static_cast<ChannelRow *>(_rows[i])->setSelected(selected);
        else if (_items[i].command)
            static_cast<CommandRow *>(_rows[i])->setSelected(selected);
        else
            static_cast<QPushButton *>(_rows[i])->setStyleSheet(plainRowStyle(selected));
    };
    style(_sel, false);
    _sel = row;
    style(_sel, true);
    if (_sel >= 0 && _sel < _rows.size())
        _scroll->ensureWidgetVisible(_rows[_sel], 0, 0);
}

void MentionCompleter::confirm() {
    if (_sel < 0 || _sel >= _items.size()) {
        dismiss();
        return;
    }
    const QString insert = _items[_sel].insert;
    dismiss();
    if (_cb)
        _cb(insert);
}
