// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Ctrl+R in the composer: search what was typed before and take one of the
// earlier prompts back into the editor — Claude Code's own Ctrl+R, as a list.
//
// A panel over the message list, just above the composer: the matches on top,
// newest at the bottom, and the search field under them, next to the composer.
// So ↑ (and Ctrl+R again) goes to older prompts, as ↑ does in the composer
// itself. Enter takes the selected prompt into the editor (it isn't sent), Esc
// or a click elsewhere closes the panel and leaves the draft alone.
//
// Generic: it searches whatever list it is given. The composer asks its
// history source (Backend::promptHistory), so the panel only opens where the
// service keeps such a history.
#pragma once

#include <QFrame>
#include <QList>
#include <QPair>
#include <QStringList>

class QLabel;
class QScrollArea;
class StyledLineEdit;

namespace HistorySearch {
// The entries that match `query`, as indices into `entries` (order kept):
// every word of the query somewhere in the entry, case-insensitively. An empty
// query matches everything.
QList<int>             filter(const QStringList &entries, const QString &query);
// Where the query's words are in `text` (start, length), in order, not overlapping.
QList<QPair<int, int>> matchRanges(const QString &text, const QString &query);
} // namespace HistorySearch

class HistorySearchList;

class HistorySearchPopup : public QFrame {
    Q_OBJECT
public:
    explicit HistorySearchPopup(QWidget *parent);

    // Show `entries` (newest first; repeats keep only the newest) filtered by
    // `query`, just above `anchor` (the composer, in parent coordinates), and
    // put the keyboard focus in the search field.
    void open(const QStringList &entries, const QString &query, const QRect &anchor);
    void dismiss();
    bool isOpen() const { return isVisible(); }

    // For tests and the composer.
    QString     query() const;
    QStringList matches() const; // newest first
    QString     selectedEntry() const;

signals:
    void picked(const QString &text); // Enter, or a click on a row
    // Esc: the composer takes the focus back. (A click elsewhere just closes
    // the panel, leaving the focus where it went.)
    void cancelled();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    friend class HistorySearchList;
    void refilter();
    void select(int match); // index into _matches
    void pick();
    void place();
    void applyTheme();

    StyledLineEdit    *_search = nullptr;
    QScrollArea       *_scroll = nullptr;
    HistorySearchList *_list   = nullptr;
    QLabel            *_empty  = nullptr;
    QStringList        _entries;
    QList<int>         _matches;      // into _entries, newest first
    int                _selected = 0; // into _matches
    QRect              _anchor;
};
