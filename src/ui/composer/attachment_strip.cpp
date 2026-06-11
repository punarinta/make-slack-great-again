// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "attachment_strip.h"
#include "ui/icon_utils.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageIOHandler>
#include <QImageReader>
#include <QLabel>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr QSize kChipSize{160, 92};
constexpr int   kChipRadius = 8;

// Chip for a pending image file: the image cover-fills the rounded chip
// (scaled to fill both dimensions, center-cropped, never stretched).
class ImageChip : public QFrame {
public:
    ImageChip(const QString &path, QWidget *parent) : QFrame(parent), _path(path) {}

protected:
    void paintEvent(QPaintEvent *) override {
        ensureCover();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (!_cover.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(r, kChipRadius, kChipRadius);
            p.setClipPath(clip);
            p.drawPixmap(rect(), _cover);
            p.setClipping(false);
        }
        p.setPen(QPen(Th::c().composer.attachmentChipBorder, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, kChipRadius, kChipRadius);
    }

private:
    void ensureCover() {
        const qreal dpr = devicePixelRatioF();
        if (!_cover.isNull() && qFuzzyCompare(_coverDpr, dpr))
            return;
        const QSize  target = size() * dpr;
        QImageReader reader(_path);
        reader.setAutoTransform(true);
        QSize src = reader.size();
        if (src.isValid()) {
            if (reader.transformation() & QImageIOHandler::TransformationRotate90)
                src.transpose();
            reader.setScaledSize(src.scaled(target, Qt::KeepAspectRatioByExpanding));
        }
        QImage img = reader.read();
        if (img.isNull())
            return;
        if (!src.isValid())
            img = img.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QRect crop(
            (img.width() - target.width()) / 2,
            (img.height() - target.height()) / 2,
            target.width(),
            target.height()
        );
        _cover = QPixmap::fromImage(img.copy(crop));
        _cover.setDevicePixelRatio(dpr);
        _coverDpr = dpr;
    }

    QString _path;
    QPixmap _cover;
    qreal   _coverDpr = 0;
};

// Chip for a pending text file: the first few lines of content fill the chip
// background as a static preview (no scrolling).
class TextChip : public QFrame {
public:
    TextChip(const QString &path, QWidget *parent) : QFrame(parent) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            QString head = QString::fromUtf8(f.read(2048));
            head.remove('\r');
            head.replace('\t', "    ");
            const QStringList all = head.split('\n');
            _lines                = all.mid(0, kPreviewLines);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(Th::c().composer.attachmentChipBorder, 1));
        p.setBrush(Th::c().composer.attachmentChipBg);
        p.drawRoundedRect(r, kChipRadius, kChipRadius);
        if (_lines.isEmpty())
            return;
        QPainterPath clip;
        clip.addRoundedRect(r, kChipRadius, kChipRadius);
        p.setClipPath(clip);
        QFont f = font();
        f.setPixelSize(Th::c().fonts.xs);
        p.setFont(f);
        p.setPen(Th::c().text.tertiary);
        const QFontMetrics fm(f);
        int                y = kPad + fm.ascent();
        for (const QString &line : _lines) {
            p.drawText(kPad, y, line);
            y += fm.lineSpacing();
            if (y > height())
                break;
        }
    }

private:
    static constexpr int kPreviewLines = 8;
    static constexpr int kPad          = 6;

    QStringList _lines;
};

bool isTextFile(const QString &path) {
    const QMimeType mt = QMimeDatabase().mimeTypeForFile(path);
    return mt.inherits("text/plain") || mt.name().startsWith("text/");
}

QString fmtSize(qint64 b) {
    if (b < 1024)
        return QString::number(b) + " B";
    if (b < 1024 * 1024)
        return QString::number(b / 1024) + " KB";
    return QString::number(b / (1024 * 1024)) + " MB";
}

} // namespace

