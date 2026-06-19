// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "image_viewer.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QUrl>

static constexpr QSize kBtnIconSz{20, 20};
static constexpr int   kBtnSz = 36;

ImageViewerOverlay::ImageViewerOverlay(QWidget *windowParent) : QWidget(windowParent) {
    setFocusPolicy(Qt::StrongFocus);
    hide();

    _bar      = new QWidget(this);
    auto *lay = new QHBoxLayout(_bar);
    lay->setContentsMargins(kMargin, 0, kMargin - 8, 0);
    lay->setSpacing(Th::c().spacing.sm);

    _nameLabel = new QLabel(_bar);
    QFont nf   = _nameLabel->font();
    nf.setPointSizeF(nf.pointSizeF() * 1.05);
    _nameLabel->setFont(nf);
    lay->addWidget(_nameLabel, 1);

    const auto makeBtn = [&](const QString &tip) {
        auto *b = new QToolButton(_bar);
        b->setFixedSize(kBtnSz, kBtnSz);
        b->setIconSize(kBtnIconSz);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        lay->addWidget(b);
        return b;
    };
    _downloadBtn = makeBtn(tr("Download"));
    _forwardBtn  = makeBtn(tr("Forward"));
    _browserBtn  = makeBtn(tr("Open in browser"));
    _moreBtn     = makeBtn(tr("More actions"));
    _closeBtn    = makeBtn(tr("Close"));

    connect(_downloadBtn, &QToolButton::clicked, this, [this] { emit downloadRequested(_file); });
    connect(_forwardBtn, &QToolButton::clicked, this, [this] {
        hide(); // the forward dialog replaces the viewer
        emit forwardRequested(_msg);
    });
    connect(_browserBtn, &QToolButton::clicked, this, [this] {
        const QString url = _file.permalink.isEmpty() ? _file.urlPrivate : _file.permalink;
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
    });
    connect(_moreBtn, &QToolButton::clicked, this, [this] {
        emit moreRequested(_file, _msg, _moreBtn->mapToGlobal(QPoint(0, _moreBtn->height() + 2)));
    });
    connect(_closeBtn, &QToolButton::clicked, this, &QWidget::hide);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        applyTheme();
        update();
    });

    if (windowParent)
        windowParent->installEventFilter(this); // track window resizes while open
}

void ImageViewerOverlay::applyTheme() {
    _nameLabel->setStyleSheet("color:" + Th::qss(Th::c().text.onDark) + ";");
    const QString btnQss = QString(
                               "QToolButton { border: none; border-radius: %1px;"
                               "  background: transparent; }"
                               "QToolButton:hover { background: %2; }"
    )
                               .arg(kBtnSz / 2)
                               .arg(Th::qss(Th::c().surface.viewerBtnHover));
    const QColor ic    = Th::c().icon.onDark;
    const auto   style = [&](QToolButton *b, const QString &icon) {
        b->setStyleSheet(btnQss);
        b->setIcon(svgIcon(icon, kBtnIconSz, ic));
    };
    style(_downloadBtn, ":/ui/download.svg");
    style(_forwardBtn, ":/ui/share-2.svg");
    style(_browserBtn, ":/ui/external-link.svg");
    style(_moreBtn, ":/ui/more-horizontal.svg");
    style(_closeBtn, ":/ui/x.svg");
}

void ImageViewerOverlay::open(const File &file, const Message &msg, const QPixmap &pixmap) {
    _file   = file;
    _msg    = msg;
    _pixmap = pixmap;
    _nameLabel->setText(file.name);

    // Pending messages expose no actions — same rule as the file action bar.
    const bool actions = !msg.pending;
    _downloadBtn->setVisible(actions);
    _forwardBtn->setVisible(actions);
    _moreBtn->setVisible(actions);
    _browserBtn->setVisible(actions && !(file.permalink.isEmpty() && file.urlPrivate.isEmpty()));

    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
        _bar->setGeometry(0, 0, width(), kBarH);
    }
    show();
    raise();
    setFocus();
    update();
}

void ImageViewerOverlay::updatePixmap(const QString &fileId, const QPixmap &pixmap) {
    if (!isVisible() || fileId != _file.id || pixmap.isNull())
        return;
    _pixmap = pixmap;
    update();
}

QRect ImageViewerOverlay::imageRect() const {
    if (_pixmap.isNull())
        return {};
    const QRect avail = rect().adjusted(kMargin, kBarH + kMargin / 2, -kMargin, -kMargin);
    if (avail.isEmpty())
        return {};
    const QSizeF ps = _pixmap.deviceIndependentSize();
    const double scale =
        std::min(1.0, std::min(avail.width() / ps.width(), avail.height() / ps.height()));
    const int w = static_cast<int>(ps.width() * scale);
    const int h = static_cast<int>(ps.height() * scale);
    return {avail.left() + (avail.width() - w) / 2, avail.top() + (avail.height() - h) / 2, w, h};
}

void ImageViewerOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Th::c().surface.viewerBackdrop);
    if (_pixmap.isNull()) {
        p.setPen(Th::c().text.onDarkDim);
        p.drawText(rect(), Qt::AlignCenter, tr("Loading image…"));
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawPixmap(imageRect(), _pixmap);
}

void ImageViewerOverlay::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ImageViewerOverlay::mousePressEvent(QMouseEvent *e) {
    // Backdrop click dismisses; clicks on the image or the top bar do not.
    if (e->button() == Qt::LeftButton && e->pos().y() > kBarH && !imageRect().contains(e->pos())) {
        hide();
        return;
    }
    QWidget::mousePressEvent(e);
}

bool ImageViewerOverlay::eventFilter(QObject *obj, QEvent *ev) {
    if (obj == parentWidget() && ev->type() == QEvent::Resize && isVisible()) {
        setGeometry(parentWidget()->rect());
        _bar->setGeometry(0, 0, width(), kBarH);
    }
    return QWidget::eventFilter(obj, ev);
}
