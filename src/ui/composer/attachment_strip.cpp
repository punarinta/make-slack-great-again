// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "attachment_strip.h"
#include "ui/icon_utils.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

AttachmentStrip::AttachmentStrip(QWidget *parent)
    : QWidget(parent)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    _scroll = new QScrollArea(this);
    _scroll->setObjectName("fileScrollArea");
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setFixedHeight(84);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setStyleSheet(
        "QScrollArea#fileScrollArea { background: transparent; border: none; }"
        "QScrollArea#fileScrollArea > QWidget { background: transparent; }");

    _strip = new QWidget;
    _strip->setObjectName("fileStrip");
    _strip->setStyleSheet("QWidget#fileStrip { background: transparent; }");
    auto *stripLayout = new QHBoxLayout(_strip);
    stripLayout->setContentsMargins(8, 6, 8, 6);
    stripLayout->setSpacing(8);
    stripLayout->addStretch();

    _scroll->setWidget(_strip);
    _scroll->setWidgetResizable(true);
    outerLayout->addWidget(_scroll);

    hide();
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
    const QString name = fi.fileName();
    const qint64  size = fi.size();

    auto *chip = new QFrame(_strip);
    chip->setObjectName("fileChip");
    chip->setFixedSize(160, 70);
    chip->setStyleSheet(
        "QFrame#fileChip {"
        "  background: #F8F8F8; border: 1px solid #E0E0E0; border-radius: 8px;"
        "}");

    auto *chipLayout = new QVBoxLayout(chip);
    chipLayout->setContentsMargins(8, 6, 8, 6);
    chipLayout->setSpacing(2);

    auto *nameLabel = new QLabel(chip);
    nameLabel->setText(name.length() > 18 ? name.left(15) + "…" + fi.suffix() : name);
    nameLabel->setStyleSheet("font-size:11px; color:#1D1C1D; font-weight:600; border:none;");
    nameLabel->setWordWrap(false);

    auto fmtSize = [](qint64 b) -> QString {
        if (b < 1024) return QString::number(b) + " B";
        if (b < 1024*1024) return QString::number(b/1024) + " KB";
        return QString::number(b/(1024*1024)) + " MB";
    };
    auto *sizeLabel = new QLabel(fmtSize(size), chip);
    sizeLabel->setStyleSheet("font-size:10px; color:#888; border:none;");

    chipLayout->addWidget(nameLabel);
    chipLayout->addWidget(sizeLabel);
    chipLayout->addStretch();

    auto *removeBtn = new QToolButton(chip);
    removeBtn->setFixedSize(16, 16);
    removeBtn->setIconSize(QSize(10, 10));
    removeBtn->setIcon(svgIcon(":/ui/x.svg", QSize(10, 10), QColor("#888")));
    removeBtn->setFocusPolicy(Qt::NoFocus);
    removeBtn->setCursor(Qt::PointingHandCursor);
    removeBtn->setStyleSheet(
        "QToolButton { border:none; border-radius:8px; background:#E0E0E0; }"
        "QToolButton:hover { background:#CCCCCC; }");
    removeBtn->setParent(chip);
    removeBtn->move(chip->width() - 20, 4);
    removeBtn->raise();
    connect(removeBtn, &QToolButton::clicked, this, [this, path] {
        emit removeRequested(path);
    });

    auto *lay = qobject_cast<QHBoxLayout *>(_strip->layout());
    lay->insertWidget(lay->count() - 1, chip);
}

void AttachmentStrip::addReadOnlyChip(const File &file) {
    auto *chip = new QFrame(_strip);
    chip->setObjectName("fileChipRO");
    chip->setFixedSize(160, 70);
    chip->setStyleSheet(
        "QFrame#fileChipRO {"
        "  background: #F0F0F0; border: 1px solid #E0E0E0; border-radius: 8px;"
        "}");

    auto *chipLayout = new QVBoxLayout(chip);
    chipLayout->setContentsMargins(8, 6, 8, 6);
    chipLayout->setSpacing(2);

    const QString name = file.name;
    auto *nameLabel = new QLabel(chip);
    nameLabel->setText(name.length() > 18
        ? name.left(15) + "…" + QFileInfo(name).suffix()
        : name);
    nameLabel->setStyleSheet("font-size:11px; color:#616061; font-weight:600; border:none;");

    auto *typeLabel = new QLabel(
        file.prettyType.isEmpty() ? file.mimeType : file.prettyType, chip);
    typeLabel->setStyleSheet("font-size:10px; color:#888; border:none;");

    chipLayout->addWidget(nameLabel);
    chipLayout->addWidget(typeLabel);
    chipLayout->addStretch();

    auto *lay = qobject_cast<QHBoxLayout *>(_strip->layout());
    lay->insertWidget(lay->count() - 1, chip);
}
