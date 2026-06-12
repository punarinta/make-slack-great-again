// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/lifetime.h"

#include <QWidget>

class Session;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class CanvasEdit;

// Full-page channel-canvas editor shown in the content stack when the canvas
// tab is active. Notion-style: a big borderless title line over a rich-text
// body, autosaved a few seconds after typing stops.
//
// API asymmetry (Slack has no canvas read endpoint): content loads as the
// HTML served by the file's url_private and saves as canvas markdown via a
// whole-document canvases.edit replace, so exotic formatting can degrade on
// round-trip. There is no co-editing — a save overwrites concurrent edits.
class CanvasPage : public QWidget {
    Q_OBJECT
public:
    explicit CanvasPage(QWidget *parent = nullptr);

    void setSession(Session *session);

    // Show the conversation's canvas. Empty fileId = no canvas yet: opens a
    // blank editor and creates the canvas on the first save. knownTitle, when
    // non-empty, fills the title immediately while files.info is in flight.
    void open(ConversationId conv, const QString &fileId, const QString &knownTitle = {});

    // Push any pending edits now (tab switch / conversation switch / close).
    void flushPendingSave();

    void clear();

signals:
    // First save of a previously canvas-less conversation created the canvas.
    void canvasCreated(const QString &fileId);
    void canvasDeleted();
    // Canvas title learned (files.info) or edited by the user — for the tab.
    void titleChanged(const QString &title);

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void applyTheme();
    void loadContent();
    void applyRemoteHtml(const QString &html);
    void showMenu();
    void confirmDelete();
    void setBodyHtml(const QString &html);
    // Why the editor is read-only. NotAddressable is permanent (the canvas
    // can never be edited through the API); NoAccess clears if a later
    // files.info shows the canvas became visible.
    enum class ReadOnlyCause { None, NotAddressable, NoAccess };
    void setReadOnlyUi(ReadOnlyCause cause);

    Session       *_session = nullptr;
    ConversationId _conv;
    QString        _fileId;                 // empty = canvas not created yet
    QString        _permalink;              // for "Copy link"; empty until files.info lands
    QString        _lastHtml;               // remote HTML the section diff is computed against
    QString        _serverTitle;            // files.info title; identifies the title h1 in HTML
    bool           _baseRefetching = false; // base refresh in flight; saves are deferred
    ReadOnlyCause  _roCause        = ReadOnlyCause::None;
    bool           _loading        = false; // programmatic body changes; don't mark dirty
    bool           _bodyDirty      = false;
    bool           _titleDirty     = false;
    bool           _saving         = false;
    quint64        _openSeq        = 0; // invalidates in-flight loads on re-open

    QWidget     *_column    = nullptr;
    QLineEdit   *_title     = nullptr;
    CanvasEdit  *_body      = nullptr;
    QPushButton *_menuBtn   = nullptr;
    QLabel      *_roNotice  = nullptr;
    QTimer      *_saveTimer = nullptr;

    rpl::lifetime _lifetime;
};
