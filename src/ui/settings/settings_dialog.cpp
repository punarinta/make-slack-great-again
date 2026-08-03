// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "settings_dialog.h"
#include "theme_preview_card.h"
#include "ui/dropdown/dropdown.h"
#include "ui/icon_button/icon_button.h"
#include "ui/update_checker/update_checker.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "app_credentials.h"
#include "llm/llm_service.h"
#include "cache/cache_evictor.h"
#include "util/time_format.h"
#include "util/process_stats.h"
#include "util/sound_player.h"
#include "backend/slack/slack_auth.h"
#include "ui/session_import_dialog/session_import_dialog.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QShortcut>
#include <QResizeEvent>
#include <QFrame>
#include <QListWidget>
#include <QStackedWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QPushButton>
#include <QSettings>
#include <QGroupBox>
#include <QSpinBox>
#include <QLineEdit>
#include <algorithm>
#include <QDirIterator>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>

static constexpr int kPanelW    = 700;
static constexpr int kPanelH    = 540;
static constexpr int kPanelMinW = 480;
static constexpr int kPanelMinH = 360;
static constexpr int kEdge      = 7;

// Our scrollbar design: thin rounded handle, transparent track, no arrows —
// the same look used by the chats list and the canvas page. Shared so the
// settings scroll areas re-style on theme change via applyTheme().
static QString settingsScrollQss() {
    return QStringLiteral("QScrollArea { background: transparent; }") + Th::scrollBarQss();
}

// Wrap a settings page so it scrolls when it's taller than the panel instead of
// being squeezed (which clipped the theme cards on Windows, where the taller
// system font inflates every row past the fixed panel height). Transparent +
// frameless so the page looks identical when it does fit.
static QWidget *scrollWrap(QWidget *page) {
    auto *sa = new QScrollArea;
    sa->setObjectName("settingsScroll");
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->viewport()->setAutoFillBackground(false);
    sa->setStyleSheet(settingsScrollQss());
    sa->setWidget(page);
    // setWidget() force-enables autoFillBackground on the page, which would
    // paint the *palette's* light-grey window color over the themed panel —
    // invisible on the light themes, a light slab on dark ones.
    page->setAutoFillBackground(false);
    return sa;
}

SettingsDialog::SettingsDialog(QWidget *parent) : QWidget(parent) {
    setAutoFillBackground(false);
    setMouseTracking(true);
    setGeometry(parent->rect());
    parent->installEventFilter(this);
    buildPanel();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
    hide();
}

void SettingsDialog::openAt(Page page) {
    open();
    _tabs->setCurrentRow(static_cast<int>(page));
}

void SettingsDialog::open() {
    loadAppearance();
    loadNotifications();
    refreshAiProviders();
    if (_aiError)
        _aiError->clear();
    refreshCacheSize();
    {
        const QSignalBlocker block(_cacheCap); // don't save/sweep on load
        _cacheCap->setValue(CacheEvictor::capMb());
    }
    refreshLastChecked();
    refreshUpdateStatus();
    if (auto *btn = _panel->findChild<QPushButton *>("clearCacheBtn"))
        btn->setEnabled(true);
    if (auto *btn = _panel->findChild<QPushButton *>("clearStateBtn"))
        btn->setEnabled(true);
    setGeometry(parentWidget()->rect());
    updatePanelGeometry();
    _ramLabel->setText(tr("RAM used: %1").arg(ProcessStats::formatRss(ProcessStats::rssBytes())));
    _ramTimer->start();
    show();
    raise();
    _tabs->setFocus();
}

void SettingsDialog::hideEvent(QHideEvent *e) {
    _ramTimer->stop();
    QWidget::hideEvent(e);
}

// ── Panel construction ────────────────────────────────────────────────────────

