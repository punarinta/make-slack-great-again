// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "icon_picker_dialog.h"
#include "ui/file_dialog_utils.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QUrl>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>

namespace {
// Narrower than the AppDialog default (480..560): the card holds one small
// picture, two buttons and at most one option.
constexpr int kCardWidth = 440;
} // namespace

// The preview card; the owning dialog paints it (paintPreview) so what the
// user approves is what the real surface will show.
class IconPickerDialog::Preview : public QWidget {
public:
    Preview(const IconPickerDialog *owner, int size, QWidget *parent = nullptr)
        : QWidget(parent), _owner(owner) {
        setFixedSize(size, size);
    }
    void setIcon(const QPixmap &icon) {
        _icon = icon;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        _owner->paintPreview(p, QRectF(rect()), _icon);
    }

private:
    const IconPickerDialog *_owner;
    QPixmap                 _icon;
};

IconPickerDialog::IconPickerDialog(
    const Setup &setup, const QImage &current, bool hasCustom, QWidget *parent
)
    : AppDialog(setup.title, parent), _setup(setup), _current(current), _hasCustom(hasCustom) {
    setAcceptDrops(true);

    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;
    cl->setSpacing(sp.md);

    _preview = new Preview(this, kPreviewSize);
    cl->addWidget(_preview, 0, Qt::AlignHCenter);

    _chooseBtn = new StyledButton(tr("Choose image…"), StyledButton::Variant::Secondary);
    connect(_chooseBtn, &QPushButton::clicked, this, &IconPickerDialog::chooseFile);
    _defaultBtn = new StyledButton(tr("Use default"), StyledButton::Variant::Ghost);
    connect(_defaultBtn, &QPushButton::clicked, this, &IconPickerDialog::useDefault);

    auto *chooseRow = new QHBoxLayout;
    chooseRow->setSpacing(sp.md);
    chooseRow->addStretch();
    chooseRow->addWidget(_chooseBtn);
    chooseRow->addWidget(_defaultBtn);
    chooseRow->addStretch();
    cl->addLayout(chooseRow);

    _hint = new QLabel(_setup.hint);
    _hint->setWordWrap(true);
    _hint->setAlignment(Qt::AlignHCenter);
    cl->addWidget(_hint);

    // Footer: [subclass options] →stretch→ [Cancel] [Save].
    auto *leading  = new QWidget;
    _footerOptions = new QHBoxLayout(leading);
    _footerOptions->setSpacing(sp.md);
    _footerOptions->setContentsMargins(0, 0, 0, 0);
    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _saveBtn   = new StyledButton(tr("Save"), StyledButton::Variant::Primary);
    addButtonRow(_saveBtn, _cancelBtn, leading); // Cancel → reject() wired by base
    connect(_saveBtn, &QPushButton::clicked, this, [this] {
        if (_dirty)
            accept();
    });
}

int IconPickerDialog::cardWidth(int availOverlayWidth) const {
    return std::min(kCardWidth, availOverlayWidth);
}

void IconPickerDialog::finish() {
    refreshPreview();
    refreshButtons();
    applyTheme();
    updateCard();
}

bool IconPickerDialog::loadFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return loadImage(decodeBytes(f.readAll()));
}

bool IconPickerDialog::loadImage(const QImage &img) {
    if (img.isNull())
        return false;
    _chosen = img;
    _reset  = false;
    _dirty  = true;
    refreshPreview();
    refreshButtons();
    return true;
}

void IconPickerDialog::refreshPreview() {
    const QImage &src = !_chosen.isNull() ? _chosen : _reset ? QImage() : _current;
    _preview->setIcon(src.isNull() ? QPixmap() : QPixmap::fromImage(prepareImage(src)));
}

void IconPickerDialog::markDirty() {
    _dirty = true;
    refreshButtons();
}

void IconPickerDialog::chooseFile() {
    const QString path = Ui::getOpenFileName(
        this, _setup.chooserTitle, tr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.svg)")
    );
    if (path.isEmpty())
        return;
    if (!loadFile(path))
        _hint->setText(tr("That file could not be read as an image."));
}

void IconPickerDialog::useDefault() {
    _chosen = {};
    _reset  = true;
    _dirty  = _hasCustom; // nothing to save when no override exists
    refreshPreview();
    refreshButtons();
}

void IconPickerDialog::refreshButtons() {
    _saveBtn->setEnabled(_dirty);
    // Offered while an override is installed or a new picture is pending —
    // either way there is something to fall back from.
    _defaultBtn->setVisible(_hasCustom || !_chosen.isNull());
    _defaultBtn->setEnabled(!_reset);
}

void IconPickerDialog::applyTheme() {
    AppDialog::applyTheme();
    if (_hint)
        _hint->setStyleSheet(QString("color: %1; font-size: %2px;")
                                 .arg(Th::qss(Th::c().text.secondary))
                                 .arg(Th::c().fonts.sm));
    if (_preview)
        _preview->update();
}

static QString droppedImagePath(const QMimeData *mime) {
    if (!mime || !mime->hasUrls())
        return {};
    for (const QUrl &u : mime->urls())
        if (u.isLocalFile())
            return u.toLocalFile();
    return {};
}

void IconPickerDialog::dragEnterEvent(QDragEnterEvent *e) {
    const auto *mime = e->mimeData();
    if (mime && (mime->hasImage() || !droppedImagePath(mime).isEmpty()))
        e->acceptProposedAction();
}

void IconPickerDialog::dropEvent(QDropEvent *e) {
    const auto *mime = e->mimeData();
    bool        ok   = false;
    if (const QString path = droppedImagePath(mime); !path.isEmpty())
        ok = loadFile(path);
    else if (mime && mime->hasImage())
        ok = loadImage(qvariant_cast<QImage>(mime->imageData()));
    if (ok)
        e->acceptProposedAction();
    else
        _hint->setText(tr("That file could not be read as an image."));
}
