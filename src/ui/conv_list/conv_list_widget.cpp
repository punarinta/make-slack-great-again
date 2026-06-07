// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "conv_list_widget.h"
#include "ui/theme.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/user_avatar.h"
#include "ui/message_list/message_render.h"
#include "util/emoji_font.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QFontMetrics>
#include <QApplication>

ConvListWidget::ConvListWidget(ImageCache *imgCache, QWidget *parent)
    : VirtualListWidget(parent)
    , _imgCache(imgCache)
{
    viewport()->setCursor(Qt::PointingHandCursor);

    _selAnim.setDuration(140);
    _selAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&_selAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        _selT = v.toDouble();
        viewport()->update();
    });
    connect(&_selAnim, &QVariantAnimation::finished, this, [this] {
        _selT    = 1.0;
        _selFrom = -1;
    });

    if (_imgCache) {
        connect(_imgCache, &ImageCache::loaded, this, [this]{ viewport()->update(); });
    }
}

// Returns true if s looks like a raw Slack user ID (e.g. "U0A1B2C3D").
static bool isRawSlackId(const QString &s) {
    if (s.length() < 9) return false;
    if (s[0] != 'U' && s[0] != 'W') return false;
    for (int i = 1; i < s.length(); ++i) {
        const QChar c = s[i];
        if (!c.isDigit() && !(c >= 'A' && c <= 'Z')) return false;
    }
    return true;
}

void ConvListWidget::setConversations(std::vector<Conversation> convs) {
    _allConvs = std::move(convs);
    rebuildFilteredConvs();
}

void ConvListWidget::selectRow(int row) {
    setSelected(row);
}

void ConvListWidget::setUsers(const std::vector<User> &users) {
    _userInfos.clear();
    _userInfos.reserve(users.size());
    for (const auto &u : users) {
        _userInfos.insert(u.id.value, {
            .displayName  = u.displayName.isEmpty() ? u.name : u.displayName,
            .avatarUrl    = u.avatarUrl,
            .isDeactivated= u.isDeactivated,
            .isActive     = u.isActive,
            .dndEnabled   = u.dndEnabled,
            .statusEmoji  = u.statusEmoji,
        });
    }
    rebuildFilteredConvs();
}

void ConvListWidget::rebuildFilteredConvs() {
    _convs.clear();
    for (const auto &conv : _allConvs) {
        if ((conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim) && conv.dmUser) {
            const auto it = _userInfos.constFind(conv.dmUser->value);
            if (it != _userInfos.constEnd()) {
                if (it->isDeactivated) continue;
                if (it->displayName == "deactivateduser") continue;
                if (isRawSlackId(it->displayName)) continue;
            } else if (isRawSlackId(conv.name)) {
                continue;
            }
        }
        _convs.push_back(conv);
    }
    rebuildRows();
}

void ConvListWidget::rebuildRows() {
    _rows.clear();

    // ── Channels section ─────────────────────────────────────────────
    _rows.push_back({RowKind::SectionHeader, -1, 0});
    if (!_channelsCollapsed) {
        for (int i = 0; i < (int)_convs.size(); ++i) {
            const ConvKind k = _convs[i].kind;
            if (k == ConvKind::Im || k == ConvKind::Mpim) continue;
            _rows.push_back({RowKind::Conv, i, -1});
        }
        _rows.push_back({RowKind::AddChannels, -1, 0});
    }

    // ── Direct messages section ───────────────────────────────────────
    _rows.push_back({RowKind::SectionHeader, -1, 1});
    if (!_dmsCollapsed) {
        for (int i = 0; i < (int)_convs.size(); ++i) {
            const ConvKind k = _convs[i].kind;
            if (k != ConvKind::Im && k != ConvKind::Mpim) continue;
            _rows.push_back({RowKind::Conv, i, -1});
        }
    }

    // Re-map selection to new visual row indices.
    _selAnim.stop();
    _selT    = 1.0;
    _selFrom = -1;
    _selected = -1;
    if (!_selectedId.value.isEmpty()) {
        for (int r = 0; r < (int)_rows.size(); ++r) {
            if (_rows[r].kind == RowKind::Conv
                    && _convs[_rows[r].convIdx].id == _selectedId) {
                _selected = r;
                break;
            }
        }
    }
    _hovered = -1;
    updateScrollRange();
    viewport()->update();
}

