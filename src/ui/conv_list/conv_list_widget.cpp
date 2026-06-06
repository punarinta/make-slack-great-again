// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "conv_list_widget.h"
#include "ui/theme.h"
#include "ui/icon_utils.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QFontMetrics>
#include <QApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

ConvListWidget::ConvListWidget(QWidget *parent)
    : QAbstractScrollArea(parent)
    , _nam(new QNetworkAccessManager(this))
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setCursor(Qt::PointingHandCursor);
    viewport()->installEventFilter(this);

    _selAnim.setDuration(140);
    _selAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&_selAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        _selT = v.toDouble();
        viewport()->update();
    });
    connect(&_selAnim, &QVariantAnimation::finished, this, [this] {
        _selT   = 1.0;
        _selFrom = -1;
    });
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
            u.displayName.isEmpty() ? u.name : u.displayName,
            u.avatarUrl,
            u.isDeactivated,
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
    _hovered  = -1;
    _selected = -1;
    updateScrollRange();
    viewport()->update();
}

ConversationId ConvListWidget::conversationId(int row) const {
    if (row < 0 || row >= (int)_convs.size()) return {};
    return _convs[row].id;
}

int ConvListWidget::rowForId(ConversationId id) const {
    for (int i = 0; i < (int)_convs.size(); ++i)
        if (_convs[i].id == id) return i;
    return -1;
}

QString ConvListWidget::resolvedName(int row) const {
    if (row < 0 || row >= (int)_convs.size()) return {};
    const auto &conv = _convs[row];
    if ((conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim) && conv.dmUser) {
        const auto it = _userInfos.constFind(conv.dmUser->value);
        if (it != _userInfos.constEnd() && !it->displayName.isEmpty())
            return it->displayName;
    }
    return conv.name;
}

// ── Events ────────────────────────────────────────────────────────────────────

