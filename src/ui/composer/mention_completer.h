// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFrame>
#include <QString>
#include <functional>

class QVBoxLayout;
class QScrollArea;
class QWidget;
class Session;
class ImageCache;

// Floating autocomplete list for @user, #channel, :emoji: and /command triggers.
// Appears above the trigger word in the editor; auto-dismisses on Escape or
// when the editor loses focus. A plain child widget of the conversation panel
// (NOT a window) so it never takes focus and positions reliably on Wayland.
class MentionCompleter : public QFrame {
    Q_OBJECT
public:
    explicit MentionCompleter(QWidget *parent = nullptr);

    enum class Mode { User, Channel, Emoji };

    using Callback = std::function<void(const QString &insertText)>;

    // Show the completer with its bottom edge just above globalPos (pass the
    // global top-left of the trigger character so the popup hugs the word).
    // Each item: a plain display string + the text to insert on confirmation.
    // For slash commands set `command = true` and fill the rich fields below;
    // those rows render Slack-style (avatar + bold title with dim syntax + a
    // "source · description" subtitle) instead of a single text line.
    struct Item {
        QString display; // plain one-line label (users / channels / emoji)
        QString insert;  // text inserted into the editor on confirmation

        // Rich channel row (channel == true) ────────────────────────────────
        // Single line: a hashtag (or padlock, for private) icon + bold name,
        // styled like the @-mention rows. `title` holds the channel name.
        bool channel        = false;
        bool channelPrivate = false; // padlock icon instead of hashtag

        // Rich slash-command row (command == true) ─────────────────────────
        bool    command = false;
        QString title;         // bold first line, e.g. "/archive"
        QString usage;         // dim argument hint appended to the title
        QString source;        // bold subtitle prefix, e.g. "Slack" or "Giphy"
        bool    isApp = false; // prefixes the source with "App · "
        QString desc;          // dim subtitle text after the source
        QString iconUrl;       // app icon URL; empty → built-in Slack mark
    };
    void show(const QPoint &globalPos, const QList<Item> &items, Callback cb);
    void dismiss();

    // App-icon downloads for command rows. Optional; rows fall back to the
    // built-in Slack mark / generic bot glyph when unset or while loading.
    void setImageCache(ImageCache *cache) { _imgCache = cache; }

    // Keyboard navigation — call from editor's eventFilter.
    // Returns true if the event was consumed.
    bool handleKey(int key);

    bool isVisible() const;

private:
    void applyTheme();
    void rebuild(const QList<Item> &items);
    void selectRow(int row);
    void confirm();

    QScrollArea     *_scroll  = nullptr;
    QWidget         *_content = nullptr;
    QVBoxLayout     *_layout  = nullptr;
    QList<Item>      _items;
    int              _sel = 0;
    Callback         _cb;
    QList<QWidget *> _rows;
    ImageCache      *_imgCache = nullptr;
};