void SettingsDialog::buildPanel() {
    _panel = new QFrame(this);
    _panel->setObjectName("settingsPanel");
    _panel->setMinimumSize(kPanelMinW, kPanelMinH);
    _panel->resize(kPanelW, kPanelH);

    auto *root = new QVBoxLayout(_panel);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    const auto &sp = Th::c().spacing;

    // ── Header ────────────────────────────────────────────────────────
    auto *header = new QWidget(_panel);
    header->setObjectName("settingsHeader");
    header->setFixedHeight(48);
    auto *hlay = new QHBoxLayout(header);
    hlay->setContentsMargins(sp.xl, 0, sp.lg, 0);

    auto *titleLabel = new QLabel(tr("Settings"), header);
    titleLabel->setObjectName("settingsTitleLabel");
    hlay->addWidget(titleLabel);
    hlay->addStretch();

    auto *closeBtn = new IconButton(QStringLiteral(":/ui/x.svg"), 28, 14, header);
    closeBtn->setObjectName("settingsCloseBtn");
    connect(closeBtn, &QPushButton::clicked, this, &SettingsDialog::hide);
    hlay->addWidget(closeBtn);
    root->addWidget(header);

    // ── Body ──────────────────────────────────────────────────────────
    auto *body = new QWidget(_panel);
    auto *blay = new QHBoxLayout(body);
    blay->setContentsMargins(0, 0, 0, 0);
    blay->setSpacing(0);

    _tabs = new QListWidget(body);
    _tabs->setFixedWidth(175);
    _tabs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _tabs->addItem(tr("Appearance"));
    _tabs->addItem(tr("Notifications"));
    _tabs->addItem(tr("AI assistance"));
    _tabs->addItem(tr("Storage"));
    _tabs->addItem(tr("System"));
    _tabs->addItem(tr("About"));
    _tabs->setCurrentRow(0);
    blay->addWidget(_tabs);

    _stack = new QStackedWidget(body);
    blay->addWidget(_stack, 1);

    connect(_tabs, &QListWidget::currentRowChanged, _stack, &QStackedWidget::setCurrentIndex);

    // ── Appearance page ───────────────────────────────────────────────
    auto *appearPage = new QWidget;
    auto *alay       = new QVBoxLayout(appearPage);
    alay->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
    alay->setSpacing(sp.xl);

    // ── Color theme ───────────────────────────────────────────────────
    auto *themeHeading = new QLabel(tr("Color theme"), appearPage);
    themeHeading->setObjectName("sectionHeading");
    alay->addWidget(themeHeading);

    auto *themeBox = new QGroupBox(appearPage);
    themeBox->setObjectName("themeBox");
    // Pin to the row's natural height. The cards are fixed-size; with the default
    // (Preferred) policy the page layout shrinks this box below them when vertical
    // space is tight — which happens on Windows, where the taller system font
    // inflates every other row — clipping the cards' captions. sizeHint() already
    // accounts for the platform's font, so Fixed gives exactly the room needed.
    themeBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *themeLayout = new QHBoxLayout(themeBox);
    themeLayout->setSpacing(sp.lg);
    themeLayout->setContentsMargins(0, 0, 0, 0);

    auto *themeGroup = new QButtonGroup(themeBox);
    themeGroup->setExclusive(true);
    for (const auto &info : Th::availableThemes()) {
        const QString name = info.id == QLatin1String("purple")     ? tr("Purple")
                             : info.id == QLatin1String("charcoal") ? tr("Charcoal")
                             : info.id == QLatin1String("blue")     ? tr("Blue")
                             : info.id == QLatin1String("green")    ? tr("Green")
                                                                    : info.id;
        auto         *card = new ThemePreviewCard(info.id, name, *info.theme, themeBox);
        themeGroup->addButton(card);
        themeLayout->addWidget(card);
        _themeCards.append(card);
        // Apply + persist instantly — cheap and trivially reversible, and the
        // dialog restyling doubles as a live preview.
        connect(card, &QAbstractButton::clicked, this, [card] {
            ThemeManager::instance().setThemeById(card->themeId());
        });
    }
    themeLayout->addStretch();

    // The card row is wider than the panel's content column now that there are
    // four themes — scroll it horizontally instead of letting the layout squeeze
    // the fixed-size cards. Named "settingsScroll" so applyTheme() restyles it
    // along with the page wraps.
    auto *themeScroll = new QScrollArea(appearPage);
    themeScroll->setObjectName("settingsScroll");
    themeScroll->setWidgetResizable(true);
    themeScroll->setFrameShape(QFrame::NoFrame);
    themeScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    themeScroll->viewport()->setAutoFillBackground(false);
    themeScroll->setStyleSheet(settingsScrollQss());
    themeScroll->setWidget(themeBox);
    themeBox->setAutoFillBackground(false); // see scrollWrap()
    // Vertical scrolling is off and the cards must never be squeezed, so pin the
    // area's height to the row plus room for the horizontal scrollbar (sizeHint
    // already accounts for the platform font in the captions).
    themeScroll->setFixedHeight(themeBox->sizeHint().height() + sp.lg);
    alay->addWidget(themeScroll);

    // ── Font size ─────────────────────────────────────────────────────
    auto *fontHeading = new QLabel(tr("Font size"), appearPage);
    fontHeading->setObjectName("sectionHeading");
    alay->addWidget(fontHeading);

    auto *fontBox = new QGroupBox(appearPage);
    fontBox->setObjectName("fontBox");
    auto *fontLayout = new QVBoxLayout(fontBox);
    fontLayout->setSpacing(sp.md);
    fontLayout->setContentsMargins(0, 0, 0, 0);

    _fontSmall  = new QRadioButton(tr("Small"), fontBox);
    _fontMedium = new QRadioButton(tr("Medium (default)"), fontBox);
    _fontLarge  = new QRadioButton(tr("Large"), fontBox);

    auto *fontGroup = new QButtonGroup(fontBox);
    fontGroup->addButton(_fontSmall, 0);
    fontGroup->addButton(_fontMedium, 1);
    fontGroup->addButton(_fontLarge, 2);

    fontLayout->addWidget(_fontSmall);
    fontLayout->addWidget(_fontMedium);
    fontLayout->addWidget(_fontLarge);
    alay->addWidget(fontBox);

    // ── Language ──────────────────────────────────────────────────────
    _startupLanguage = TimeFmt::language();

    auto *langHeading = new QLabel(tr("Language"), appearPage);
    langHeading->setObjectName("sectionHeading");
    alay->addWidget(langHeading);

    auto *langBox = new QGroupBox(appearPage);
    langBox->setObjectName("langBox");
    auto *langLayout = new QVBoxLayout(langBox);
    langLayout->setSpacing(sp.md);
    langLayout->setContentsMargins(0, 0, 0, 0);

    auto *langRow   = new QHBoxLayout;
    auto *langLabel = new QLabel(tr("App language"), langBox);
    langLabel->setObjectName("langLabel");
    langRow->addWidget(langLabel);

    _language = new Dropdown(langBox);
    _language->setSize(Dropdown::Size::Small);
    // Language names are intentionally not translated — each stays readable
    // to a speaker of that language regardless of the active locale.
    _language->addItem(tr("System default"), "system");
    _language->addItem("English", "en");
    _language->addItem("日本語", "ja");
    _language->setFixedWidth(180);
    langRow->addWidget(_language);
    langRow->addStretch();
    langLayout->addLayout(langRow);

    _langRestartNote = new QLabel(
        tr("The new language will be applied the next time MSGA starts.\n"
           "Time and date formats update immediately."),
        langBox
    );
    _langRestartNote->setObjectName("langRestartNote");
    _langRestartNote->setWordWrap(true);
    _langRestartNote->hide();
    langLayout->addWidget(_langRestartNote);

    connect(_language, &Dropdown::currentIndexChanged, this, [this] {
        _langRestartNote->setVisible(_language->currentData().toString() != _startupLanguage);
    });

    alay->addWidget(langBox);

    // ── Date/Time ─────────────────────────────────────────────────────
    auto *timeHeading = new QLabel(tr("Date/Time"), appearPage);
    timeHeading->setObjectName("sectionHeading");
    alay->addWidget(timeHeading);

    auto *timeBox = new QGroupBox(appearPage);
    timeBox->setObjectName("timeBox");
    auto *timeLayout = new QVBoxLayout(timeBox);
    timeLayout->setSpacing(sp.md);
    timeLayout->setContentsMargins(0, 0, 0, 0);

    _time12 = new QRadioButton(tr("12-hour clock (2:34 PM)"), timeBox);
    _time24 = new QRadioButton(tr("24-hour clock (14:34)"), timeBox);

    auto *timeGroup = new QButtonGroup(timeBox);
    timeGroup->addButton(_time12, 0);
    timeGroup->addButton(_time24, 1);

    timeLayout->addWidget(_time12);
    timeLayout->addWidget(_time24);
    alay->addWidget(timeBox);

    // ── Threads ───────────────────────────────────────────────────────
    auto *threadHeading = new QLabel(tr("Threads"), appearPage);
    threadHeading->setObjectName("sectionHeading");
    alay->addWidget(threadHeading);

    auto *threadBox = new QGroupBox(appearPage);
    threadBox->setObjectName("threadBox");
    auto *threadLayout = new QVBoxLayout(threadBox);
    threadLayout->setSpacing(sp.md);
    threadLayout->setContentsMargins(0, 0, 0, 0);

    _threadStandalone =
        new QRadioButton(tr("Standalone (open replies in a side panel)"), threadBox);
    _threadInline = new QRadioButton(tr("Inline (expand replies under the message)"), threadBox);

    auto *threadGroup = new QButtonGroup(threadBox);
    threadGroup->addButton(_threadStandalone, 0);
    threadGroup->addButton(_threadInline, 1);

    threadLayout->addWidget(_threadStandalone);
    threadLayout->addWidget(_threadInline);
    alay->addWidget(threadBox);

    // ── Conversations ─────────────────────────────────────────────────
    auto *sidebarHeading = new QLabel(tr("Conversations"), appearPage);
    sidebarHeading->setObjectName("sectionHeading");
    alay->addWidget(sidebarHeading);

    auto *sidebarBox = new QGroupBox(appearPage);
    sidebarBox->setObjectName("sidebarBox");
    auto *sidebarLayout = new QVBoxLayout(sidebarBox);
    sidebarLayout->setSpacing(sp.md);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);

    auto *daysRow    = new QHBoxLayout;
    auto *daysPrefix = new QLabel(tr("Show conversations active in the last"), sidebarBox);
    daysPrefix->setObjectName("daysPrefix");
    daysRow->addWidget(daysPrefix);

    _relevantDays = new QSpinBox(sidebarBox);
    _relevantDays->setRange(1, 365);
    _relevantDays->setSuffix(tr(" days"));
    _relevantDays->setFixedWidth(90);
    daysRow->addWidget(_relevantDays);
    daysRow->addStretch();
    sidebarLayout->addLayout(daysRow);

    auto *daysDesc = new QLabel(
        tr("Conversations with no activity in this period are hidden\n"
           "under an \"N more...\" row at the bottom of each section."),
        sidebarBox
    );
    daysDesc->setObjectName("daysDesc");
    daysDesc->setWordWrap(true);
    sidebarLayout->addWidget(daysDesc);

    alay->addWidget(sidebarBox);
    alay->addStretch();

    auto *aBtnRow = new QHBoxLayout;
    aBtnRow->addStretch();
    auto *aSaveBtn = new StyledButton(tr("Save"), StyledButton::Variant::Primary, appearPage);
    aSaveBtn->setMinimumWidth(80);
    connect(aSaveBtn, &QPushButton::clicked, this, [this] {
        saveAppearance();
        hide();
    });
    aBtnRow->addWidget(aSaveBtn);
    alay->addLayout(aBtnRow);

    _stack->addWidget(scrollWrap(appearPage));

    // ── Notifications page ────────────────────────────────────────────
    auto *notifPage = new QWidget;
    auto *nlay      = new QVBoxLayout(notifPage);
    nlay->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
    nlay->setSpacing(sp.xl);

    _notifEnabled = new QCheckBox(tr("Enable desktop notifications"), notifPage);
    nlay->addWidget(_notifEnabled);

    auto *levelBox = new QGroupBox(notifPage);
    levelBox->setObjectName("levelBox");
    auto *levelLayout = new QVBoxLayout(levelBox);
    levelLayout->setSpacing(sp.md);
    levelLayout->setContentsMargins(0, 0, 0, 0);

    _notifAll      = new QRadioButton(tr("All new messages"), levelBox);
    _notifMentions = new QRadioButton(tr("Direct messages and mentions only"), levelBox);

    auto *group = new QButtonGroup(levelBox);
    group->addButton(_notifAll, 0);
    group->addButton(_notifMentions, 1);

    levelLayout->addWidget(_notifAll);
    levelLayout->addWidget(_notifMentions);
    nlay->addWidget(levelBox);

    _notifHuddles = new QCheckBox(tr("Notify me when a huddle starts"), notifPage);
    nlay->addWidget(_notifHuddles);

    _notifSound = new QCheckBox(tr("Play a sound for notifications"), notifPage);
    nlay->addWidget(_notifSound);

    // Sound chooser: bundled chime + OS system sounds (enumerated lazily on
    // open), with a preview button. Populated in loadNotifications().
    _notifSoundRow = new QWidget(notifPage);
    auto *soundRow = new QHBoxLayout(_notifSoundRow);
    soundRow->setContentsMargins(0, 0, 0, 0);
    soundRow->setSpacing(sp.md);
    auto *soundLabel  = new QLabel(tr("Sound:"), _notifSoundRow);
    _notifSoundChoice = new Dropdown(_notifSoundRow);
    _notifSoundChoice->setSize(Dropdown::Size::Small);
    _notifSoundChoice->setMinimumWidth(220);
    _notifSoundPreview =
        new StyledButton(tr("Test"), StyledButton::Variant::Secondary, _notifSoundRow);
    _notifSoundPreview->setSize(StyledButton::Size::Small);
    soundRow->addWidget(soundLabel);
    soundRow->addWidget(_notifSoundChoice, 1);
    soundRow->addWidget(_notifSoundPreview);
    soundRow->addStretch();
    nlay->addWidget(_notifSoundRow);

    connect(_notifSoundPreview, &QPushButton::clicked, this, [this] {
        Sound::Player::instance().play(_notifSoundChoice->currentData().toString());
    });

    // Sample notifications: fire a representative notification so the user can
    // see how each kind looks with their current OS/daemon and settings. The
    // actual delivery lives in MainWindow, so we just emit the request.
    auto *sampleHeading = new QLabel(tr("Sample notifications"), notifPage);
    sampleHeading->setObjectName("sectionHeading");
    nlay->addWidget(sampleHeading);

    auto *sampleRow = new QHBoxLayout;
    sampleRow->setContentsMargins(0, 0, 0, 0);
    sampleRow->setSpacing(sp.md);
    _sampleNotifChoice = new Dropdown(notifPage);
    _sampleNotifChoice->addItem(tr("New DM"), int(SampleNotif::Dm));
    _sampleNotifChoice->addItem(tr("New channel message"), int(SampleNotif::Channel));
    _sampleNotifChoice->addItem(tr("New huddle"), int(SampleNotif::Huddle));
    _sampleNotifChoice->setSize(Dropdown::Size::Small);
    _sampleNotifChoice->setMinimumWidth(220);
    _sampleNotifTest = new StyledButton(tr("Test"), StyledButton::Variant::Secondary, notifPage);
    _sampleNotifTest->setSize(StyledButton::Size::Small);
    sampleRow->addWidget(_sampleNotifChoice, 1);
    sampleRow->addWidget(_sampleNotifTest);
    sampleRow->addStretch();
    nlay->addLayout(sampleRow);

    connect(_sampleNotifTest, &QPushButton::clicked, this, [this] {
        emit testNotificationRequested(_sampleNotifChoice->currentData().toInt());
    });

    // Disable level/sound when master toggle is off; the sound chooser also
    // depends on the "play a sound" checkbox.
    auto updateEnabled = [this, levelBox]() {
        const bool on      = _notifEnabled->isChecked();
        const bool soundOn = on && _notifSound->isChecked();
        levelBox->setEnabled(on);
        _notifHuddles->setEnabled(on);
        _notifSound->setEnabled(on);
        _notifSoundRow->setEnabled(soundOn);
        _sampleNotifChoice->setEnabled(on);
        _sampleNotifTest->setEnabled(on);
    };
    connect(_notifEnabled, &QCheckBox::toggled, this, updateEnabled);
    connect(_notifSound, &QCheckBox::toggled, this, updateEnabled);

    nlay->addStretch();

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *saveBtn = new StyledButton(tr("Save"), StyledButton::Variant::Primary, notifPage);
    saveBtn->setMinimumWidth(80);
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        saveNotifications();
        hide();
    });
    btnRow->addWidget(saveBtn);
    nlay->addLayout(btnRow);

    _stack->addWidget(scrollWrap(notifPage));

    // ── AI assistance page ────────────────────────────────────────────
    _stack->addWidget(scrollWrap(buildAiPage()));

    // ── Storage page ──────────────────────────────────────────────────
    auto *storagePage = new QWidget;
    auto *slay        = new QVBoxLayout(storagePage);
    slay->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
    slay->setSpacing(sp.xl);

    auto *cacheHeading = new QLabel(tr("Cache"), storagePage);
    cacheHeading->setObjectName("sectionHeading");
    slay->addWidget(cacheHeading);

    auto *sizeRow         = new QHBoxLayout;
    auto *sizePrefixLabel = new QLabel(tr("Cache size:"), storagePage);
    sizePrefixLabel->setObjectName("sizePrefixLabel");
    sizeRow->addWidget(sizePrefixLabel);

    _cacheSize = new QLabel("–", storagePage);
    sizeRow->addWidget(_cacheSize);
    sizeRow->addStretch();
    slay->addLayout(sizeRow);

    auto *cacheDesc = new QLabel(
        tr("Conversations, user names, message history, and image thumbnails\n"
           "stored locally to speed up startup."),
        storagePage
    );
    cacheDesc->setObjectName("cacheDesc");
    cacheDesc->setWordWrap(true);
    slay->addWidget(cacheDesc);

    auto *capRow    = new QHBoxLayout;
    auto *capPrefix = new QLabel(tr("Limit cache to"), storagePage);
    capPrefix->setObjectName("capPrefix");
    capRow->addWidget(capPrefix);

    _cacheCap = new QSpinBox(storagePage);
    _cacheCap->setRange(50, 10240);
    _cacheCap->setSuffix(tr(" MB"));
    _cacheCap->setFixedWidth(110);
    capRow->addWidget(_cacheCap);
    capRow->addStretch();
    slay->addLayout(capRow);

    auto *capDesc = new QLabel(
        tr("When the cache grows past this limit, the least recently\n"
           "viewed images are deleted first."),
        storagePage
    );
    capDesc->setObjectName("capDesc");
    capDesc->setWordWrap(true);
    slay->addWidget(capDesc);

    connect(_cacheCap, &QSpinBox::valueChanged, this, [](int mb) {
        CacheEvictor::setCapMb(mb);
        CacheEvictor::instance()->schedule();
    });
    connect(CacheEvictor::instance(), &CacheEvictor::finished, this, [this] {
        if (isVisible())
            refreshCacheSize();
    });

    auto *clearCacheRow = new QHBoxLayout;
    auto *clearCacheBtn =
        new StyledButton(tr("Clear cache"), StyledButton::Variant::Danger, storagePage);
    clearCacheBtn->setObjectName("clearCacheBtn");
    clearCacheBtn->setSize(StyledButton::Size::Small);
    connect(clearCacheBtn, &QPushButton::clicked, this, &SettingsDialog::clearCache);
    clearCacheRow->addWidget(clearCacheBtn);
    clearCacheRow->addStretch();
    slay->addLayout(clearCacheRow);

    // ── State section ─────────────────────────────────────────────────
    slay->addSpacing(sp.lg); // breathing room between the Cache and State blocks
    auto *stateHeading = new QLabel(tr("State"), storagePage);
    stateHeading->setObjectName("sectionHeading");
    slay->addWidget(stateHeading);

    auto *stateDesc = new QLabel(
        tr("Sidebar visit history used to decide which conversations are shown.\n"
           "Clear this to let the app re-analyse activity from scratch on next load."),
        storagePage
    );
    stateDesc->setObjectName("stateDesc");
    stateDesc->setWordWrap(true);
    slay->addWidget(stateDesc);

    auto *clearStateRow = new QHBoxLayout;
    auto *clearStateBtn =
        new StyledButton(tr("Clear state"), StyledButton::Variant::Danger, storagePage);
    clearStateBtn->setObjectName("clearStateBtn");
    clearStateBtn->setSize(StyledButton::Size::Small);
    connect(clearStateBtn, &QPushButton::clicked, this, &SettingsDialog::clearState);
    clearStateRow->addWidget(clearStateBtn);
    clearStateRow->addStretch();
    slay->addLayout(clearStateRow);

    slay->addStretch();

    _stack->addWidget(scrollWrap(storagePage));

    // ── System page ───────────────────────────────────────────────────
    auto *sysPage = new QWidget;
    auto *sylay   = new QVBoxLayout(sysPage);
    sylay->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
    sylay->setSpacing(sp.xl);

    // ── Version section ───────────────────────────────────────────────
    auto *versionHeading = new QLabel(tr("Version"), sysPage);
    versionHeading->setObjectName("sectionHeading");
    sylay->addWidget(versionHeading);

    const QString buildTs =
        QString(AppCredentials::buildTimestamp).replace('T', ' ').chopped(1); // drop trailing Z
    auto *verLabel =
        new QLabel(tr("Version %1, built %2").arg(AppCredentials::version).arg(buildTs), sysPage);
    verLabel->setObjectName("verLabel");
    sylay->addWidget(verLabel);

    auto *updBox = new QGroupBox(sysPage);
    updBox->setObjectName("updBox");
    auto *updLayout = new QVBoxLayout(updBox);
    updLayout->setSpacing(sp.md);
    updLayout->setContentsMargins(0, 0, 0, 0);

    auto *checkRow = new QHBoxLayout;
    _checkBtn = new StyledButton(tr("Check for updates"), StyledButton::Variant::Primary, updBox);
    _checkBtn->setSize(StyledButton::Size::Small);
    checkRow->addWidget(_checkBtn);
    checkRow->addStretch();
    updLayout->addLayout(checkRow);

    _updateStatus = new QLabel("", updBox);
    _updateStatus->setWordWrap(true);
    updLayout->addWidget(_updateStatus);

    _lastChecked = new QLabel("", updBox);
    updLayout->addWidget(_lastChecked);

    sylay->addWidget(updBox);

    // ── Slack connection section ──────────────────────────────────────
    // Global either/or switch: connect with the user's own Slack session (cookie)
    // or with app keys (OAuth + Socket Mode). Session mode turns app keys and
    // Socket Mode completely off — no shared-key contention, but no live push
    // (new messages arrive by polling). See slack::connectionMode().
    auto *connHeading = new QLabel(tr("Slack connection"), sysPage);
    connHeading->setObjectName("sectionHeading");
    sylay->addWidget(connHeading);

    auto *connDesc = new QLabel(tr("Choose how msga connects to Slack."), sysPage);
    connDesc->setObjectName("credDesc");
    connDesc->setWordWrap(true);
    sylay->addWidget(connDesc);

    // The switch — two prominent radio options.
    auto *modeBox = new QGroupBox(sysPage);
    modeBox->setObjectName("credBox");
    auto *modeLay = new QVBoxLayout(modeBox);
    modeLay->setSpacing(sp.md);
    modeLay->setContentsMargins(sp.lg, sp.lg, sp.lg, sp.lg);
    _modeSession = new QRadioButton(
        tr("Slack session — no app keys, uses your own account's limits"), modeBox
    );
    _modeAppKeys =
        new QRadioButton(tr("Slack app keys — OAuth sign-in with live message push"), modeBox);
    auto *modeGroup = new QButtonGroup(this);
    modeGroup->addButton(_modeSession);
    modeGroup->addButton(_modeAppKeys);
    modeLay->addWidget(_modeSession);
    modeLay->addWidget(_modeAppKeys);
    _modeRestartNote = new QLabel(tr("Restart msga to apply this change."), modeBox);
    _modeRestartNote->setObjectName("credDesc");
    _modeRestartNote->setWordWrap(true);
    _modeRestartNote->setVisible(false);
    modeLay->addWidget(_modeRestartNote);
    sylay->addWidget(modeBox);

    // ── Session sub-section (shown in session mode) ──
    _sessionBox   = new QWidget(sysPage);
    auto *sessLay = new QVBoxLayout(_sessionBox);
    sessLay->setContentsMargins(0, 0, 0, 0);
    sessLay->setSpacing(sp.sm);
    auto *sessDesc = new QLabel(
        tr("Add a workspace using your existing Slack session. New messages arrive by "
           "polling — there's no live push in this mode."),
        _sessionBox
    );
    sessDesc->setObjectName("credDesc");
    sessDesc->setWordWrap(true);
    sessLay->addWidget(sessDesc);
    auto *importRow = new QHBoxLayout;
    auto *importBtn =
        new StyledButton(tr("Import Slack session…"), StyledButton::Variant::Primary, _sessionBox);
    importBtn->setSize(StyledButton::Size::Small);
    connect(importBtn, &QPushButton::clicked, this, &SettingsDialog::openSessionImport);
    importRow->addWidget(importBtn);
    importRow->addStretch();
    sessLay->addLayout(importRow);

    // If the user still has app-key (OAuth) Slack workspaces, offer one-click
    // conversion of them to session auth (reuses the one session cookie).
    int oauthSlackCount = 0;
    for (const auto &key : TokenStore::workspaceKeys()) {
        if (key.service != Service::Slack)
            continue;
        const auto rec = TokenStore::loadWorkspace(key);
        if (rec && slack::fromRecord(*rec).cookie.isEmpty())
            ++oauthSlackCount;
    }
    if (oauthSlackCount > 0) {
        auto *migDesc = new QLabel(
            tr("You still have %n Slack workspace(s) on app keys. Convert them to session "
               "so no workspace uses Socket Mode.",
               nullptr,
               oauthSlackCount),
            _sessionBox
        );
        migDesc->setObjectName("credDesc");
        migDesc->setWordWrap(true);
        sessLay->addWidget(migDesc);
        auto *migRow = new QHBoxLayout;
        auto *migBtn = new StyledButton(
            tr("Convert them to session"), StyledButton::Variant::Secondary, _sessionBox
        );
        migBtn->setSize(StyledButton::Size::Small);
        connect(migBtn, &QPushButton::clicked, this, [this] {
            emit migrateSlackToSessionRequested();
        });
        migRow->addWidget(migBtn);
        migRow->addStretch();
        sessLay->addLayout(migRow);
    }
    sylay->addWidget(_sessionBox);

    // ── App-keys sub-section (shown in app-keys mode) ──
    // Lets prebuilt-app users run their own Slack app instead of sharing the
    // build's app keys (which causes the parallel-usage connection eviction).
    _appKeysBox = new QWidget(sysPage);
    auto *akLay = new QVBoxLayout(_appKeysBox);
    akLay->setContentsMargins(0, 0, 0, 0);
    akLay->setSpacing(sp.md);

    auto *credDesc = new QLabel(
        tr("Run your own Slack app so you don't share connection keys with other "
           "devices and users. Leave a field empty to use the built-in default."),
        _appKeysBox
    );
    credDesc->setObjectName("credDesc");
    credDesc->setWordWrap(true);
    akLay->addWidget(credDesc);

    auto *credLink = new StyledButton(
        tr("How to create your Slack app…"), StyledButton::Variant::Link, _appKeysBox
    );
    connect(credLink, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(
            "https://github.com/punarinta/make-slack-great-again/blob/master/docs/SETUP_SLACK.md"
        )));
    });
    auto *credLinkRow = new QHBoxLayout;
    credLinkRow->addWidget(credLink);
    credLinkRow->addStretch();
    akLay->addLayout(credLinkRow);

    auto *credBox = new QGroupBox(_appKeysBox);
    credBox->setObjectName("credBox");
    auto *credLayout = new QVBoxLayout(credBox);
    credLayout->setSpacing(sp.sm);
    // Keep the framed box, but inset the fields so they don't touch its border.
    credLayout->setContentsMargins(sp.lg, sp.lg, sp.lg, sp.lg);

    const auto addCredField =
        [&](const QString &label, const QString &placeholder, bool secret) -> StyledLineEdit * {
        auto *fieldLabel = new QLabel(label, credBox);
        credLayout->addWidget(fieldLabel);
        auto *edit = new StyledLineEdit(credBox);
        edit->setSize(StyledLineEdit::Size::Small);
        edit->setPlaceholderText(placeholder);
        if (secret)
            edit->lineEdit()->setEchoMode(QLineEdit::Password);
        credLayout->addWidget(edit);
        return edit;
    };

    _credClientId     = addCredField(tr("Client ID"), tr("e.g. 1234567890.1234567890"), false);
    _credClientSecret = addCredField(tr("Client secret"), tr("Paste your client secret"), true);
    _credXapp         = addCredField(tr("App-level token"), tr("Paste your xapp- token"), true);

    _credStatus = new QLabel(credBox);
    _credStatus->setObjectName("credStatus");
    _credStatus->setWordWrap(true);
    credLayout->addWidget(_credStatus);

    auto *credSaveRow = new QHBoxLayout;
    auto *credSaveBtn =
        new StyledButton(tr("Save and restart"), StyledButton::Variant::Primary, credBox);
    credSaveBtn->setSize(StyledButton::Size::Small);
    connect(credSaveBtn, &QPushButton::clicked, this, &SettingsDialog::saveAppCredentials);
    credSaveRow->addWidget(credSaveBtn);
    credSaveRow->addStretch();
    credLayout->addLayout(credSaveRow);

    akLay->addWidget(credBox);
    sylay->addWidget(_appKeysBox);

    // Initialize the switch from the persisted mode, then wire the toggle. Changing
    // the mode persists immediately and shows a "restart to apply" note (the socket
    // gate + backends are decided at launch, same convention as the language note).
    _buildingSlackMode  = true;
    _startupSessionMode = (slack::connectionMode() == slack::ConnectionMode::Session);
    (_startupSessionMode ? _modeSession : _modeAppKeys)->setChecked(true);
    _buildingSlackMode = false;
    updateSlackModeUi();
    connect(modeGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton *, bool) {
        if (_buildingSlackMode)
            return;
        slack::setConnectionMode(
            _modeSession->isChecked() ? slack::ConnectionMode::Session
                                      : slack::ConnectionMode::AppKeys
        );
        updateSlackModeUi();
    });
    loadAppCredentials();

    // ── Memory section ────────────────────────────────────────────────
    auto *memoryHeading = new QLabel(tr("Memory"), sysPage);
    memoryHeading->setObjectName("sectionHeading");
    sylay->addWidget(memoryHeading);

    auto *memBox = new QGroupBox(sysPage);
    memBox->setObjectName("memBox");
    auto *memLayout = new QVBoxLayout(memBox);
    memLayout->setSpacing(sp.sm);
    memLayout->setContentsMargins(0, 0, 0, 0);

    _ramLabel = new QLabel(sysPage);
    _ramLabel->setObjectName("ramLabel");
    memLayout->addWidget(_ramLabel);

    sylay->addWidget(memBox);
    sylay->addStretch();

    _ramTimer = new QTimer(this);
    _ramTimer->setInterval(5000);
    connect(_ramTimer, &QTimer::timeout, this, [this] {
        _ramLabel->setText(
            tr("RAM used: %1").arg(ProcessStats::formatRss(ProcessStats::rssBytes()))
        );
    });

    _stack->addWidget(scrollWrap(sysPage));

    // ── About page ────────────────────────────────────────────────────
    auto *aboutPage = new QWidget;
    auto *ablay     = new QVBoxLayout(aboutPage);
    ablay->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
    ablay->setSpacing(sp.xl);

    // ── License & copyright ───────────────────────────────────────────
    auto *licenseHeading = new QLabel(tr("License"), aboutPage);
    licenseHeading->setObjectName("sectionHeading");
    ablay->addWidget(licenseHeading);

    auto *licenseLabel = new QLabel(
        tr("MSGA — Make Slack Great Again\n"
           "Copyright © 2026 Vladimir Osipov\n\n"
           "This program is free software: you can redistribute it and/or modify "
           "it under the terms of the GNU General Public License as published by "
           "the Free Software Foundation, either version 3 of the License, or "
           "(at your option) any later version (GPL-3.0-or-later)."),
        aboutPage
    );
    licenseLabel->setObjectName("aboutLicense");
    licenseLabel->setWordWrap(true);
    ablay->addWidget(licenseLabel);

    auto *licenseLink =
        new StyledButton(tr("View full license"), StyledButton::Variant::Link, aboutPage);
    connect(licenseLink, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl("https://github.com/punarinta/make-slack-great-again/blob/master/LICENSE")
        );
    });
    auto *licenseLinkRow = new QHBoxLayout;
    licenseLinkRow->addWidget(licenseLink);
    licenseLinkRow->addStretch();
    ablay->addLayout(licenseLinkRow);

    // ── Contact ───────────────────────────────────────────────────────
    auto *contactHeading = new QLabel(tr("Contact"), aboutPage);
    contactHeading->setObjectName("sectionHeading");
    ablay->addWidget(contactHeading);

    auto *contactLabel = new QLabel(aboutPage);
    contactLabel->setObjectName("aboutContact");
    contactLabel->setText(tr("Questions or feedback: %1")
                              .arg("<a href=\"mailto:vladimir@msga.app\">vladimir@msga.app</a>"));
    contactLabel->setTextFormat(Qt::RichText);
    contactLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    contactLabel->setOpenExternalLinks(true);
    ablay->addWidget(contactLabel);

    // ── Report a bug ──────────────────────────────────────────────────
    auto *bugHeading = new QLabel(tr("Found a bug?"), aboutPage);
    bugHeading->setObjectName("sectionHeading");
    ablay->addWidget(bugHeading);

    auto *bugDesc =
        new QLabel(tr("Report it on GitHub so it can be tracked and fixed."), aboutPage);
    bugDesc->setObjectName("aboutBugDesc");
    bugDesc->setWordWrap(true);
    ablay->addWidget(bugDesc);

    auto *bugRow = new QHBoxLayout;
    auto *bugBtn = new StyledButton(tr("Report a bug"), StyledButton::Variant::Danger, aboutPage);
    bugBtn->setSize(StyledButton::Size::Small);
    connect(bugBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl("https://github.com/punarinta/make-slack-great-again/issues")
        );
    });
    bugRow->addWidget(bugBtn);
    bugRow->addStretch();
    ablay->addLayout(bugRow);

    ablay->addStretch();

    _stack->addWidget(scrollWrap(aboutPage));
    root->addWidget(body, 1);

    auto *esc = new QShortcut(Qt::Key_Escape, _panel);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, &SettingsDialog::hide);

    updatePanelGeometry();
}

