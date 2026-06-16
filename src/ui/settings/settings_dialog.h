// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Modal settings overlay — full-window child widget that dims the background.
// No compositor required; uses Qt's backing-store alpha blending.
#pragma once

#include <QWidget>
#include <QPoint>
#include <QRect>
#include <QList>
#include <QTimer>
#include <QHideEvent>

class QFrame;
class QLabel;
class QListWidget;
class QStackedWidget;
class QCheckBox;
class QRadioButton;
class QPushButton;
class QSpinBox;
class QComboBox;
class QLineEdit;
class UpdateChecker;
class ThemePreviewCard;
class LlmProvider;

class SettingsDialog : public QWidget {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent);

    void open();
    void setUpdateChecker(UpdateChecker *checker);

signals:
    // Emitted when appearance settings are saved; carries the new relevantDays value.
    void appearanceChanged(int relevantDays);
    // Emitted when the 12h/24h preference (or language, which affects date
    // patterns) is saved, so timestamp-painting views can repaint.
    void timeFormatChanged();
    // Emitted after conv/visitedAt is wiped so the conv list can re-seed from API data.
    void stateCleared();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    enum class Dir { None, N, NE, E, SE, S, SW, W, NW };

    void                   buildPanel();
    QWidget               *buildAiPage();
    void                   refreshAiProviders();
    void                   applyTheme();
    void                   saveNotifications();
    void                   loadNotifications();
    void                   saveAppearance();
    void                   loadAppearance();
    void                   refreshCacheSize();
    void                   clearCache();
    void                   clearState();
    void                   refreshLastChecked();
    void                   refreshUpdateStatus();
    void                   updatePanelGeometry();
    Dir                    edgeAt(const QPoint &pos) const;
    static Qt::CursorShape cursorFor(Dir d);

    QFrame         *_panel = nullptr;
    QListWidget    *_tabs  = nullptr;
    QStackedWidget *_stack = nullptr;

    // Notification controls
    QCheckBox    *_notifEnabled      = nullptr;
    QRadioButton *_notifAll          = nullptr;
    QRadioButton *_notifMentions     = nullptr;
    QCheckBox    *_notifSound        = nullptr;
    QComboBox    *_notifSoundChoice  = nullptr;
    QPushButton  *_notifSoundPreview = nullptr;

    // Appearance controls
    QSpinBox                 *_relevantDays    = nullptr;
    QComboBox                *_language        = nullptr;
    QLabel                   *_langRestartNote = nullptr;
    QRadioButton             *_time12          = nullptr;
    QRadioButton             *_time24          = nullptr;
    QList<ThemePreviewCard *> _themeCards; // one per registry theme
    // Language the app actually started with — the restart note shows whenever
    // the combo selection differs from this, even across settings re-opens.
    QString                   _startupLanguage;

    // AI assistance controls
    struct AiProviderRow {
        LlmProvider *provider      = nullptr;
        QLabel      *status        = nullptr;
        QPushButton *oauthBtn      = nullptr;
        QPushButton *disconnectBtn = nullptr;
        QLineEdit   *keyEdit       = nullptr;
        QPushButton *saveKeyBtn    = nullptr;
        // QPushButton styled as a link: rich-text QLabels rasterize at
        // fractional pixel offsets and show inconsistent stroke weight on
        // fractionally-scaled displays; plain widget text is pixel-snapped.
        QPushButton *keyLink       = nullptr;
    };
    QList<AiProviderRow> _aiRows;
    QComboBox           *_aiDefault = nullptr;
    QLabel              *_aiError   = nullptr;

    // Storage controls
    QLabel   *_cacheSize = nullptr;
    QSpinBox *_cacheCap  = nullptr;

    // System / update controls
    UpdateChecker *_updateChecker = nullptr;
    QLabel        *_updateStatus  = nullptr;
    QLabel        *_lastChecked   = nullptr;
    QPushButton   *_checkBtn      = nullptr;
    QLabel        *_ramLabel      = nullptr;
    QTimer        *_ramTimer      = nullptr;

    Dir    _resizeDir = Dir::None;
    QPoint _dragStart;
    QRect  _panelAtDrag;
};
