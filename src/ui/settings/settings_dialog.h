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
class QLineEdit;
class Dropdown;
class StyledButton;
class StyledLineEdit;
class UpdateChecker;
class ThemePreviewCard;
class LlmProvider;

class SettingsDialog : public QWidget {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent);

    void open();
    // Tab pages, in _tabs row order (buildPanel adds them in this sequence).
    enum class Page { Appearance = 0, Notifications, Ai, Storage, System, About };
    // open() with a specific tab pre-selected (e.g. the summary no-provider
    // notice deep-links to AI assistance).
    void openAt(Page page);
    void setUpdateChecker(UpdateChecker *checker);

    // Which sample notification the "Test" button fires; the int carried by
    // testNotificationRequested is one of these.
    enum class SampleNotif { Dm = 0, Channel = 1, Huddle = 2 };

signals:
    // Emitted when appearance settings are saved; carries the new relevantDays value.
    void appearanceChanged(int relevantDays);
    // Emitted when the 12h/24h preference (or language, which affects date
    // patterns) is saved, so timestamp-painting views can repaint.
    void timeFormatChanged();
    // Emitted when the threads display mode is saved (true = inline, false =
    // standalone panel), so the message list can switch how "View thread" behaves.
    void threadDisplayChanged(bool inlineThreads);
    // Emitted after conv/visitedAt is wiped so the conv list can re-seed from API data.
    void stateCleared();
    // Emitted when notification settings (incl. the global default level) are
    // saved, so the conv list and unread badges can re-resolve effective levels.
    void notificationsChanged();
    // Emitted when the "Test" button under Sample notifications is clicked; the
    // int is a SampleNotif value. MainWindow owns the actual delivery.
    void testNotificationRequested(int kind);
    // Emitted after the user saves personal Slack app credentials — they only
    // take effect on a fresh start, so MainWindow performs a clean restart.
    void restartRequested();

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
    void                   saveAppCredentials();
    void                   loadAppCredentials();
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
    QCheckBox    *_notifHuddles      = nullptr;
    QCheckBox    *_notifSound        = nullptr;
    QWidget      *_notifSoundRow     = nullptr;
    Dropdown     *_notifSoundChoice  = nullptr;
    StyledButton *_notifSoundPreview = nullptr;
    Dropdown     *_sampleNotifChoice = nullptr;
    StyledButton *_sampleNotifTest   = nullptr;

    // Appearance controls
    QSpinBox                 *_relevantDays     = nullptr;
    Dropdown                 *_language         = nullptr;
    QLabel                   *_langRestartNote  = nullptr;
    QRadioButton             *_time12           = nullptr;
    QRadioButton             *_time24           = nullptr;
    QRadioButton             *_threadStandalone = nullptr;
    QRadioButton             *_threadInline     = nullptr;
    QList<ThemePreviewCard *> _themeCards; // one per registry theme
    // Language the app actually started with — the restart note shows whenever
    // the combo selection differs from this, even across settings re-opens.
    QString                   _startupLanguage;

    // AI assistance controls
    struct AiProviderRow {
        LlmProvider    *provider      = nullptr;
        QLabel         *status        = nullptr;
        StyledButton   *oauthBtn      = nullptr;
        StyledButton   *disconnectBtn = nullptr;
        StyledLineEdit *keyEdit       = nullptr;
        StyledButton   *saveKeyBtn    = nullptr;
        // StyledButton (Link variant): a borderless text link. Plain widget text
        // is pixel-snapped, unlike rich-text QLabels which show inconsistent
        // stroke weight on fractionally-scaled displays.
        StyledButton   *keyLink       = nullptr;
    };
    QList<AiProviderRow> _aiRows;
    Dropdown            *_aiDefault  = nullptr;
    Dropdown            *_aiLanguage = nullptr;
    QLabel              *_aiError    = nullptr;

    // Storage controls
    QLabel   *_cacheSize = nullptr;
    QSpinBox *_cacheCap  = nullptr;

    // System / update controls
    UpdateChecker *_updateChecker = nullptr;
    QLabel        *_updateStatus  = nullptr;
    QLabel        *_lastChecked   = nullptr;
    StyledButton  *_checkBtn      = nullptr;
    QLabel        *_ramLabel      = nullptr;
    QTimer        *_ramTimer      = nullptr;

    // Personal Slack app credentials (System page)
    StyledLineEdit *_credClientId     = nullptr;
    StyledLineEdit *_credClientSecret = nullptr;
    StyledLineEdit *_credXapp         = nullptr;
    QLabel         *_credStatus       = nullptr;

    Dir    _resizeDir = Dir::None;
    QPoint _dragStart;
    QRect  _panelAtDrag;
};