QWidget *SettingsDialog::buildAiPage() {
    auto &svc = LlmService::instance();

    auto       *page = new QWidget;
    const auto &sp   = Th::c().spacing;
    auto       *lay  = new QVBoxLayout(page);
    lay->setContentsMargins(sp.xxl, sp.xl, sp.xxl, sp.xl);
    lay->setSpacing(sp.xl);

    auto *desc = new QLabel(
        tr("Connect an AI provider to enable assistant features.\n"
           "Create an API key in your own provider account and paste it below —\n"
           "it is stored on this computer and sent only to that provider."),
        page
    );
    desc->setObjectName("aiDesc");
    desc->setWordWrap(true);

    auto *providerHeading = new QLabel(tr("AI provider"), page);
    providerHeading->setObjectName("sectionHeading");
    lay->addWidget(providerHeading);

    lay->addWidget(desc);

    // Default provider selector
    auto *defRow   = new QHBoxLayout;
    auto *defLabel = new QLabel(tr("Default:"), page);
    defLabel->setObjectName("aiDefaultLabel");
    defRow->addWidget(defLabel);

    _aiDefault = new Dropdown(page);
    _aiDefault->setSize(Dropdown::Size::Small);
    for (auto *p : svc.providers())
        _aiDefault->addItem(p->displayName(), p->id());
    defRow->addWidget(_aiDefault);
    defRow->addStretch();
    lay->addLayout(defRow);

    connect(_aiDefault, &Dropdown::currentIndexChanged, this, [this](int idx) {
        if (idx >= 0)
            LlmService::instance().setDefaultProviderId(_aiDefault->currentData().toString());
    });

    // One section per provider (Anthropic, OpenAI).
    for (auto *p : svc.providers()) {
        auto *heading = new QLabel(p->displayName(), page);
        heading->setObjectName("sectionHeading");
        lay->addWidget(heading);

        auto *box = new QGroupBox(page);
        box->setObjectName("aiBox");
        auto *bl = new QVBoxLayout(box);
        bl->setSpacing(sp.md);
        bl->setContentsMargins(0, 0, 0, 0);

        AiProviderRow row;
        row.provider = p;

        row.status = new QLabel(box);
        row.status->setObjectName("aiStatus");
        bl->addWidget(row.status);

        auto *btnRow = new QHBoxLayout;
        row.oauthBtn = new StyledButton(tr("Connect (OAuth)"), StyledButton::Variant::Primary, box);
        row.oauthBtn->setSize(StyledButton::Size::Small);
        btnRow->addWidget(row.oauthBtn);

        row.disconnectBtn = new StyledButton(tr("Disconnect"), StyledButton::Variant::Danger, box);
        row.disconnectBtn->setSize(StyledButton::Size::Small);
        btnRow->addWidget(row.disconnectBtn);
        btnRow->addStretch();
        bl->addLayout(btnRow);

        auto *keyRow = new QHBoxLayout;
        row.keyEdit  = new StyledLineEdit(box);
        row.keyEdit->setSize(StyledLineEdit::Size::Small);
        row.keyEdit->setPlaceholderText(tr("Paste your API key"));
        row.keyEdit->lineEdit()->setEchoMode(QLineEdit::Password);
        keyRow->addWidget(row.keyEdit, 1);

        row.saveKeyBtn = new StyledButton(tr("Save key"), StyledButton::Variant::Primary, box);
        row.saveKeyBtn->setSize(StyledButton::Size::Small);
        keyRow->addWidget(row.saveKeyBtn);
        bl->addLayout(keyRow);

        auto *keyLink = new StyledButton(
            tr("Get an API key from %1…").arg(p->displayName()), StyledButton::Variant::Link, box
        );
        connect(keyLink, &QPushButton::clicked, this, [p] {
            QDesktopServices::openUrl(QUrl(p->apiKeyUrl()));
        });
        auto *keyLinkRow = new QHBoxLayout;
        keyLinkRow->addWidget(keyLink);
        keyLinkRow->addStretch();
        bl->addLayout(keyLinkRow);
        row.keyLink = keyLink;

        connect(row.oauthBtn, &QPushButton::clicked, this, [this, p] {
            _aiError->clear();
            p->connectOAuth();
        });
        connect(row.disconnectBtn, &QPushButton::clicked, this, [this, p] {
            _aiError->clear();
            p->disconnectAccount();
        });
        const auto saveKey = [this, p, keyEdit = row.keyEdit] {
            _aiError->clear();
            p->connectApiKey(keyEdit->text());
            keyEdit->clear();
        };
        connect(row.saveKeyBtn, &QPushButton::clicked, this, saveKey);
        connect(row.keyEdit, &StyledLineEdit::returnPressed, this, saveKey);

        connect(p, &LlmProvider::authStateChanged, this, &SettingsDialog::refreshAiProviders);
        connect(p, &LlmProvider::authFailed, this, [this, p](const QString &reason) {
            _aiError->setText(tr("%1: %2").arg(p->displayName(), reason));
        });

        _aiRows.append(row);
        lay->addWidget(box);
    }

    // ── Your language ─────────────────────────────────────────────────
    auto *langHeading = new QLabel(tr("Your language"), page);
    langHeading->setObjectName("sectionHeading");
    lay->addWidget(langHeading);

    auto *langDesc = new QLabel(
        tr("AI features address you in this language.\n"
           "It follows the app language until you pick one here."),
        page
    );
    langDesc->setObjectName("aiDesc");
    langDesc->setWordWrap(true);
    lay->addWidget(langDesc);

    auto *aiLangRow   = new QHBoxLayout;
    auto *aiLangLabel = new QLabel(tr("Native language:"), page);
    aiLangLabel->setObjectName("aiDefaultLabel");
    aiLangRow->addWidget(aiLangLabel);

    _aiLanguage = new Dropdown(page);
    _aiLanguage->setSize(Dropdown::Size::Small);
    // Language names are intentionally not translated — each stays readable
    // to a speaker of that language regardless of the active locale.
    static const struct {
        const char *code;
        const char *name;
    } kAiLanguages[] = {
        {"de", "Deutsch"},
        {"en", "English"},
        {"es", "Español"},
        {"fr", "Français"},
        {"hi", "हिन्दी"},
        {"it", "Italiano"},
        {"ja", "日本語"},
        {"ko", "한국어"},
        {"nl", "Nederlands"},
        {"pl", "Polski"},
        {"pt", "Português"},
        {"ru", "Русский"},
        {"sv", "Svenska"},
        {"tr", "Türkçe"},
        {"uk", "Українська"},
        {"zh", "中文"},
    };
    for (const auto &l : kAiLanguages)
        _aiLanguage->addItem(QString::fromUtf8(l.name), QString::fromUtf8(l.code));
    _aiLanguage->setFixedWidth(180);
    aiLangRow->addWidget(_aiLanguage);
    aiLangRow->addStretch();
    lay->addLayout(aiLangRow);

    connect(_aiLanguage, &Dropdown::currentIndexChanged, this, [this](int idx) {
        if (idx >= 0)
            LlmService::instance().setNativeLanguage(_aiLanguage->currentData().toString());
    });

    _aiError = new QLabel(page);
    _aiError->setObjectName("aiError");
    _aiError->setWordWrap(true);
    lay->addWidget(_aiError);
    lay->addStretch();

    refreshAiProviders();
    return page;
}

