// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mention_popup.h"
#include "session/session.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/paint_utils.h"
#include "ui/pick_row_paint.h"
#include "ui/popup_placement.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
constexpr int kRowH        = 38;
constexpr int kMaxVisible  = 8;
constexpr int kWidth       = 360;
// Wider in threads: the alias rows there carry the "Disabled in threads"
// notice next to the description and the Enter badge.
constexpr int kThreadWidth = 560;
constexpr int kMargins     = 4;

constexpr int kAvatar = 26; // avatar / alias-icon box
constexpr int kPadX   = 8;
constexpr int kGap    = 8; // avatar → text

enum class Presence { None, Online, Away, Dnd };

struct RowData {
    bool     isAlias = false;
    QString  name;     // bold primary label
    QString  subtitle; // dimmed secondary (alias description / user real name)
    QString  status;   // broadcast availability, reserved ahead of the subtitle
    bool     isBot    = false;
    Presence presence = Presence::None;
    QString  avatarUrl;
};

struct AliasInfo {
    const char *name;
    const char *insert;
    const char *desc;
};
constexpr AliasInfo kAliases[] = {
    {"@channel",
     "<!channel>",
     QT_TRANSLATE_NOOP("MentionPopup", "Notify everyone in this channel")},
    {"@everyone",
     "<!everyone>",
     QT_TRANSLATE_NOOP("MentionPopup", "Notify everyone in your workspace")},
    {"@here", "<!here>", QT_TRANSLATE_NOOP("MentionPopup", "Notify every online member here")},
};
} // namespace

// ── Internal row widget — custom-painted, no Q_OBJECT, click via std::function ─
// Slack-style single line: avatar (or megaphone for aliases), bold name, an
// optional APP badge, a presence dot, then a dimmed subtitle. The keyboard /
// hover selected row paints a subtle fill and reveals an "Enter" affordance.

class MentionRow : public QWidget {
public:
    std::function<void()>                onClick;
    std::function<void()>                onHover;
    std::function<void(const QString &)> onTip; // subtitle didn't fit: show it whole
    std::function<void()>                onTipHide;