ConversationId ConvListWidget::conversationId(int row) const {
    if (row < 0 || row >= (int)_rows.size()) return {};
    const auto &ri = _rows[row];
    if (ri.kind != RowKind::Conv) return {};
    return _convs[ri.convIdx].id;
}

int ConvListWidget::rowForId(ConversationId id) const {
    for (int r = 0; r < (int)_rows.size(); ++r) {
        const auto &ri = _rows[r];
        if (ri.kind == RowKind::Conv && _convs[ri.convIdx].id == id) return r;
    }
    return -1;
}

QString ConvListWidget::resolvedName(int row) const {
    if (row < 0 || row >= (int)_rows.size()) return {};
    const auto &ri = _rows[row];
    if (ri.kind != RowKind::Conv) return {};
    const auto &conv = _convs[ri.convIdx];
    if ((conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim) && conv.dmUser) {
        const auto it = _userInfos.constFind(conv.dmUser->value);
        if (it != _userInfos.constEnd() && !it->displayName.isEmpty())
            return it->displayName;
    }
    return conv.name;
}

// ── Events ────────────────────────────────────────────────────────────────────

void ConvListWidget::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    updateScrollRange();
}

void ConvListWidget::wheelEvent(QWheelEvent *event) {
    auto *vsb = verticalScrollBar();
    const QPoint px = event->pixelDelta();
    if (!px.isNull()) {
        vsb->setValue(vsb->value() - px.y());
    } else {
        const int steps  = event->angleDelta().y();
        const int pixels = steps * QApplication::wheelScrollLines() * kRowH / 120;
        vsb->setValue(vsb->value() - pixels);
    }
    event->accept();
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

int ConvListWidget::rowAt(int viewportY) const {
    const int docY = viewportY + verticalScrollBar()->value();
    const int row  = docY / kRowH;
    if (row < 0 || row >= static_cast<int>(_rows.size())) return -1;
    return row;
}

void ConvListWidget::setHovered(int row) {
    if (row == _hovered) return;
    _hovered = row;
    viewport()->update();
}

void ConvListWidget::setSelected(int row) {
    if (row < 0 || row >= (int)_rows.size()) return;
    if (_rows[row].kind != RowKind::Conv) return;
    if (row == _selected) return;
    _selFrom  = _selected;
    _selT     = 0.0;
    _selected = row;
    _selectedId = _convs[_rows[row].convIdx].id;
    _selAnim.stop();
    _selAnim.setStartValue(0.0);
    _selAnim.setEndValue(1.0);
    _selAnim.start();
    emit conversationSelected(row);
}

void ConvListWidget::doMouseMove(QMouseEvent *e) {
    const int total = static_cast<int>(_rows.size()) * kRowH;
    if (_sbDragging) {
        const int vh         = viewport()->height();
        const int thumbH     = std::max(20, vh * vh / total);
        const int trackRange = vh - thumbH;
        if (trackRange > 0) {
            const int newScroll = _sbDragStartScroll
                + (e->pos().y() - _sbDragStartY) * (total - vh) / trackRange;
            verticalScrollBar()->setValue(
                std::clamp(newScroll, 0, verticalScrollBar()->maximum()));
        }
        return;
    }
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (e->pos().x() >= sbHitX && VirtualListWidget::isOnScrollThumb(e->pos().y(), total))
        viewport()->setCursor(Qt::SizeVerCursor);
    else
        viewport()->setCursor(Qt::PointingHandCursor);
    setHovered(rowAt(e->pos().y()));
}

void ConvListWidget::doMousePress(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int total  = static_cast<int>(_rows.size()) * kRowH;
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (e->pos().x() >= sbHitX && VirtualListWidget::isOnScrollThumb(e->pos().y(), total)) {
        _sbDragging        = true;
        _sbDragStartY      = e->pos().y();
        _sbDragStartScroll = verticalScrollBar()->value();
        viewport()->setCursor(Qt::SizeVerCursor);
        return;
    }
    const int row = rowAt(e->pos().y());
    if (row < 0) return;
    const auto &ri = _rows[row];
    switch (ri.kind) {
    case RowKind::SectionHeader:
        if (ri.sectionId == 0)
            _channelsCollapsed = !_channelsCollapsed;
        else
            _dmsCollapsed = !_dmsCollapsed;
        rebuildRows();
        break;
    case RowKind::Conv:
        setSelected(row);
        break;
    case RowKind::AddChannels:
        emit addChannelsClicked();
        break;
    }
}

void ConvListWidget::doMouseRelease(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    if (_sbDragging) {
        _sbDragging = false;
        viewport()->setCursor(Qt::PointingHandCursor);
    }
}

void ConvListWidget::doMouseLeave() {
    setHovered(-1);
}

// ── Layout ────────────────────────────────────────────────────────────────────

void ConvListWidget::updateScrollRange() {
    const int total = static_cast<int>(_rows.size()) * kRowH;
    const int vh    = viewport()->height();
    verticalScrollBar()->setRange(0, std::max(0, total - vh));
    verticalScrollBar()->setPageStep(vh);
}

// ── Avatar downloads ──────────────────────────────────────────────────────────

void ConvListWidget::triggerMissingAvatarDownloads() {
    if (!_imgCache) return;
    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();
    const int first   = scrollY / kRowH;
    const int last    = std::min((int)_rows.size() - 1, (scrollY + vh) / kRowH);

    for (int r = first; r <= last; ++r) {
        const auto &ri = _rows[r];
        if (ri.kind != RowKind::Conv) continue;
        const auto &conv = _convs[ri.convIdx];
        if ((conv.kind != ConvKind::Im && conv.kind != ConvKind::Mpim) || !conv.dmUser)
            continue;
        const auto infoIt = _userInfos.constFind(conv.dmUser->value);
        if (infoIt == _userInfos.constEnd()) continue;
        const QString &url = infoIt->avatarUrl;
        if (!url.isEmpty())
            _imgCache->get(url);
    }
}

void ConvListWidget::drawUserAvatar(QPainter &p, QRect rect, const QString &userId,
                                    QColor bgColor) const {
    const auto infoIt = _userInfos.constFind(userId);
    const QString url  = (infoIt != _userInfos.constEnd()) ? infoIt->avatarUrl : QString{};
    const QPixmap pixmap = (_imgCache && !url.isEmpty()) ? _imgCache->get(url) : QPixmap{};
    const QString initial = (infoIt != _userInfos.constEnd() && !infoIt->displayName.isEmpty())
                            ? infoIt->displayName.left(1) : QString{"?"};
    const UserAvatar::State state = (infoIt != _userInfos.constEnd())
        ? UserAvatar::State{infoIt->isActive, infoIt->dndEnabled}
        : UserAvatar::State{};
    UserAvatar::paint(p, rect, pixmap, initial, state, kAvatarRadius,
                      p.device()->devicePixelRatioF(), bgColor);
}

// ── Painting ──────────────────────────────────────────────────────────────────

void ConvListWidget::doPaint(QPaintEvent *event) {
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(event->rect(), Theme::kConvPanelBg);

    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();

    const int first = scrollY / kRowH;
    const int last  = std::min(static_cast<int>(_rows.size()) - 1,
                               (scrollY + vh) / kRowH);

    for (int r = first; r <= last; ++r)
        paintRow(p, r, r * kRowH - scrollY);

    triggerMissingAvatarDownloads();

    paintScrollThumb(p, static_cast<int>(_rows.size()) * kRowH, QColor(255, 255, 255, 100));
}

void ConvListWidget::paintSectionHeader(QPainter &p, int row, int y, int sectionId) const {
    const bool hovered   = (row == _hovered);
    const bool collapsed = (sectionId == 0) ? _channelsCollapsed : _dmsCollapsed;

    if (hovered)
        p.fillRect(QRect(0, y, viewport()->width(), kRowH), Theme::kHoverItem);

    // Normally show the section icon; on hover replace it with the chevron that
    // previews what clicking will do (collapsed → down chevron, expanded → right chevron).
    static const QPixmap kChevDownDim  = svgPixmap(":/ui/chevron-down.svg",  QSize(kIconSize, kIconSize), Theme::kTextOnDarkDim);
    static const QPixmap kChevRightDim = svgPixmap(":/ui/chevron-right.svg", QSize(kIconSize, kIconSize), Theme::kTextOnDarkDim);
    static const QPixmap kHashDim      = svgPixmap(":/ui/hash.svg",          QSize(kIconSize, kIconSize), Theme::kTextOnDarkDim);
    static const QPixmap kMsgDim       = svgPixmap(":/ui/message-square.svg",QSize(kIconSize, kIconSize), Theme::kTextOnDarkDim);

    const QColor color = Theme::kTextOnDarkDim;

    const QPixmap *icon;
    if (hovered)
        icon = collapsed ? &kChevRightDim : &kChevDownDim;
    else
        icon = (sectionId == 0) ? &kHashDim : &kMsgDim;

    const int iconY = y + (kRowH - kIconSize) / 2;
    int x = kPadH;
    p.drawPixmap(x, iconY, *icon);
    x += kIconSize + 6;

    QFont font = QApplication::font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 0.82);
    p.setFont(font);
    p.setPen(color);
    const QFontMetrics fm(font);
    const QString label = (sectionId == 0) ? tr("Channels") : tr("Direct messages");
    p.drawText(x, y + (kRowH - fm.height()) / 2 + fm.ascent(), label);
}