void SettingsDialog::refreshAiProviders() {
    for (const auto &row : _aiRows) {
        const auto state      = row.provider->authState();
        const bool connected  = state == LlmProvider::AuthState::Connected;
        const bool connecting = state == LlmProvider::AuthState::Connecting;

        if (connected) {
            row.status->setText(
                row.provider->authMethod() == LlmProvider::AuthMethod::OAuth
                    ? tr("Connected as %1").arg(row.provider->accountLabel())
                    : tr("Connected with API key (%1)").arg(row.provider->accountLabel())
            );
        } else if (connecting) {
            row.status->setText(tr("Waiting for browser sign-in…"));
        } else {
            row.status->setText(tr("Not connected"));
        }

        row.oauthBtn->setVisible(!connected && row.provider->supportsOAuth());
        row.oauthBtn->setEnabled(!connecting);
        row.keyEdit->setVisible(!connected);
        row.saveKeyBtn->setVisible(!connected);
        row.keyLink->setVisible(!connected);
        row.disconnectBtn->setVisible(connected);
    }

    // Sync the default-provider combo without re-triggering the save.
    const QString def = LlmService::instance().defaultProviderId();
    int           idx = _aiDefault->findData(def);
    if (idx < 0)
        idx = 0;
    const QSignalBlocker blocker(_aiDefault);
    _aiDefault->setCurrentIndex(idx);

    // Same for the native-language combo. Its effective value tracks the UI
    // language until the user ever picks one, so it must not be written back
    // on load; English stands in when the resolved language isn't offered.
    int langIdx = _aiLanguage->findData(LlmService::instance().nativeLanguage());
    if (langIdx < 0)
        langIdx = std::max(0, _aiLanguage->findData(QStringLiteral("en")));
    const QSignalBlocker langBlocker(_aiLanguage);
    _aiLanguage->setCurrentIndex(langIdx);
}

