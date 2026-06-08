// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Search bar + results panel. Displayed as an animated overlay above the message list.
#pragma once

#include "backend/domain.h"
#include <QWidget>
#include <vector>

class QLineEdit;
class QListWidget;
class QPushButton;
class QLabel;
class QPropertyAnimation;
class Session;
class PopupTooltip;

class SearchWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int overlayAlpha READ overlayAlpha WRITE setOverlayAlpha)
public:
    explicit SearchWidget(QWidget *parent = nullptr);

    void setSession(Session *session);
    void focusInput();
    // Animated close — fades out then hides. Use hide() for instant programmatic close.
    void closeSearch();

signals:
    void resultSelected(ConversationId conv, Ts ts);
    void closeRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void    runSearch(const QString &query);
    void    populateResults(const std::vector<SearchResult> &results);
    void    navigateBy(int delta);
    void    activateSelected();
    void    applyTheme();
    QString resolveConvName(const SearchResult &r) const;
    QString resolvePreview(const TextWithEntities &t) const;

    int  overlayAlpha() const { return _overlayAlpha; }
    void setOverlayAlpha(int a) {
        _overlayAlpha = a;
        update();
    }

    Session            *_session           = nullptr;
    QWidget            *_card              = nullptr;
    QWidget            *_header            = nullptr;
    QLineEdit          *_queryEdit         = nullptr;
    QLabel             *_searchIconLabel   = nullptr;
    QPushButton        *_closeBtn          = nullptr;
    QListWidget        *_resultList        = nullptr;
    PopupTooltip       *_searchIconTooltip = nullptr;
    PopupTooltip       *_closeBtnTooltip   = nullptr;
    QPropertyAnimation *_overlayAnim       = nullptr;
    QPropertyAnimation *_cardAnim          = nullptr;
    int                 _overlayAlpha      = 0;
    int                 _selectedIdx       = -1;

    std::vector<SearchResult> _results;
};