void ConvListWidget::paintAddChannelsRow(QPainter &p, int row, int y) const {
    const bool hovered = (row == _hovered);
    if (hovered)
        p.fillRect(QRect(0, y, viewport()->width(), kRowH), Theme::kHoverItem);

    const QColor color = hovered ? Theme::kTextOnDark : Theme::kTextOnDarkDim;

    QFont font = QApplication::font();
    font.setWeight(QFont::Normal);
    p.setFont(font);
    p.setPen(color);

    const QFontMetrics fm(font);
    const int textY   = y + (kRowH - fm.height()) / 2 + fm.ascent();
    const int leftX   = kPadH + kGroupIndent;
    const int prefixW = fm.horizontalAdvance("+") + 6;
    p.drawText(leftX, textY, "+");
    p.drawText(leftX + prefixW, textY, tr("Add channels"));
}

void ConvListWidget::paintRow(QPainter &p, int row, int y) const {
    const auto &ri = _rows[row];

    if (ri.kind == RowKind::SectionHeader) {
        paintSectionHeader(p, row, y, ri.sectionId);
        return;
    }
    if (ri.kind == RowKind::AddChannels) {
        paintAddChannelsRow(p, row, y);
        return;
    }

    // ── Conv row ──────────────────────────────────────────────────────
    const auto &conv = _convs[ri.convIdx];
    const QRect rowRect(0, y, viewport()->width(), kRowH);

    // Background
    QColor rowBg = Theme::kConvPanelBg;
    if (row == _selected) {
        const QColor base = (row == _hovered) ? Theme::kHoverItem : Theme::kConvPanelBg;
        const QColor sel  = Theme::kSelectedItem;
        auto lerp = [](int a, int b, double t) {
            return static_cast<int>(a + (b - a) * t);
        };
        rowBg = QColor(lerp(base.red(),   sel.red(),   _selT),
                       lerp(base.green(), sel.green(), _selT),
                       lerp(base.blue(),  sel.blue(),  _selT));
        p.fillRect(rowRect, rowBg);
    } else if (row == _hovered) {
        rowBg = Theme::kHoverItem;
        p.fillRect(rowRect, rowBg);
    }

    QFont font = QApplication::font();
    const bool isUnread   = conv.unread > 0;
    const bool isSelected = (row == _selected);
    font.setWeight((isUnread || isSelected) ? QFont::DemiBold : QFont::Normal);
    p.setFont(font);

    const QColor textColor = (isSelected || isUnread)
        ? Theme::kTextOnDark
        : Theme::kTextOnDarkDim;
    p.setPen(textColor);

    const QFontMetrics fm(font);
    const int textY = y + (kRowH - fm.height()) / 2 + fm.ascent();

    const bool isDm = (conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim);

    const int leftX = kPadH + kGroupIndent;
    if (isDm && conv.dmUser) {
        const int avY = y + (kRowH - kAvatarSize) / 2;
        drawUserAvatar(p, QRect(leftX, avY, kAvatarSize, kAvatarSize),
                       conv.dmUser->value, rowBg);

        const auto infoIt = _userInfos.constFind(conv.dmUser->value);
        const QString emoji = (infoIt != _userInfos.constEnd())
                              ? MsgRender::resolveEmoji(infoIt->statusEmoji) : QString{};
        const bool isMe = !_meUserId.value.isEmpty() && conv.dmUser == _meUserId;

        const int nameX  = leftX + kAvatarSize + kAvatarGap;
        const int badgeW = conv.unread > 9 ? 28 : conv.unread > 0 ? 20 : 0;

        int suffixW = 0;
        QString emojiGlyph;
        if (!emoji.isEmpty() && !infoIt->statusEmoji.isEmpty()) {
            emojiGlyph = emoji;
            QFont ef = emojiFont(static_cast<int>(font.pixelSize() > 0
                                                  ? font.pixelSize()
                                                  : QFontMetrics(font).height()));
            suffixW += QFontMetrics(ef).horizontalAdvance(emojiGlyph) + 4;
        }
        int youW = 0;
        if (isMe) {
            QFont df = font;
            df.setWeight(QFont::Normal);
            df.setPointSizeF(df.pointSizeF() * 0.88);
            youW = QFontMetrics(df).horizontalAdvance(tr("you")) + 6;
            suffixW += youW;
        }

        const int maxW  = viewport()->width() - nameX - suffixW - badgeW - 8;
        const QString name = fm.elidedText(resolvedName(row), Qt::ElideRight, maxW);
        p.setPen(textColor);
        p.drawText(nameX, textY, name);
        int curX = nameX + fm.horizontalAdvance(name);

        if (!emojiGlyph.isEmpty()) {
            curX += 4;
            const int emojiPx = static_cast<int>(font.pixelSize() > 0
                                                 ? font.pixelSize()
                                                 : QFontMetrics(font).height());
            p.setFont(emojiFont(emojiPx));
            p.setPen(textColor);
            p.drawText(curX, textY, emojiGlyph);
            curX += QFontMetrics(p.font()).horizontalAdvance(emojiGlyph);
        }

        if (isMe) {
            curX += 4;
            QFont df = font;
            df.setWeight(QFont::Normal);
            df.setPointSizeF(df.pointSizeF() * 0.88);
            p.setFont(df);
            p.setPen(Theme::kTextOnDarkDim);
            p.drawText(curX, textY, tr("you"));
        }
    } else {
        const int badgeW = conv.unread > 9 ? 28 : conv.unread > 0 ? 20 : 0;
        int prefixW = 0;
        if (conv.kind == ConvKind::PrivateChannel) {
            static const QPixmap kLockDim    = svgPixmap(":/ui/lock.svg", QSize(14, 14), Theme::kTextOnDarkDim);
            static const QPixmap kLockBright = svgPixmap(":/ui/lock.svg", QSize(14, 14), Theme::kTextOnDark);
            const QPixmap &lockPx = (isSelected || isUnread) ? kLockBright : kLockDim;
            p.drawPixmap(leftX, y + (kRowH - 14) / 2, lockPx);
            prefixW = 14 + 6;
        } else {
            p.drawText(leftX, textY, "#");
            prefixW = fm.horizontalAdvance("#") + 6;
        }
        const int maxW = viewport()->width() - leftX - prefixW - badgeW - 8;
        const QString name = fm.elidedText(conv.name, Qt::ElideRight, maxW);
        p.drawText(leftX + prefixW, textY, name);
    }

    // ── Unread badge ──────────────────────────────────────────────────
    if (conv.unread > 0) {
        const QString badge = conv.unread > 99 ? "99+" : QString::number(conv.unread);
        QFont bf = font;
        bf.setPointSizeF(bf.pointSizeF() * 0.78);
        bf.setBold(true);
        p.setFont(bf);
        const QFontMetrics bfm(bf);
        const int bw = bfm.horizontalAdvance(badge) + 10;
        const int bh = bfm.height() + 4;
        const int bx = viewport()->width() - bw - 8;
        const int by = y + (kRowH - bh) / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::kUnreadBadge);
        p.drawRoundedRect(QRect(bx, by, bw, bh), bh / 2, bh / 2);
        p.setPen(Qt::white);
        p.drawText(QRect(bx, by, bw, bh), Qt::AlignCenter, badge);
    }
}