void SettingsDialog::applyTheme() {
    const auto &th = Th::c();

    // Tab pages scroll with our thin rounded scrollbar (like the chats list);
    // re-style live so a theme switch from the Appearance tab updates them too.
    const QString scrollQss = settingsScrollQss();
    for (auto *sa : _panel->findChildren<QScrollArea *>("settingsScroll"))
        sa->setStyleSheet(scrollQss);

    // Panel frame
    _panel->setStyleSheet(QString(
                              "QFrame#settingsPanel {"
                              "  background: %1;"
                              "  border-radius: 8px;"
                              "  border: 1px solid %2;"
                              "}"
    )
                              .arg(Th::qss(th.surface.raised), Th::qss(th.divider.strong)));

    // Header
    if (auto *w = _panel->findChild<QWidget *>("settingsHeader")) {
        w->setStyleSheet(QString(
                             "background: %1;"
                             "border-bottom: 1px solid %2;"
                             "border-top-left-radius: 8px;"
                             "border-top-right-radius: 8px;"
        )
                             .arg(Th::qss(th.surface.highlight), Th::qss(th.divider.def)));
    }
    if (auto *w = _panel->findChild<QLabel *>("settingsTitleLabel")) {
        w->setStyleSheet(QString(
                             "font-size: %1px; font-weight: 600; color: %2;"
                             "background: transparent; border: none;"
        )
                             .arg(th.fonts.lg)
                             .arg(Th::qss(th.text.primary)));
    }
    // settingsCloseBtn (IconButton) self-themes.

    // Tabs list
    _tabs->setStyleSheet(
        QString(
            "QListWidget {"
            "  background: %1;"
            "  border: none;"
            "  border-right: 1px solid %2;"
            "  border-bottom-left-radius: 8px;"
            "  outline: none;"
            "  padding: 8px 0;"
            "}"
            "QListWidget::item {"
            "  padding: 9px 14px;"
            "  color: %3;"
            "  font-size: %4px;"
            "  border-radius: 4px;"
            "  margin: 1px 6px;"
            "}"
            "QListWidget::item:selected {"
            "  background: %5;"
            "  color: %3;"
            "}"
            "QListWidget::item:hover:!selected {"
            "  background: %6;"
            "}"
        )
            .arg(Th::qss(th.surface.sunken), Th::qss(th.divider.def), Th::qss(th.text.primary))
            .arg(th.fonts.md)
            .arg(Th::qss(th.surface.highlightStrong), Th::qss(th.surface.highlight))
    );

    // ── Section headings (bold, used across all pages) ─────────────────
    for (auto *w : _panel->findChildren<QLabel *>("sectionHeading")) {
        w->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                             .arg(th.fonts.base)
                             .arg(Th::qss(th.text.primary)));
    }

    // ── Appearance page ───────────────────────────────────────────────
    // The section containers are now titleless; just strip the default frame.
    for (const auto &boxName :
         {QString("sidebarBox"),
          QString("langBox"),
          QString("timeBox"),
          QString("themeBox"),
          QString("threadBox"),
          QString("fontBox")}) {
        if (auto *w = _panel->findChild<QGroupBox *>(boxName))
            w->setStyleSheet("QGroupBox { border: none; }");
    }
    if (auto *w = _panel->findChild<QLabel *>("langLabel")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    // _language self-themes (Dropdown).
    _langRestartNote->setStyleSheet(
        QString(
            "font-size: %1px; color: %2; background: %3;"
            "border: 1px solid %4; border-radius: 4px; padding: 6px 8px;"
        )
            .arg(th.fonts.caption)
            .arg(
                Th::qss(th.editBanner.text),
                Th::qss(th.editBanner.bg),
                Th::qss(th.editBanner.border)
            )
    );
    const QString radioQss = Th::radioQss(th.fonts.md);
    const QString checkQss = Th::checkBoxQss(th.fonts.md);
    const QString spinQss  = Th::spinBoxQss(th.fonts.md);
    _time12->setStyleSheet(radioQss);
    _time24->setStyleSheet(radioQss);
    _threadStandalone->setStyleSheet(radioQss);
    _threadInline->setStyleSheet(radioQss);
    _fontSmall->setStyleSheet(radioQss);
    _fontMedium->setStyleSheet(radioQss);
    _fontLarge->setStyleSheet(radioQss);
    if (auto *w = _panel->findChild<QLabel *>("daysPrefix")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    _relevantDays->setStyleSheet(spinQss);
    if (auto *w = _panel->findChild<QLabel *>("daysDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    // (Save button self-themes — StyledButton)

    // ── Notifications page ────────────────────────────────────────────
    _notifEnabled->setStyleSheet(checkQss);
    if (auto *w = _panel->findChild<QGroupBox *>("levelBox"))
        w->setStyleSheet("QGroupBox { border: none; }");
    _notifAll->setStyleSheet(radioQss);
    _notifMentions->setStyleSheet(radioQss);
    if (_modeSession)
        _modeSession->setStyleSheet(radioQss);
    if (_modeAppKeys)
        _modeAppKeys->setStyleSheet(radioQss);
    _notifHuddles->setStyleSheet(checkQss);
    _notifSound->setStyleSheet(checkQss);
    // (Save button self-themes — StyledButton)

    // ── AI assistance page ────────────────────────────────────────────
    for (auto *w : _panel->findChildren<QLabel *>("aiDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    for (auto *w : _panel->findChildren<QLabel *>("aiDefaultLabel")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    // _aiDefault self-themes (Dropdown).
    for (auto *w : _panel->findChildren<QGroupBox *>("aiBox"))
        w->setStyleSheet("QGroupBox { border: none; }");
    for (auto *w : _panel->findChildren<QLabel *>("aiStatus")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    // (AI connect/save/disconnect buttons, the key input and the key link all
    // self-theme — StyledButton / StyledLineEdit)
    if (_aiError) {
        _aiError->setStyleSheet(QString("font-size: %1px; color: %2;")
                                    .arg(th.fonts.caption)
                                    .arg(Th::qss(th.text.danger)));
    }

    // ── Storage page ──────────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("sizePrefixLabel")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.secondary))
        );
    }
    _cacheSize->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                                  .arg(th.fonts.md)
                                  .arg(Th::qss(th.text.primary)));
    if (auto *w = _panel->findChild<QLabel *>("cacheDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    if (auto *w = _panel->findChild<QLabel *>("capPrefix")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    _cacheCap->setStyleSheet(spinQss);
    if (auto *w = _panel->findChild<QLabel *>("capDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    // (Clear Cache button self-themes — StyledButton Danger)
    if (auto *w = _panel->findChild<QLabel *>("stateDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    // (Clear State button self-themes — StyledButton Danger)

    // ── System page ───────────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("verLabel")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.secondary))
        );
    }
    if (auto *w = _panel->findChild<QGroupBox *>("updBox"))
        w->setStyleSheet("QGroupBox { border: none; }");
    // (Check-for-updates button self-themes — StyledButton Ghost)
    _updateStatus->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.caption).arg(Th::qss(th.text.secondary))
    );
    _lastChecked->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.sm).arg(Th::qss(th.text.tertiary))
    );
    if (auto *w = _panel->findChild<QGroupBox *>("memBox"))
        w->setStyleSheet("QGroupBox { border: none; }");
    _ramLabel->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );

    // ── About page ────────────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("aboutLicense")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.secondary))
        );
    }
    if (auto *w = _panel->findChild<QLabel *>("aboutContact")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    if (auto *w = _panel->findChild<QLabel *>("aboutBugDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
}

void SettingsDialog::loadNotifications() {
    QSettings s("msga", "msga");
    _notifEnabled->setChecked(s.value("notifications/enabled", true).toBool());
    _notifHuddles->setChecked(s.value("notifications/huddles", true).toBool());
    _notifSound->setChecked(s.value("notifications/sound", true).toBool());
    const int level = s.value("notifications/level", 0).toInt();
    (level == 0 ? _notifAll : _notifMentions)->setChecked(true);

    // (Re)populate the sound chooser — bundled sounds first, then the
    // OS-enumerated system sounds. Enumeration is per-open so a freshly added
    // system sound shows up.
    _notifSoundChoice->clear();
    auto &player = Sound::Player::instance();
    for (const auto &e : player.bundledSounds())
        _notifSoundChoice->addItem(e.label, e.id);
    const auto sys = player.systemSounds();
    if (!sys.empty()) {
        _notifSoundChoice->addSeparator();
        for (const auto &e : sys)
            _notifSoundChoice->addItem(e.label, e.id);
    }
    const QString soundId = s.value("notifications/soundId", Sound::Player::defaultId()).toString();
    const int     idx     = _notifSoundChoice->findData(soundId);
    _notifSoundChoice->setCurrentIndex(idx >= 0 ? idx : 0);

    // Sync enabled state of child controls
    const bool on      = _notifEnabled->isChecked();
    const bool soundOn = on && _notifSound->isChecked();
    _notifAll->parentWidget()->setEnabled(on);
    _notifHuddles->setEnabled(on);
    _notifSound->setEnabled(on);
    _notifSoundRow->setEnabled(soundOn);
}

void SettingsDialog::saveNotifications() {
    QSettings s("msga", "msga");
    s.setValue("notifications/enabled", _notifEnabled->isChecked());
    s.setValue("notifications/huddles", _notifHuddles->isChecked());
    s.setValue("notifications/sound", _notifSound->isChecked());
    s.setValue("notifications/level", _notifAll->isChecked() ? 0 : 1);
    if (_notifSoundChoice->currentIndex() >= 0)
        s.setValue("notifications/soundId", _notifSoundChoice->currentData());
    emit notificationsChanged();
}

void SettingsDialog::loadAppearance() {
    const int days = QSettings("msga", "msga").value("appearance/relevantDays", 14).toInt();
    _relevantDays->setValue(std::max(1, days));

    const int idx = _language->findData(TimeFmt::language());
    _language->setCurrentIndex(idx >= 0 ? idx : 0);
    _langRestartNote->setVisible(_language->currentData().toString() != _startupLanguage);
    (TimeFmt::use24h() ? _time24 : _time12)->setChecked(true);

    const bool inlineThreads =
        QSettings("msga", "msga").value("appearance/threadsInline", false).toBool();
    (inlineThreads ? _threadInline : _threadStandalone)->setChecked(true);

    const QString fontId = ThemeManager::instance().fontSizeId();
    (fontId == QLatin1String("small")   ? _fontSmall
     : fontId == QLatin1String("large") ? _fontLarge
                                        : _fontMedium)
        ->setChecked(true);

    for (auto *card : _themeCards)
        card->setChecked(card->themeId() == ThemeManager::instance().themeId());
}

void SettingsDialog::saveAppearance() {
    const int days = _relevantDays->value();
    QSettings("msga", "msga").setValue("appearance/relevantDays", days);

    TimeFmt::setLanguage(_language->currentData().toString());
    TimeFmt::setUse24h(_time24->isChecked());

    const bool inlineThreads = _threadInline->isChecked();
    QSettings("msga", "msga").setValue("appearance/threadsInline", inlineThreads);

    // Applies + persists + re-emits themeChanged (a no-op when unchanged).
    ThemeManager::instance().setFontSizeId(
        _fontSmall->isChecked()   ? QStringLiteral("small")
        : _fontLarge->isChecked() ? QStringLiteral("large")
                                  : QStringLiteral("medium")
    );

    emit appearanceChanged(days);
    emit timeFormatChanged();
    emit threadDisplayChanged(inlineThreads);
}

static QString formatBytes(qint64 bytes) {
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QString("%1 KB").arg(bytes / 1024);
    return QString("%1 MB").arg(bytes / (1024 * 1024));
}

static qint64 dirSizeBytes(const QString &path) {
    qint64       total = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

void SettingsDialog::refreshCacheSize() {
    const QString cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache";
    _cacheSize->setText(formatBytes(dirSizeBytes(cacheDir)));
}

void SettingsDialog::clearCache() {
    if (auto *btn = _panel->findChild<QPushButton *>("clearCacheBtn"))
        btn->setEnabled(false);
    const QString cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache";
    QDir(cacheDir).removeRecursively();
    refreshCacheSize();
}

void SettingsDialog::clearState() {
    if (auto *btn = _panel->findChild<QPushButton *>("clearStateBtn"))
        btn->setEnabled(false);
    QSettings("msga", "msga").remove("conv/visitedAt");
    emit stateCleared();
}

void SettingsDialog::loadAppCredentials() {
    if (!_credClientId)
        return;
    const slack::PersonalAppCredentials c = slack::personalAppCredentials();
    _credClientId->setText(c.clientId);
    _credClientSecret->setText(c.clientSecret);
    _credXapp->setText(c.xapp);
    _credStatus->clear();
}

void SettingsDialog::saveAppCredentials() {
    if (!_credClientId)
        return;
    const slack::PersonalAppCredentials next{
        _credClientId->text().trimmed(),
        _credClientSecret->text().trimmed(),
        _credXapp->text().trimmed(),
    };
    // No-op saves shouldn't kill the session — only restart when something changed.
    const slack::PersonalAppCredentials cur = slack::personalAppCredentials();
    if (next.clientId == cur.clientId && next.clientSecret == cur.clientSecret &&
        next.xapp == cur.xapp) {
        _credStatus->setText(tr("No changes to save."));
        return;
    }
    slack::setPersonalAppCredentials(next);
    _credStatus->setText(tr("Saved. Restarting…"));
    emit restartRequested();
}

void SettingsDialog::updateSlackModeUi() {
    if (!_modeSession)
        return;
    const bool session = _modeSession->isChecked();
    _sessionBox->setVisible(session);
    _appKeysBox->setVisible(!session);
    // Only a change from the mode the app launched with needs a restart to apply.
    _modeRestartNote->setVisible(session != _startupSessionMode);
}

void SettingsDialog::openSessionImport() {
    // Parent to the top-level window so the overlay covers the whole window
    // (over this settings overlay), consistent with other AppDialogs.
    auto *dlg = new SessionImportDialog(this);
    connect(
        dlg,
        &SessionImportDialog::imported,
        this,
        [this](const QList<TokenStore::WorkspaceRecord> &records) {
            emit slackWorkspacesImported(records);
        }
    );
    connect(dlg, &AppDialog::finished, dlg, [dlg](int) { dlg->deleteLater(); });
    dlg->open();
}

static QString timeAgo(qint64 epochSecs) {
    auto t = [](const char *s) { return QCoreApplication::translate("SettingsDialog", s); };
    if (epochSecs <= 0)
        return t("Never checked");
    const qint64 ago = QDateTime::currentSecsSinceEpoch() - epochSecs;
    if (ago < 60)
        return t("Just now");
    if (ago < 3600)
        return t("%1 min ago").arg(ago / 60);
    if (ago < 86400)
        return t("%1 h ago").arg(ago / 3600);
    return t("%1 days ago").arg(ago / 86400);
}

void SettingsDialog::refreshLastChecked() {
    if (!_lastChecked)
        return;
    const qint64 ts = QSettings("msga", "msga").value("updates/lastChecked", 0).toLongLong();
    _lastChecked->setText(tr("Last checked: %1").arg(timeAgo(ts)));
}

void SettingsDialog::refreshUpdateStatus() {
    if (!_updateStatus || !_checkBtn)
        return;
    if (!_updateChecker) {
        _checkBtn->setEnabled(false);
        _updateStatus->setText(tr("Update checks not available."));
        return;
    }
    if (_updateChecker->isChecking()) {
        _checkBtn->setEnabled(false);
        _updateStatus->setText(tr("Checking for updates…"));
    } else if (_updateChecker->isReady()) {
        _checkBtn->setEnabled(true);
        _updateStatus->setText(tr("Update downloaded — restart the app to apply."));
    } else {
        _checkBtn->setEnabled(true);
        _updateStatus->clear();
    }
}

void SettingsDialog::setUpdateChecker(UpdateChecker *checker) {
    _updateChecker = checker;

    connect(_checkBtn, &QPushButton::clicked, checker, &UpdateChecker::checkNow);

    connect(checker, &UpdateChecker::checkStarted, this, [this] {
        _checkBtn->setEnabled(false);
        _updateStatus->setText(tr("Checking for updates…"));
    });
    connect(checker, &UpdateChecker::upToDate, this, [this] {
        _checkBtn->setEnabled(true);
        _updateStatus->setText(tr("msga is up to date."));
        refreshLastChecked();
    });
    connect(checker, &UpdateChecker::updateAvailable, this, [this](int v) {
        _updateStatus->setText(tr("Version %1 available — downloading…").arg(v));
    });
    connect(checker, &UpdateChecker::downloadProgress, this, [this](int pct) {
        _updateStatus->setText(tr("Downloading update… %1%").arg(pct));
    });
    connect(checker, &UpdateChecker::downloadReady, this, [this]() {
        _checkBtn->setEnabled(true);
        _updateStatus->setText(tr("Update downloaded — restart the app to apply."));
        refreshLastChecked();
    });
    connect(checker, &UpdateChecker::checkFailed, this, [this](const QString &msg) {
        _checkBtn->setEnabled(true);
        _updateStatus->setText(tr("Check failed: %1").arg(msg));
        refreshLastChecked();
    });
}

void SettingsDialog::updatePanelGeometry() {
    if (!_panel)
        return;
    const QSize ps = _panel->size();
    _panel->move((width() - ps.width()) / 2, (height() - ps.height()) / 2);
}

// ── Edge detection & resize helpers ──────────────────────────────────────────

SettingsDialog::Dir SettingsDialog::edgeAt(const QPoint &p) const {
    if (!_panel)
        return Dir::None;
    const QRect r = _panel->geometry();
    if (!r.adjusted(-kEdge, -kEdge, kEdge, kEdge).contains(p) || r.contains(p))
        return Dir::None;

    const bool n = p.y() < r.top() + kEdge;
    const bool s = p.y() > r.bottom() - kEdge;
    const bool w = p.x() < r.left() + kEdge;
    const bool e = p.x() > r.right() - kEdge;

    if (n && w)
        return Dir::NW;
    if (n && e)
        return Dir::NE;
    if (s && w)
        return Dir::SW;
    if (s && e)
        return Dir::SE;
    if (n)
        return Dir::N;
    if (s)
        return Dir::S;
    if (w)
        return Dir::W;
    if (e)
        return Dir::E;
    return Dir::None;
}

Qt::CursorShape SettingsDialog::cursorFor(Dir d) {
    switch (d) {
    case Dir::N:
    case Dir::S:
        return Qt::SizeVerCursor;
    case Dir::E:
    case Dir::W:
        return Qt::SizeHorCursor;
    case Dir::NE:
    case Dir::SW:
        return Qt::SizeBDiagCursor;
    case Dir::NW:
    case Dir::SE:
        return Qt::SizeFDiagCursor;
    default:
        return Qt::ArrowCursor;
    }
}

// ── Painting & events ─────────────────────────────────────────────────────────

void SettingsDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 150));
}

void SettingsDialog::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const Dir dir = edgeAt(e->pos());
    if (dir == Dir::None)
        return;
    _resizeDir   = dir;
    _dragStart   = e->pos();
    _panelAtDrag = _panel->geometry();
    grabMouse(cursorFor(dir));
}

