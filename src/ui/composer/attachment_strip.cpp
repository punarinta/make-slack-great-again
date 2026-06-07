// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "attachment_strip.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

AttachmentStrip::AttachmentStrip(QWidget *parent) : QWidget(parent) {
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    _scroll = new QScrollArea(this);
    _scroll->setObjectName("fileScrollArea");
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setFixedHeight(84);
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

    hide();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void AttachmentStrip::applyTheme() {
    _scroll->setStyleSheet("QScrollArea#fileScrollArea { background: transparent; border: none; }"
                           "QScrollArea#fileScrollArea > QWidget { background: transparent; }");
    _strip->setStyleSheet("QWidget#fileStrip { background: transparent; }");
}

void AttachmentStrip::rebuild(const QStringList &pending, const std::vector<File> &readOnly) {
    _pending  = pending;
    _readOnly = readOnly;

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
    const QString   name = fi.fileName();
    const qint64    size = fi.size();

    auto *chip = new QFrame(_strip);
    chip->setObjectName("fileChip");
    chip->setFixedSize(160, 70);
    chip->setStyleSheet(QString("QFrame#fileChip {"
                                "  background: %1; border: 1px solid %2; border-radius: 8px;"
                                "}")
                            .arg(
                                Th::qss(Th::c().composer.attachmentChipBg),
                                Th::qss(Th::c().composer.attachmentChipBorder)
                            ));

    auto *chipLayout = new QVBoxLayout(chip);
    chipLayout->setContentsMargins(8, 6, 8, 6);
    chipLayout->setSpacing(2);

    auto *nameLabel = new QLabel(chip);
    nameLabel->setText(name.length() > 18 ? name.left(15) + "…" + fi.suffix() : name);
    nameLabel->setStyleSheet(QString("font-size:%2px; color:%1; font-weight:600; border:none;")
                                 .arg(Th::qss(Th::c().text.primary))
                                 .arg(Th::c().fonts.sm));
    nameLabel->setWordWrap(false);

    auto fmtSize = [](qint64 b) -> QString {
        if (b < 1024)
            return QString::number(b) + " B";
        if (b < 1024 * 1024)
            return QString::number(b / 1024) + " KB";
        return QString::number(b / (1024 * 1024)) + " MB";
    };
    auto *sizeLabel = new QLabel(fmtSize(size), chip);
    sizeLabel->setStyleSheet(QString("font-size:%2px; color:%1; border:none;")
                                 .arg(Th::qss(Th::c().text.tertiary))
                                 .arg(Th::c().fonts.xs));

    chipLayout->addWidget(nameLabel);
    chipLayout->addWidget(sizeLabel);
    chipLayout->addStretch();

    auto *removeBtn = new QToolButton(chip);
    removeBtn->setFixedSize(16, 16);
    removeBtn->setIconSize(QSize(10, 10));
    removeBtn->setIcon(svgIcon(":/ui/x.svg", QSize(10, 10), Th::c().icon.def));
    removeBtn->setFocusPolicy(Qt::NoFocus);
    removeBtn->setCursor(Qt::PointingHandCursor);
    removeBtn->setStyleSheet(
        QString("QToolButton { border:none; border-radius:8px; background:%1; }"
                "QToolButton:hover { background:%2; }")
            .arg(Th::qss(Th::c().composer.attachmentChipBorder), Th::qss(Th::c().icon.dim))
    );
    removeBtn->setParent(chip);
    removeBtn->move(chip->width() - 20, 4);
    removeBtn->raise();
    connect(removeBtn, &QToolButton::clicked, this, [this, path] { emit removeRequested(path); });

    auto *lay = qobject_cast<QHBoxLayout *>(_strip->layout());
    lay->insertWidget(lay->count() - 1, chip);
}

void AttachmentStrip::addReadOnlyChip(const File &file) {
    auto *chip = new QFrame(_strip);
    chip->setObjectName("fileChipRO");
    chip->setFixedSize(160, 70);
    chip->setStyleSheet(
        QString("QFrame#fileChipRO {"
                "  background: %1; border: 1px solid %2; border-radius: 8px;"
                "}")
            .arg(Th::qss(Th::c().surface.highlight), Th::qss(Th::c().composer.attachmentChipBorder))
    );

    auto *chipLayout = new QVBoxLayout(chip);
    chipLayout->setContentsMargins(8, 6, 8, 6);
    chipLayout->setSpacing(2);

    const QString name      = file.name;
    auto         *nameLabel = new QLabel(chip);
    nameLabel->setText(name.length() > 18 ? name.left(15) + "…" + QFileInfo(name).suffix() : name);
    nameLabel->setStyleSheet(QString("font-size:%2px; color:%1; font-weight:600; border:none;")
                                 .arg(Th::qss(Th::c().text.secondary))
                                 .arg(Th::c().fonts.sm));

    auto *typeLabel = new QLabel(file.prettyType.isEmpty() ? file.mimeType : file.prettyType, chip);
    typeLabel->setStyleSheet(QString("font-size:%2px; color:%1; border:none;")
                                 .arg(Th::qss(Th::c().text.tertiary))
                                 .arg(Th::c().fonts.xs));

    chipLayout->addWidget(nameLabel);
    chipLayout->addWidget(typeLabel);
    chipLayout->addStretch();

    auto *lay = qobject_cast<QHBoxLayout *>(_strip->layout());
    lay->insertWidget(lay->count() - 1, chip);
}