bool ConvListWidget::eventFilter(QObject *obj, QEvent *event) {
    if (obj == viewport()) {
        switch (event->type()) {
        case QEvent::Paint:
            doPaint(static_cast<QPaintEvent *>(event));
            return true;
        case QEvent::MouseMove:
            doMouseMove(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::MouseButtonPress:
            doMousePress(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::MouseButtonRelease:
            doMouseRelease(static_cast<QMouseEvent *>(event));
            return true;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            doLeave(event);
            return true;
        default:
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(obj, event);
}

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
    if (row < 0 || row >= static_cast<int>(_convs.size())) return -1;
    return row;
}

void ConvListWidget::setHovered(int row) {
    if (row == _hovered) return;
    _hovered = row;
    viewport()->update();
}

void ConvListWidget::setSelected(int row) {
    if (row == _selected) return;
    _selFrom = _selected;
    _selT    = 0.0;
    _selected = row;
    _selAnim.stop();
    _selAnim.setStartValue(0.0);
    _selAnim.setEndValue(1.0);
    _selAnim.start();
    emit conversationSelected(row);
}

bool ConvListWidget::isOnScrollThumb(int vpY) const {
    const int total = static_cast<int>(_convs.size()) * kRowH;
    const int vh    = viewport()->height();
    if (total <= vh) return false;
    const int scrollY = verticalScrollBar()->value();
    const int thumbH  = std::max(20, vh * vh / total);
    const int thumbY  = scrollY * (vh - thumbH) / (total - vh);
    return vpY >= thumbY && vpY < thumbY + thumbH;
}

void ConvListWidget::doMouseMove(QMouseEvent *e) {
    if (_sbDragging) {
        const int total = static_cast<int>(_convs.size()) * kRowH;
        const int vh    = viewport()->height();
        const int thumbH    = std::max(20, vh * vh / total);
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
    if (e->pos().x() >= sbHitX && isOnScrollThumb(e->pos().y()))
        viewport()->setCursor(Qt::SizeVerCursor);
    else
        viewport()->setCursor(Qt::PointingHandCursor);
    setHovered(rowAt(e->pos().y()));
}

void ConvListWidget::doMousePress(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int sbHitX = viewport()->width() - kScrollW - 2 - 6;
    if (e->pos().x() >= sbHitX && isOnScrollThumb(e->pos().y())) {
        _sbDragging        = true;
        _sbDragStartY      = e->pos().y();
        _sbDragStartScroll = verticalScrollBar()->value();
        viewport()->setCursor(Qt::SizeVerCursor);
        return;
    }
    const int row = rowAt(e->pos().y());
    if (row >= 0) setSelected(row);
}

void ConvListWidget::doMouseRelease(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    if (_sbDragging) {
        _sbDragging = false;
        viewport()->setCursor(Qt::PointingHandCursor);
    }
}

void ConvListWidget::doLeave(QEvent *) {
    setHovered(-1);
}

// ── Layout ────────────────────────────────────────────────────────────────────

void ConvListWidget::updateScrollRange() {
    const int total = static_cast<int>(_convs.size()) * kRowH;
    const int vh    = viewport()->height();
    verticalScrollBar()->setRange(0, std::max(0, total - vh));
    verticalScrollBar()->setPageStep(vh);
}

// ── Avatar downloads ──────────────────────────────────────────────────────────

void ConvListWidget::triggerMissingAvatarDownloads() {
    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();
    const int first   = scrollY / kRowH;
    const int last    = std::min((int)_convs.size() - 1, (scrollY + vh) / kRowH);

    for (int i = first; i <= last; ++i) {
        const auto &conv = _convs[i];
        if ((conv.kind != ConvKind::Im && conv.kind != ConvKind::Mpim) || !conv.dmUser)
            continue;
        const auto infoIt = _userInfos.constFind(conv.dmUser->value);
        if (infoIt == _userInfos.constEnd()) continue;
        const QString &url = infoIt->avatarUrl;
        if (url.isEmpty() || _avatarCache.contains(url)) continue;

        _avatarCache.insert(url, {}); // sentinel — prevents double download
        auto *reply = _nam->get(QNetworkRequest(QUrl(url)));
        connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                QPixmap px;
                if (px.loadFromData(reply->readAll()) && !px.isNull())
                    _avatarCache[url] = px;
            }
            viewport()->update();
        });
    }
}

void ConvListWidget::drawUserAvatar(QPainter &p, QRect rect, const QString &userId) const {
    const auto infoIt = _userInfos.constFind(userId);
    const QString url = (infoIt != _userInfos.constEnd()) ? infoIt->avatarUrl : QString{};
    const auto cacheIt = _avatarCache.constFind(url);
    const bool hasPixmap = !url.isEmpty()
                           && cacheIt != _avatarCache.constEnd()
                           && !cacheIt->isNull();

    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    if (hasPixmap) {
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect), kAvatarRadius, kAvatarRadius);
        p.setClipPath(clip);
        const qreal dpr = p.device()->devicePixelRatioF();
        QPixmap scaled = cacheIt->scaled(
            rect.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        p.drawPixmap(rect, scaled);
    } else {
        // Placeholder: rounded-rect with first initial in white
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x8B8B8B));
        p.drawRoundedRect(rect, kAvatarRadius, kAvatarRadius);
        p.setPen(Qt::white);
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPointSizeF(rect.height() * 0.38);
        p.setFont(f);
        const QString initial = (infoIt != _userInfos.constEnd() && !infoIt->displayName.isEmpty())
            ? infoIt->displayName.left(1).toUpper()
            : "?";
        p.drawText(rect, Qt::AlignCenter, initial);
    }

    p.restore();
}

// ── Painting ──────────────────────────────────────────────────────────────────