void SettingsDialog::mouseMoveEvent(QMouseEvent *e) {
    if (_resizeDir != Dir::None) {
        const QPoint delta = e->pos() - _dragStart;
        QRect        r     = _panelAtDrag;

        switch (_resizeDir) {
        case Dir::E:
            r.setRight(r.right() + delta.x());
            break;
        case Dir::W:
            r.setLeft(r.left() + delta.x());
            break;
        case Dir::S:
            r.setBottom(r.bottom() + delta.y());
            break;
        case Dir::N:
            r.setTop(r.top() + delta.y());
            break;
        case Dir::SE:
            r.setRight(r.right() + delta.x());
            r.setBottom(r.bottom() + delta.y());
            break;
        case Dir::SW:
            r.setLeft(r.left() + delta.x());
            r.setBottom(r.bottom() + delta.y());
            break;
        case Dir::NE:
            r.setRight(r.right() + delta.x());
            r.setTop(r.top() + delta.y());
            break;
        case Dir::NW:
            r.setLeft(r.left() + delta.x());
            r.setTop(r.top() + delta.y());
            break;
        default:
            break;
        }

        const QSize minS = _panel->minimumSize();
        if (r.width() < minS.width()) {
            const bool movingLeft =
                (_resizeDir == Dir::W || _resizeDir == Dir::NW || _resizeDir == Dir::SW);
            if (movingLeft)
                r.setLeft(r.right() - minS.width());
            else
                r.setRight(r.left() + minS.width());
        }
        if (r.height() < minS.height()) {
            const bool movingTop =
                (_resizeDir == Dir::N || _resizeDir == Dir::NW || _resizeDir == Dir::NE);
            if (movingTop)
                r.setTop(r.bottom() - minS.height());
            else
                r.setBottom(r.top() + minS.height());
        }

        _panel->setGeometry(r);
        return;
    }

    setCursor(cursorFor(edgeAt(e->pos())));
}

void SettingsDialog::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _resizeDir != Dir::None) {
        _resizeDir = Dir::None;
        releaseMouse();
        unsetCursor();
    }
}

void SettingsDialog::leaveEvent(QEvent *) {
    if (_resizeDir == Dir::None)
        unsetCursor();
}

bool SettingsDialog::eventFilter(QObject *obj, QEvent *e) {
    if (obj == parent() && e->type() == QEvent::Resize) {
        auto *re = static_cast<QResizeEvent *>(e);
        setGeometry(QRect({}, re->size()));
        updatePanelGeometry();
    }
    return QWidget::eventFilter(obj, e);
}
