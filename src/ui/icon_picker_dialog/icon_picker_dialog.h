// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"

#include <QImage>
#include <QPixmap>

class QLabel;
class QPainter;
class QHBoxLayout;
class StyledButton;

// Shared "pick a local picture for X" dialog: a preview card, "Choose image…"
// with "Use default" beside it (a file chooser or a drop onto the card), a
// hint line and Save / Cancel, on a fixed-width card. Subclasses decide how bytes decode, how a
// picture is normalised and how the preview paints (WorkspaceIconDialog: the rail bubble;
// TrayIconDialog: a tray-like tile).
//
// After exec()/finished() == Accepted, at most one holds:
//   resetRequested()        — drop the override, show the default again
//   !chosenImage().isNull() — install this picture (decoded, not yet normalised)
// (Neither holds when a subclass option alone was changed — see markDirty().)
// The dialog changes nothing itself; the caller applies the result.
class IconPickerDialog : public AppDialog {
    Q_OBJECT
public:
    QImage chosenImage() const { return _chosen; }
    bool   resetRequested() const { return _reset; }

    // Decode `path` into the preview. False (and the preview unchanged) when
    // the file is not an image we can read.
    bool loadFile(const QString &path);
    // Same for pixels already in memory (a dropped/pasted picture).
    bool loadImage(const QImage &img);

protected:
    // Side of the square preview card paintPreview() fills.
    static constexpr int kPreviewSize = 96;

    struct Setup {
        QString title;        // card header
        QString hint;         // line under the chooser button
        QString chooserTitle; // file dialog title
    };
    // `current` is what shows now (null = the default); `hasCustom` whether an
    // override is installed (offers "Use default"). Subclasses call finish()
    // at the end of their constructor, after adding any options.
    IconPickerDialog(const Setup &setup, const QImage &current, bool hasCustom, QWidget *parent);
    void finish();

    virtual QImage decodeBytes(const QByteArray &bytes) const                            = 0;
    // A picture (chosen, or `current`) → what the preview shows.
    virtual QImage prepareImage(const QImage &img) const                                 = 0;
    // `icon` is prepareImage()'s result, or null for the default.
    virtual void   paintPreview(QPainter &p, const QRectF &r, const QPixmap &icon) const = 0;

    // Where subclass options go: the footer, left of Cancel / Save.
    QHBoxLayout *footerOptions() const { return _footerOptions; }
    // Re-run prepareImage() on what the preview shows (an option changed).
    void         refreshPreview();
    // Something other than the picture changed; enables Save.
    void         markDirty();

    void applyTheme() override;
    int  cardWidth(int availOverlayWidth) const override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    class Preview;

    void chooseFile();
    void useDefault();
    void refreshButtons();

    Setup  _setup;
    QImage _current;
    bool   _hasCustom = false;
    QImage _chosen; // null until a picture is loaded
    bool   _reset = false;
    bool   _dirty = false; // something to save

    Preview      *_preview       = nullptr;
    QLabel       *_hint          = nullptr;
    QHBoxLayout  *_footerOptions = nullptr;
    StyledButton *_chooseBtn     = nullptr;
    StyledButton *_defaultBtn    = nullptr;
    StyledButton *_saveBtn       = nullptr;
    StyledButton *_cancelBtn     = nullptr;
};