    MentionRow(const RowData &data, ImageCache *cache, QWidget *parent)
        : QWidget(parent), _data(data), _cache(cache) {
        setFixedHeight(kRowH);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setAccessibleName(_data.name);
        setAccessibleDescription(_data.status.isEmpty() ? _data.subtitle : _data.status);

        if (_cache && !_data.isAlias && !_data.avatarUrl.isEmpty()) {
            _cache->get(_data.avatarUrl); // kick off the download
            connect(_cache, &ImageCache::loaded, this, [this](const QString &url) {
                if (url == _data.avatarUrl)
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

protected:
    void enterEvent(QEnterEvent *) override {
        if (onHover)
            onHover();
        // Selection adds the Enter badge, which may be what elides the subtitle —
        // paint synchronously so the decision sees the hovered layout.
        repaint();
        if (_subtitleClipped && onTip)
            onTip(_data.subtitle);
    }

    void leaveEvent(QEvent *) override {
        if (onTipHide)
            onTipHide();
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

        const qreal dpr = devicePixelRatioF();
        const QRect r   = rect();

        // ── Selection fill (subtle gray, dark text — not the blue accent) ──
        if (_selected)
            Paint::rowHighlight(p, r, Th::c().surface.highlight);

        // ── Avatar / alias icon ─────────────────────────────────────────────
        const QRect iconR(kPadX, (kRowH - kAvatar) / 2, kAvatar, kAvatar);
        if (_data.isAlias)
            paintAliasIcon(p, iconR);
        else
            paintAvatar(p, iconR, dpr);

        int       x         = iconR.right() + 1 + kGap;
        int       textRight = r.right() - kPadX;
        const int midY      = kRowH / 2;

        // ── "Enter" affordance on the selected row (reserved on the right) ──
        if (_selected)
            textRight = PickRow::paintEnterBadge(
                p,
                font(),
                QCoreApplication::translate("MentionPopup", "Enter"),
                textRight,
                kRowH,
                kGap
            );

        // Keep the thread restriction visible even when the description is elided.
        if (!_data.status.isEmpty()) {
            QFont sf = font();
            sf.setPixelSize(Th::c().fonts.caption);
            const int   sw = QFontMetrics(sf).horizontalAdvance(_data.status);
            const QRect statusRect(textRight - sw, 0, sw, kRowH);
            p.setFont(sf);
            p.setPen(Th::c().text.secondary);
            p.drawText(statusRect, Qt::AlignVCenter | Qt::AlignLeft, _data.status);
            textRight = statusRect.left() - kGap;
        }

        // ── Name (bold) ─────────────────────────────────────────────────────
        x += PickRow::paintBoldName(p, font(), _data.name, x, textRight, kRowH);

        // ── APP badge (bots) ────────────────────────────────────────────────
        if (_data.isBot && x < textRight) {
            const QString app = QCoreApplication::translate("MentionPopup", "APP");
            QFont         bf  = font();
            bf.setPixelSize(Th::c().fonts.xs);
            bf.setBold(true);
            const QFontMetrics bfm(bf);
            const int          bw = bfm.horizontalAdvance(app) + 10;
            const int          bh = 15;
            const QRect        badge(x + 6, midY - bh / 2, bw, bh);
            if (badge.right() <= textRight) {
                p.setPen(Qt::NoPen);
                p.setBrush(Th::c().surface.highlightStrong);
                p.drawRoundedRect(badge, 3, 3);
                p.setFont(bf);
                p.setPen(Th::c().text.secondary);
                p.drawText(badge, Qt::AlignCenter, app);
                x = badge.right();
            }
        }

        // ── Presence dot (right after the name) ─────────────────────────────
        if (_data.presence != Presence::None && x + 10 < textRight) {
            x += 8;
            constexpr int d = 9;
            const QRect   dot(x, midY - d / 2, d, d);
            p.setBrush(Qt::NoBrush);
            p.setPen(Qt::NoPen);
            if (_data.presence == Presence::Online) {
                p.setBrush(Th::c().presence.online);
                p.drawEllipse(dot);
            } else if (_data.presence == Presence::Dnd) {
                p.setBrush(Th::c().divider.def);
                p.drawEllipse(dot);
                p.setPen(QPen(Th::c().presence.away, 1.5, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(
                    dot.center().x() - 2, dot.center().y(), dot.center().x() + 2, dot.center().y()
                );
            } else { // Away — hollow ring
                p.setPen(QPen(Th::c().text.tertiary, 1.5));
                p.drawEllipse(QRectF(dot).adjusted(0.75, 0.75, -0.75, -0.75));
            }
            x += d;
        }

        // ── Subtitle (dimmed) ───────────────────────────────────────────────
        _subtitleClipped = false;
        if (!_data.subtitle.isEmpty()) {
            x += kGap;
            _subtitleClipped = x >= textRight;
            if (x < textRight) {
                QFont sf = font();
                sf.setPixelSize(Th::c().fonts.base);
                const QFontMetrics sfm(sf);
                const QString sub = sfm.elidedText(_data.subtitle, Qt::ElideRight, textRight - x);
                _subtitleClipped  = sub != _data.subtitle;
                p.setFont(sf);
                // Alias descriptions read darker in Slack; user real names are dimmer.
                p.setPen(_data.isAlias ? Th::c().text.secondary : Th::c().text.tertiary);
                p.drawText(
                    QRect(x, 0, textRight - x, kRowH), Qt::AlignVCenter | Qt::AlignLeft, sub
                );
            }
        }
    }

private:
    void paintAliasIcon(QPainter &p, const QRect &iconR) {
        constexpr int kGlyph = 20;
        const QRect   g(
            iconR.center().x() - kGlyph / 2, iconR.center().y() - kGlyph / 2, kGlyph, kGlyph
        );
        const QPixmap px = svgPixmap(
            QStringLiteral(":/ui/megaphone.svg"), {kGlyph, kGlyph}, Th::c().text.secondary
        );
        if (!px.isNull())
            p.drawPixmap(g, px);
    }

    void paintAvatar(QPainter &p, const QRect &iconR, qreal dpr) {
        const QPixmap px =
            (_cache && !_data.avatarUrl.isEmpty()) ? _cache->get(_data.avatarUrl) : QPixmap();
        const QString initial =
            _data.name.isEmpty() ? QString() : QString(_data.name).remove('@').left(1).toUpper();
        PickRow::paintIcon(p, iconR, px, dpr, 6, font(), initial);
    }

    RowData     _data;
    ImageCache *_cache           = nullptr;
    bool        _selected        = false;
    bool        _subtitleClipped = false; // last paint elided or skipped the subtitle
};

// ── MentionPopup ──────────────────────────────────────────────────────────────

MentionPopup::MentionPopup(QWidget *parent) : QFrame(parent) {
    // Plain child widget of the container — no separate window, no focus events.
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("mentionPopup");
    setFixedWidth(kWidth);
    _tooltip = new PopupTooltip(this);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kMargins, kMargins, kMargins, kMargins);
    outer->setSpacing(0);

    _scroll = new QScrollArea(this);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    _content = new QWidget(_scroll);
    _content->setStyleSheet("background:transparent;");

    _vbox = new QVBoxLayout(_content);
    _vbox->setContentsMargins(0, 0, 0, 0);
    _vbox->setSpacing(1);

    _scroll->setWidget(_content);
    _scroll->setWidgetResizable(true);
    outer->addWidget(_scroll);

    hide();

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &MentionPopup::applyTheme
    );
}

void MentionPopup::applyTheme() {
    setStyleSheet(
        QString(
            "QFrame#mentionPopup {"
            "  background:%1;"
            "  border:1px solid %2;"
            "  border-radius:6px;"
            "}"
            "QScrollArea { background: transparent; border: none; }"
        )
            .arg(Th::qss(Th::c().surface.raised), Th::qss(Th::c().divider.strong)) +
        Th::popupScrollBarQss()
    );
}

void MentionPopup::setSession(Session *s) {
    _session = s;
}

void MentionPopup::open(const QPoint &anchor, const QString &query, bool isDm, bool isThread) {
    rebuild(query, isDm, isThread);
    if (_rows.isEmpty()) {
        hide();
        return;
    }

    const int visible = qMin(_rows.size(), kMaxVisible);
    // height = visible rows + spacing between them + top/bottom margins
    setFixedHeight(visible * kRowH + (visible - 1) * 1 + 2 * kMargins);

    // Re-anchor on every call: place the popup so its bottom edge sits just
    // above the '@' that triggered it. When filtering shrinks the list the
    // popup must shrink toward the anchor instead of leaving a gap above it.
    QWidget *par = parentWidget();
    setFixedWidth(qMin(isThread && !isDm ? kThreadWidth : kWidth, par->width()));
    const QPoint local = par->mapFromGlobal(anchor);
    // Bottom edge just above the '@'; flip below + clamp within the parent if
    // there's no room above (previously y was never clamped → ran off-screen).
    const QRect  bounds(0, 0, par->width(), par->height());
    const QPoint pos = Ui::placePopup(
        QRect(local, QSize(0, 0)), size(), bounds, Ui::Edge::Above, 4, Ui::Align::Start
    );
    move(pos);

    if (!isVisible()) {
        show();
        raise();
    }
}

void MentionPopup::dismiss() {
    hide();
    _tooltip->hide();
    // Do NOT delete rows here: this may be called from inside a MentionRow's
    // own mousePressEvent (via onClick → confirm → dismiss), and deleting the
    // widget while its event handler is on the call stack is undefined behavior.
    // rebuild() cleans up old rows at the start of the next open().
}

bool MentionPopup::handleKey(int key) {
    if (!isVisible() || _rows.isEmpty())
        return false;
    switch (key) {
    case Qt::Key_Escape:
        dismiss();
        return true;
    case Qt::Key_Up:
        selectRow((_sel - 1 + _rows.size()) % _rows.size());
        return true;
    case Qt::Key_Down:
        selectRow((_sel + 1) % _rows.size());
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Return:
        confirm();
        return true;
    default:
        return false;
    }
}

void MentionPopup::rebuild(const QString &query, bool isDm, bool isThread) {
    _tooltip->hide(); // its row is about to go away
    while (_vbox->count())
        delete _vbox->takeAt(0)->widget();
    _rows.clear();
    _displays.clear();
    _inserts.clear();
    _sel = 0;

    const QString q = query.toLower();

    // `display` is the text written into the composer for the mention; it omits
    // adornments shown only in the row (the "(you)" suffix).
    auto addRow = [&](const RowData &data, const QString &display, const QString &insert) {
        auto     *row = new MentionRow(data, _imgCache, _content);
        const int idx = _rows.size();
        row->onClick  = [this, idx] {
            _sel = idx;
            confirm();
        };
        row->onHover = [this, idx] { selectRow(idx); };
        row->onTip   = [this, row](const QString &text) {
            _tooltip->showAbove(text, QRect(row->mapToGlobal(QPoint(0, 0)), row->size()));
        };
        row->onTipHide = [this] { _tooltip->hide(); };
        _vbox->addWidget(row);
        _rows.append(row);
        _displays.append(display);
        _inserts.append(insert);
    };

    // Aliases are only available in channels.
    if (!isDm) {
        for (const auto &a : kAliases) {
            const QString name(a.name);
            if (q.isEmpty() || name.contains(q, Qt::CaseInsensitive)) {
                RowData d;
                d.isAlias  = true;
                d.name     = name;
                d.subtitle = tr(a.desc);
                if (isThread)
                    d.status = tr("Disabled in threads");
                addRow(d, name, isThread ? name : QLatin1String(a.insert));
            }
        }
    }

    // Users from session
    if (_session) {
        const auto   &users = _session->currentUsers();
        const QString me    = _session->meUserId().value;
        int           added = 0;
        for (const auto &u : users) {
            if (u.isDeactivated)
                continue;
            const QString disp = u.displayLabel();
            if (!q.isEmpty() && !disp.contains(q, Qt::CaseInsensitive) &&
                !u.name.contains(q, Qt::CaseInsensitive))
                continue;

            RowData       d;
            const QString display = "@" + disp;
            d.name                = display;
            if (!me.isEmpty() && u.id.value == me)
                d.name += " " + tr("(you)");
            // Subtitle: the account/username when it differs from the shown label.
            if (!u.name.isEmpty() && !u.name.contains(disp, Qt::CaseInsensitive) &&
                u.name.compare(disp, Qt::CaseInsensitive) != 0)
                d.subtitle = u.name;
            d.isBot     = u.isBot;
            d.avatarUrl = u.avatarUrl;
            d.presence  = u.dndEnabled ? Presence::Dnd
                          : u.isActive ? Presence::Online
                                       : Presence::Away;
            // Bots/apps have no meaningful away state — show them as online.
            if (u.isBot && !u.isActive && !u.dndEnabled)
                d.presence = Presence::Online;

            addRow(d, display, "<@" + u.id.value + ">");
            if (++added >= 50)
                break;
        }
    }

    if (!_rows.isEmpty())
        static_cast<MentionRow *>(_rows[0])->setSelected(true);
}

void MentionPopup::selectRow(int idx) {
    if (_rows.isEmpty())
        return;
    if (_sel >= 0 && _sel < _rows.size())
        static_cast<MentionRow *>(_rows[_sel])->setSelected(false);
    _sel = qBound(0, idx, _rows.size() - 1);
    static_cast<MentionRow *>(_rows[_sel])->setSelected(true);
    _scroll->ensureWidgetVisible(_rows[_sel]);
}

void MentionPopup::confirm() {
    if (_sel < 0 || _sel >= _inserts.size()) {
        dismiss();
        return;
    }
    const QString display = _displays[_sel];
    const QString text    = _inserts[_sel];
    dismiss();
    emit selected(display, text);
}