AttachmentStrip::AttachmentStrip(QWidget *parent) : QWidget(parent) {
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    _scroll = new QScrollArea(this);
    _scroll->setObjectName("fileScrollArea");
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setFixedHeight(kChipSize.height() + 14);
    _scroll->setFrameShape(QFrame::NoFrame);

    _strip = new QWidget;
    _strip->setObjectName("fileStrip");
    auto *stripLayout = new QHBoxLayout(_strip);
    stripLayout->setContentsMargins(8, 6, 8, 6);
    stripLayout->setSpacing(8);
    stripLayout->addStretch();

    _scroll->setWidget(_strip);
    _scroll->setWidgetResizable(true);
    outerLayout->addWidget(_scroll);

    _tooltip = new PopupTooltip(this);

    hide();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void AttachmentStrip::applyTheme() {
    _scroll->setStyleSheet(
        "QScrollArea#fileScrollArea { background: transparent; border: none; }"
        "QScrollArea#fileScrollArea > QWidget { background: transparent; }"
    );
    _strip->setStyleSheet("QWidget#fileStrip { background: transparent; }");
}

void AttachmentStrip::rebuild(const QStringList &pending, const std::vector<File> &readOnly) {
    _pending  = pending;
    _readOnly = readOnly;

    _tooltip->hide();
    _tooltipBtns.clear();

    // Remove all chips (everything except the trailing stretch).
    auto *lay = qobject_cast<QHBoxLayout *>(_strip->layout());
    while (lay->count() > 1)
        delete lay->takeAt(0)->widget();

    if (!hasFiles()) {
        hide();
        return;
    }

    for (const QString &path : std::as_const(_pending))
        addPendingChip(path);
    for (const auto &f : _readOnly)
        addReadOnlyChip(f);

    show();
    _strip->adjustSize();
}

void AttachmentStrip::addPendingChip(const QString &path) {
    const QFileInfo fi(path);
    const QString   name    = fi.fileName();
    const qint64    size    = fi.size();
    const bool      isImage = QImageReader(path).canRead();

    QFrame *chip = nullptr;
    if (isImage) {
        chip = new ImageChip(path, _strip);
    } else if (isTextFile(path)) {
        chip = new TextChip(path, _strip);
    } else {
        chip = new QFrame(_strip);
        chip->setObjectName("fileChip");
        chip->setStyleSheet(QString(
                                "QFrame#fileChip {"
                                "  background: %1; border: 1px solid %2; border-radius: 8px;"
                                "}"
        )
                                .arg(
                                    Th::qss(Th::c().composer.attachmentChipBg),
                                    Th::qss(Th::c().composer.attachmentChipBorder)
                                ));
    }
    chip->setFixedSize(kChipSize);

    addOverlayLabels(
        chip, name.length() > 18 ? name.left(15) + "…" + fi.suffix() : name, fmtSize(size)
    );
    addRemoveButton(chip, path, isImage);

    auto *lay = qobject_cast<QHBoxLayout *>(_strip->layout());
    lay->insertWidget(lay->count() - 1, chip);
}

// Name + secondary line bottom-left on semitransparent plates, shared by all chip kinds.
void AttachmentStrip::addOverlayLabels(QFrame *chip, const QString &name, const QString &sub) {
    auto *chipLayout = new QVBoxLayout(chip);
    chipLayout->setContentsMargins(6, 6, 6, 6);
    chipLayout->setSpacing(2);

    const auto plate = [](int fontPx) {
        return QString(
                   "font-size:%2px; color:%1; background:%3; border:none;"
                   " border-radius:4px; padding:1px 4px;"
        )
            .arg(
                Th::qss(Th::c().composer.attachmentOverlayText),
                QString::number(fontPx),
                Th::qss(Th::c().composer.attachmentOverlayBg)
            );
    };

    auto *nameLabel = new QLabel(name, chip);
    nameLabel->setWordWrap(false);
    nameLabel->setStyleSheet(plate(Th::c().fonts.sm) + "font-weight:600;");

    auto *subLabel = new QLabel(sub, chip);
    subLabel->setStyleSheet(plate(Th::c().fonts.xs));

    chipLayout->addStretch();
    chipLayout->addWidget(nameLabel, 0, Qt::AlignLeft);
    chipLayout->addWidget(subLabel, 0, Qt::AlignLeft);
}

void AttachmentStrip::addRemoveButton(QFrame *chip, const QString &path, bool onImage) {
    auto *removeBtn = new QToolButton(chip);
    removeBtn->setFixedSize(16, 16);
    removeBtn->setIconSize(QSize(10, 10));
    removeBtn->setIcon(svgIcon(
        ":/ui/x.svg",
        QSize(10, 10),
        onImage ? Th::c().composer.attachmentOverlayText : Th::c().icon.def
    ));
    removeBtn->setFocusPolicy(Qt::NoFocus);
    removeBtn->setCursor(Qt::PointingHandCursor);
    removeBtn->setAttribute(Qt::WA_Hover);
    removeBtn->installEventFilter(this);
    _tooltipBtns[removeBtn] = tr("Remove attachment");
    removeBtn->setStyleSheet(QString(
                                 "QToolButton { border:none; border-radius:8px; background:%1; }"
                                 "QToolButton:hover { background:%2; }"
    )
                                 .arg(
                                     Th::qss(
                                         onImage ? Th::c().composer.attachmentOverlayBg
                                                 : Th::c().composer.attachmentChipBorder
                                     ),
                                     Th::qss(Th::c().icon.dim)
                                 ));
    removeBtn->move(chip->width() - 20, 4);
    removeBtn->raise();
    connect(removeBtn, &QToolButton::clicked, this, [this, path] { emit removeRequested(path); });
}

bool AttachmentStrip::eventFilter(QObject *obj, QEvent *event) {
    if (auto *w = qobject_cast<QWidget *>(obj); w && _tooltipBtns.contains(w)) {
        if (event->type() == QEvent::HoverEnter) {
            _tooltip->showAbove(_tooltipBtns[w], QRect(w->mapToGlobal(QPoint(0, 0)), w->size()));
        } else if (event->type() == QEvent::HoverLeave) {
            _tooltip->hide();
        }
    }
    return QWidget::eventFilter(obj, event);
}

void AttachmentStrip::addReadOnlyChip(const File &file) {
    auto *chip = new QFrame(_strip);
    chip->setObjectName("fileChipRO");
    chip->setFixedSize(kChipSize);
    chip->setStyleSheet(
        QString(
            "QFrame#fileChipRO {"
            "  background: %1; border: 1px solid %2; border-radius: 8px;"
            "}"
        )
            .arg(Th::qss(Th::c().surface.highlight), Th::qss(Th::c().composer.attachmentChipBorder))
    );

    const QString name = file.name;
    addOverlayLabels(
        chip,
        name.length() > 18 ? name.left(15) + "…" + QFileInfo(name).suffix() : name,
        file.prettyType.isEmpty() ? file.mimeType : file.prettyType
    );

    auto *lay = qobject_cast<QHBoxLayout *>(_strip->layout());
    lay->insertWidget(lay->count() - 1, chip);
}