void ConvListWidget::doPaint(QPaintEvent *event) {
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(event->rect(), Theme::kConvPanelBg);

    const int scrollY = verticalScrollBar()->value();
    const int vh      = viewport()->height();

    const int first = scrollY / kRowH;
    const int last  = std::min(static_cast<int>(_convs.size()) - 1,
                               (scrollY + vh) / kRowH);

    for (int i = first; i <= last; ++i)
        paintRow(p, i, i * kRowH - scrollY);

    // Trigger async downloads for any DM avatars not yet in cache.
    triggerMissingAvatarDownloads();

    // Thin Telegram-style scrollbar overlay
    const int total = static_cast<int>(_convs.size()) * kRowH;
    if (total > vh) {
        const int thumbH = std::max(20, vh * vh / total);
        const int thumbY = (total - vh > 0)
            ? scrollY * (vh - thumbH) / (total - vh) : 0;
        const int sbX = viewport()->width() - kScrollW - 2;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 100));
        p.drawRoundedRect(sbX, thumbY, kScrollW, thumbH,
                          kScrollW / 2.0, kScrollW / 2.0);
    }
}

void ConvListWidget::paintRow(QPainter &p, int i, int y) const {
    const auto &conv = _convs[i];
    const QRect row(0, y, viewport()->width(), kRowH);

    // ── Background ────────────────────────────────────────────────────
    if (i == _selected) {
        const QColor base = (i == _hovered) ? Theme::kHoverItem : Theme::kConvPanelBg;
        const QColor sel  = Theme::kSelectedItem;
        auto lerp = [](int a, int b, double t) {
            return static_cast<int>(a + (b - a) * t);
        };
        const QColor bg(lerp(base.red(),   sel.red(),   _selT),
                        lerp(base.green(), sel.green(), _selT),
                        lerp(base.blue(),  sel.blue(),  _selT));
        p.fillRect(row, bg);
    } else if (i == _hovered) {
        p.fillRect(row, Theme::kHoverItem);
    }

    QFont font = QApplication::font();
    const bool isUnread  = conv.unread > 0;
    const bool isSelected = (i == _selected);
    font.setWeight((isUnread || isSelected) ? QFont::DemiBold : QFont::Normal);
    p.setFont(font);

    const QColor textColor = (isSelected || isUnread)
        ? Theme::kTextOnDark
        : Theme::kTextOnDarkDim;
    p.setPen(textColor);

    const QFontMetrics fm(font);
    const int textY = y + (kRowH - fm.height()) / 2 + fm.ascent();

    const bool isDm = (conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim);

    if (isDm && conv.dmUser) {
        // Draw circular user avatar
        const int avY   = y + (kRowH - kAvatarSize) / 2;
        drawUserAvatar(p, QRect(kPadH, avY, kAvatarSize, kAvatarSize),
                       conv.dmUser->value);

        // Name after avatar
        const int nameX = kPadH + kAvatarSize + kAvatarGap;
        const int badgeW = conv.unread > 9 ? 28 : conv.unread > 0 ? 20 : 0;
        const int maxW = viewport()->width() - nameX - badgeW - 8;
        const QString name = fm.elidedText(resolvedName(i), Qt::ElideRight, maxW);
        p.setPen(textColor);
        p.drawText(nameX, textY, name);
    } else {
        // Channel: text prefix (#, lock)
        const int badgeW = conv.unread > 9 ? 28 : conv.unread > 0 ? 20 : 0;
        int prefixW = 0;
        if (conv.kind == ConvKind::PrivateChannel) {
            static const QPixmap kLockDim    = svgPixmap(":/ui/lock.svg", QSize(14, 14), Theme::kTextOnDarkDim);
            static const QPixmap kLockBright = svgPixmap(":/ui/lock.svg", QSize(14, 14), Theme::kTextOnDark);
            const QPixmap &lockPx = (isSelected || isUnread) ? kLockBright : kLockDim;
            p.drawPixmap(kPadH, y + (kRowH - 14) / 2, lockPx);
            prefixW = 14 + 6;
        } else {
            p.drawText(kPadH, textY, "#");
            prefixW = fm.horizontalAdvance("#") + 6;
        }
        const int maxW = viewport()->width() - kPadH - prefixW - badgeW - 8;
        const QString name = fm.elidedText(conv.name, Qt::ElideRight, maxW);
        p.drawText(kPadH + prefixW, textY, name);
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
