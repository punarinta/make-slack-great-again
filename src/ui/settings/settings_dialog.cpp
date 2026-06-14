// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "settings_dialog.h"
#include "theme_preview_card.h"
#include "ui/update_checker/update_checker.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "app_credentials.h"
#include "llm/llm_service.h"
#include "cache/cache_evictor.h"
#include "util/time_format.h"
#include "util/process_stats.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QShortcut>
#include <QResizeEvent>
#include <QFrame>
#include <QListWidget>
#include <QStackedWidget>
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
#include <QComboBox>
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

    // ── Header ────────────────────────────────────────────────────────
    auto *header = new QWidget(_panel);
    header->setObjectName("settingsHeader");
    header->setFixedHeight(48);
    auto *hlay = new QHBoxLayout(header);
    hlay->setContentsMargins(20, 0, 12, 0);

    auto *titleLabel = new QLabel(tr("Settings"), header);
    titleLabel->setObjectName("settingsTitleLabel");
    hlay->addWidget(titleLabel);
    hlay->addStretch();

    auto *closeBtn = new QPushButton("\xC3\x97", header);
    closeBtn->setObjectName("settingsCloseBtn");
    closeBtn->setFixedSize(28, 28);
    closeBtn->setCursor(Qt::PointingHandCursor);
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
    _tabs->setCurrentRow(0);
    blay->addWidget(_tabs);

    _stack = new QStackedWidget(body);
    blay->addWidget(_stack, 1);

    connect(_tabs, &QListWidget::currentRowChanged, _stack, &QStackedWidget::setCurrentIndex);

    // ── Appearance page ───────────────────────────────────────────────
    auto *appearPage = new QWidget;
    auto *alay       = new QVBoxLayout(appearPage);
    alay->setContentsMargins(24, 20, 24, 20);
    alay->setSpacing(16);

    auto *appearHeading = new QLabel(tr("Appearance"), appearPage);
    appearHeading->setObjectName("appearHeading");
    alay->addWidget(appearHeading);

    // ── Color theme ───────────────────────────────────────────────────
    auto *themeBox = new QGroupBox(tr("Color theme"), appearPage);
    themeBox->setObjectName("themeBox");
    auto *themeLayout = new QHBoxLayout(themeBox);
    themeLayout->setSpacing(12);
    themeLayout->setContentsMargins(0, 12, 0, 0);

    auto *themeGroup = new QButtonGroup(themeBox);
    themeGroup->setExclusive(true);
    for (const auto &info : Th::availableThemes()) {
        const QString name = info.id == QLatin1String("purple")  ? tr("Purple")
                             : info.id == QLatin1String("blue")  ? tr("Blue")
                             : info.id == QLatin1String("green") ? tr("Green")
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
    alay->addWidget(themeBox);

    // ── Language ──────────────────────────────────────────────────────
    _startupLanguage = TimeFmt::language();

    auto *langBox = new QGroupBox(tr("Language"), appearPage);
    langBox->setObjectName("langBox");
    auto *langLayout = new QVBoxLayout(langBox);
    langLayout->setSpacing(8);
    langLayout->setContentsMargins(0, 12, 0, 0);

    auto *langRow   = new QHBoxLayout;
    auto *langLabel = new QLabel(tr("App language"), langBox);
    langLabel->setObjectName("langLabel");
    langRow->addWidget(langLabel);

    _language = new QComboBox(langBox);
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

    connect(_language, &QComboBox::currentIndexChanged, this, [this] {
        _langRestartNote->setVisible(_language->currentData().toString() != _startupLanguage);
    });

    alay->addWidget(langBox);

    // ── Time format ───────────────────────────────────────────────────
    auto *timeBox = new QGroupBox(tr("Time format"), appearPage);
    timeBox->setObjectName("timeBox");
    auto *timeLayout = new QVBoxLayout(timeBox);
    timeLayout->setSpacing(6);
    timeLayout->setContentsMargins(0, 12, 0, 0);

    _time12 = new QRadioButton(tr("12-hour clock (2:34 PM)"), timeBox);
    _time24 = new QRadioButton(tr("24-hour clock (14:34)"), timeBox);

    auto *timeGroup = new QButtonGroup(timeBox);
    timeGroup->addButton(_time12, 0);
    timeGroup->addButton(_time24, 1);

    timeLayout->addWidget(_time12);
    timeLayout->addWidget(_time24);
    alay->addWidget(timeBox);

    auto *sidebarBox = new QGroupBox(tr("Conversations sidebar"), appearPage);
    sidebarBox->setObjectName("sidebarBox");
    auto *sidebarLayout = new QVBoxLayout(sidebarBox);
    sidebarLayout->setSpacing(8);
    sidebarLayout->setContentsMargins(0, 12, 0, 0);

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
    auto *aSaveBtn = new QPushButton(tr("Save"), appearPage);
    aSaveBtn->setObjectName("appearSaveBtn");
    aSaveBtn->setFixedHeight(34);
    aSaveBtn->setMinimumWidth(80);
    aSaveBtn->setCursor(Qt::PointingHandCursor);
    connect(aSaveBtn, &QPushButton::clicked, this, [this] {
        saveAppearance();
        hide();
    });
    aBtnRow->addWidget(aSaveBtn);
    alay->addLayout(aBtnRow);

    _stack->addWidget(appearPage);

    // ── Notifications page ────────────────────────────────────────────
    auto *notifPage = new QWidget;
    auto *nlay      = new QVBoxLayout(notifPage);
    nlay->setContentsMargins(24, 20, 24, 20);
    nlay->setSpacing(16);

    auto *notifHeading = new QLabel(tr("Notifications"), notifPage);
    notifHeading->setObjectName("notifHeading");
    nlay->addWidget(notifHeading);

    _notifEnabled = new QCheckBox(tr("Enable desktop notifications"), notifPage);
    nlay->addWidget(_notifEnabled);

    auto *levelBox = new QGroupBox(tr("Notify me about"), notifPage);
    levelBox->setObjectName("levelBox");
    auto *levelLayout = new QVBoxLayout(levelBox);
    levelLayout->setSpacing(6);
    levelLayout->setContentsMargins(0, 12, 0, 0);

    _notifAll      = new QRadioButton(tr("All new messages"), levelBox);
    _notifMentions = new QRadioButton(tr("Direct messages and mentions only"), levelBox);

    auto *group = new QButtonGroup(levelBox);
    group->addButton(_notifAll, 0);
    group->addButton(_notifMentions, 1);

    levelLayout->addWidget(_notifAll);
    levelLayout->addWidget(_notifMentions);
    nlay->addWidget(levelBox);

    _notifSound = new QCheckBox(tr("Play a sound for notifications"), notifPage);
    nlay->addWidget(_notifSound);

    // Disable level/sound when master toggle is off
    auto updateEnabled = [this, levelBox]() {
        const bool on = _notifEnabled->isChecked();
        levelBox->setEnabled(on);
        _notifSound->setEnabled(on);
    };
    connect(_notifEnabled, &QCheckBox::toggled, this, updateEnabled);

    nlay->addStretch();

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *saveBtn = new QPushButton(tr("Save"), notifPage);
    saveBtn->setObjectName("notifSaveBtn");
    saveBtn->setFixedHeight(34);
    saveBtn->setMinimumWidth(80);
    saveBtn->setCursor(Qt::PointingHandCursor);
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        saveNotifications();
        hide();
    });
    btnRow->addWidget(saveBtn);
    nlay->addLayout(btnRow);

    _stack->addWidget(notifPage);

    // ── AI assistance page ────────────────────────────────────────────
    _stack->addWidget(buildAiPage());

    // ── Storage page ──────────────────────────────────────────────────
    auto *storagePage = new QWidget;
    auto *slay        = new QVBoxLayout(storagePage);
    slay->setContentsMargins(24, 20, 24, 20);
    slay->setSpacing(16);

    auto *storageHeading = new QLabel(tr("Storage"), storagePage);
    storageHeading->setObjectName("storageHeading");
    slay->addWidget(storageHeading);

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
    clearCacheRow->addStretch();
    auto *clearCacheBtn = new QPushButton(tr("Clear Cache"), storagePage);
    clearCacheBtn->setObjectName("clearCacheBtn");
    clearCacheBtn->setFixedHeight(34);
    clearCacheBtn->setMinimumWidth(110);
    clearCacheBtn->setCursor(Qt::PointingHandCursor);
    connect(clearCacheBtn, &QPushButton::clicked, this, &SettingsDialog::clearCache);
    clearCacheRow->addWidget(clearCacheBtn);
    slay->addLayout(clearCacheRow);

    // Separator
    auto *sep = new QFrame(storagePage);
    sep->setObjectName("storageSep");
    sep->setFrameShape(QFrame::HLine);
    slay->addWidget(sep);

    // ── App state section ─────────────────────────────────────────────
    auto *stateHeading = new QLabel(tr("App state"), storagePage);
    stateHeading->setObjectName("stateHeading");
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
    clearStateRow->addStretch();
    auto *clearStateBtn = new QPushButton(tr("Clear State"), storagePage);
    clearStateBtn->setObjectName("clearStateBtn");
    clearStateBtn->setFixedHeight(34);
    clearStateBtn->setMinimumWidth(110);
    clearStateBtn->setCursor(Qt::PointingHandCursor);
    connect(clearStateBtn, &QPushButton::clicked, this, &SettingsDialog::clearState);
    clearStateRow->addWidget(clearStateBtn);
    slay->addLayout(clearStateRow);

    slay->addStretch();

    _stack->addWidget(storagePage);

    // ── System page ───────────────────────────────────────────────────
    auto *sysPage = new QWidget;
    auto *sylay   = new QVBoxLayout(sysPage);
    sylay->setContentsMargins(24, 20, 24, 20);
    sylay->setSpacing(16);

    auto *sysHeading = new QLabel(tr("System"), sysPage);
    sysHeading->setObjectName("sysHeading");
    sylay->addWidget(sysHeading);

    // Version info
    const QString buildTs =
        QString(AppCredentials::buildTimestamp).replace('T', ' ').chopped(1); // drop trailing Z
    auto *verLabel =
        new QLabel(tr("Version %1, built %2").arg(AppCredentials::version).arg(buildTs), sysPage);
    verLabel->setObjectName("verLabel");
    sylay->addWidget(verLabel);

    // Update section
    auto *updBox = new QGroupBox(tr("Updates"), sysPage);
    updBox->setObjectName("updBox");
    auto *updLayout = new QVBoxLayout(updBox);
    updLayout->setSpacing(8);
    updLayout->setContentsMargins(0, 12, 0, 0);

    auto *checkRow = new QHBoxLayout;
    _checkBtn      = new QPushButton(tr("Check for updates"), updBox);
    _checkBtn->setFixedHeight(30);
    _checkBtn->setCursor(Qt::PointingHandCursor);
    checkRow->addWidget(_checkBtn);
    checkRow->addStretch();
    updLayout->addLayout(checkRow);

    _updateStatus = new QLabel("", updBox);
    _updateStatus->setWordWrap(true);
    updLayout->addWidget(_updateStatus);

    _lastChecked = new QLabel("", updBox);
    updLayout->addWidget(_lastChecked);

    sylay->addWidget(updBox);

    // Memory section
    auto *memBox = new QGroupBox(tr("Memory"), sysPage);
    memBox->setObjectName("memBox");
    auto *memLayout = new QVBoxLayout(memBox);
    memLayout->setSpacing(4);
    memLayout->setContentsMargins(0, 12, 0, 0);

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

    _stack->addWidget(sysPage);
    root->addWidget(body, 1);

    auto *esc = new QShortcut(Qt::Key_Escape, _panel);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, &SettingsDialog::hide);

    updatePanelGeometry();
}

QWidget *SettingsDialog::buildAiPage() {
    auto &svc = LlmService::instance();

    auto *page = new QWidget;
    auto *lay  = new QVBoxLayout(page);
    lay->setContentsMargins(24, 20, 24, 20);
    lay->setSpacing(16);

    auto *heading = new QLabel(tr("AI assistance"), page);
    heading->setObjectName("aiHeading");
    lay->addWidget(heading);

    auto *desc = new QLabel(
        tr("Connect an AI provider to enable assistant features.\n"
           "Create an API key in your own provider account and paste it below —\n"
           "it is stored on this computer and sent only to that provider."),
        page
    );
    desc->setObjectName("aiDesc");
    desc->setWordWrap(true);
    lay->addWidget(desc);

    // Default provider selector
    auto *defRow   = new QHBoxLayout;
    auto *defLabel = new QLabel(tr("Default provider:"), page);
    defLabel->setObjectName("aiDefaultLabel");
    defRow->addWidget(defLabel);

    _aiDefault = new QComboBox(page);
    for (auto *p : svc.providers())
        _aiDefault->addItem(p->displayName(), p->id());
    defRow->addWidget(_aiDefault);
    defRow->addStretch();
    lay->addLayout(defRow);

    connect(_aiDefault, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx >= 0)
            LlmService::instance().setDefaultProviderId(_aiDefault->itemData(idx).toString());
    });

    // One card per provider
    for (auto *p : svc.providers()) {
        auto *box = new QGroupBox(p->displayName(), page);
        box->setObjectName("aiBox");
        auto *bl = new QVBoxLayout(box);
        bl->setSpacing(8);
        bl->setContentsMargins(0, 12, 0, 0);

        AiProviderRow row;
        row.provider = p;

        row.status = new QLabel(box);
        row.status->setObjectName("aiStatus");
        bl->addWidget(row.status);

        auto *btnRow = new QHBoxLayout;
        row.oauthBtn = new QPushButton(tr("Connect (OAuth)"), box);
        row.oauthBtn->setObjectName("aiConnectBtn");
        row.oauthBtn->setFixedHeight(30);
        row.oauthBtn->setCursor(Qt::PointingHandCursor);
        btnRow->addWidget(row.oauthBtn);

        row.disconnectBtn = new QPushButton(tr("Disconnect"), box);
        row.disconnectBtn->setObjectName("aiDisconnectBtn");
        row.disconnectBtn->setFixedHeight(30);
        row.disconnectBtn->setCursor(Qt::PointingHandCursor);
        btnRow->addWidget(row.disconnectBtn);
        btnRow->addStretch();
        bl->addLayout(btnRow);

        auto *keyRow = new QHBoxLayout;
        row.keyEdit  = new QLineEdit(box);
        row.keyEdit->setObjectName("aiKeyEdit");
        row.keyEdit->setPlaceholderText(tr("Paste your API key"));
        row.keyEdit->setEchoMode(QLineEdit::Password);
        keyRow->addWidget(row.keyEdit, 1);

        row.saveKeyBtn = new QPushButton(tr("Save key"), box);
        row.saveKeyBtn->setObjectName("aiSaveKeyBtn");
        row.saveKeyBtn->setFixedHeight(30);
        row.saveKeyBtn->setCursor(Qt::PointingHandCursor);
        keyRow->addWidget(row.saveKeyBtn);
        bl->addLayout(keyRow);

        auto *keyLink = new QPushButton(tr("Get an API key from %1…").arg(p->displayName()), box);
        keyLink->setObjectName("aiKeyLink");
        keyLink->setCursor(Qt::PointingHandCursor);
        keyLink->setFlat(true);
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
        connect(row.keyEdit, &QLineEdit::returnPressed, this, saveKey);

        connect(p, &LlmProvider::authStateChanged, this, &SettingsDialog::refreshAiProviders);
        connect(p, &LlmProvider::authFailed, this, [this, p](const QString &reason) {
            _aiError->setText(tr("%1: %2").arg(p->displayName(), reason));
        });

        _aiRows.append(row);
        lay->addWidget(box);
    }

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
}

void SettingsDialog::applyTheme() {
    const auto &th = Th::c();

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
    if (auto *w = _panel->findChild<QPushButton *>("settingsCloseBtn")) {
        w->setStyleSheet(QString(
                             "QPushButton {"
                             "  background: transparent; color: %1; border: none;"
                             "  border-radius: 4px; font-size: %2px;"
                             "}"
                             "QPushButton:hover   { background: %3; color: %4; }"
                             "QPushButton:pressed { background: %5; }"
        )
                             .arg(Th::qss(th.text.secondary))
                             .arg(th.fonts.xl)
                             .arg(
                                 Th::qss(th.surface.highlight),
                                 Th::qss(th.text.primary),
                                 Th::qss(th.surface.highlightStrong)
                             ));
    }

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

    // ── Appearance page ───────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("appearHeading")) {
        w->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                             .arg(th.fonts.base)
                             .arg(Th::qss(th.text.primary)));
    }
    for (const auto &boxName :
         {QString("sidebarBox"), QString("langBox"), QString("timeBox"), QString("themeBox")}) {
        if (auto *w = _panel->findChild<QGroupBox *>(boxName)) {
            w->setStyleSheet(
                QString(
                    "QGroupBox { font-size: %1px; color: %2; border: none; margin-top: 4px; }"
                    "QGroupBox::title { subcontrol-origin: margin; left: 0; }"
                )
                    .arg(th.fonts.caption)
                    .arg(Th::qss(th.text.secondary))
            );
        }
    }
    if (auto *w = _panel->findChild<QLabel *>("langLabel")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    _language->setStyleSheet(
        QString(
            "QComboBox {"
            "  font-size: %1px; color: %2;"
            "  border: 1px solid %3; border-radius: 4px; padding: 3px 6px;"
            "}"
            "QComboBox:focus { border-color: %4; }"
        )
            .arg(th.fonts.md)
            .arg(Th::qss(th.text.primary), Th::qss(th.divider.strong), Th::qss(th.text.link))
    );
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
    _time12->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );
    _time24->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );
    if (auto *w = _panel->findChild<QLabel *>("daysPrefix")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    _relevantDays->setStyleSheet(
        QString(
            "QSpinBox {"
            "  font-size: %1px; color: %2;"
            "  border: 1px solid %3; border-radius: 4px; padding: 3px 6px;"
            "}"
            "QSpinBox:focus { border-color: %4; }"
        )
            .arg(th.fonts.md)
            .arg(Th::qss(th.text.primary), Th::qss(th.divider.strong), Th::qss(th.text.link))
    );
    if (auto *w = _panel->findChild<QLabel *>("daysDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    if (auto *w = _panel->findChild<QPushButton *>("appearSaveBtn")) {
        w->setStyleSheet(
            QString(
                "QPushButton {"
                "  background: %1; color: white; border: none;"
                "  border-radius: 4px; font-size: %2px; font-weight: 600; padding: 0 16px;"
                "}"
                "QPushButton:hover   { background: %3; }"
                "QPushButton:pressed { background: %4; }"
            )
                .arg(Th::qss(th.accent.def))
                .arg(th.fonts.md)
                .arg(Th::qss(th.accent.hover), Th::qss(th.accent.pressed))
        );
    }

    // ── Notifications page ────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("notifHeading")) {
        w->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                             .arg(th.fonts.base)
                             .arg(Th::qss(th.text.primary)));
    }
    _notifEnabled->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );
    if (auto *w = _panel->findChild<QGroupBox *>("levelBox")) {
        w->setStyleSheet(
            QString(
                "QGroupBox { font-size: %1px; color: %2; border: none; margin-top: 4px; }"
                "QGroupBox::title { subcontrol-origin: margin; left: 0; }"
            )
                .arg(th.fonts.caption)
                .arg(Th::qss(th.text.secondary))
        );
    }
    _notifAll->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );
    _notifMentions->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );
    _notifSound->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );
    if (auto *w = _panel->findChild<QPushButton *>("notifSaveBtn")) {
        w->setStyleSheet(
            QString(
                "QPushButton {"
                "  background: %1; color: white; border: none;"
                "  border-radius: 4px; font-size: %2px; font-weight: 600; padding: 0 16px;"
                "}"
                "QPushButton:hover   { background: %3; }"
                "QPushButton:pressed { background: %4; }"
            )
                .arg(Th::qss(th.accent.def))
                .arg(th.fonts.md)
                .arg(Th::qss(th.accent.hover), Th::qss(th.accent.pressed))
        );
    }

    // ── AI assistance page ────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("aiHeading")) {
        w->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                             .arg(th.fonts.base)
                             .arg(Th::qss(th.text.primary)));
    }
    if (auto *w = _panel->findChild<QLabel *>("aiDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    if (auto *w = _panel->findChild<QLabel *>("aiDefaultLabel")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    if (_aiDefault) {
        _aiDefault->setStyleSheet(
            QString(
                "QComboBox {"
                "  font-size: %1px; color: %2;"
                "  border: 1px solid %3; border-radius: 4px; padding: 3px 6px;"
                "}"
                "QComboBox:focus { border-color: %4; }"
            )
                .arg(th.fonts.md)
                .arg(Th::qss(th.text.primary), Th::qss(th.divider.strong), Th::qss(th.text.link))
        );
    }
    for (auto *w : _panel->findChildren<QGroupBox *>("aiBox")) {
        w->setStyleSheet(
            QString(
                "QGroupBox { font-size: %1px; color: %2; border: none; margin-top: 4px; }"
                "QGroupBox::title { subcontrol-origin: margin; left: 0; }"
            )
                .arg(th.fonts.caption)
                .arg(Th::qss(th.text.secondary))
        );
    }
    for (auto *w : _panel->findChildren<QLabel *>("aiStatus")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
        );
    }
    const QString aiAccentBtn = QString(
                                    "QPushButton {"
                                    "  background: %1; color: white; border: none;"
                                    "  border-radius: 4px; font-size: %2px; font-weight: 600;"
                                    "  padding: 0 14px;"
                                    "}"
                                    "QPushButton:hover    { background: %3; }"
                                    "QPushButton:pressed  { background: %4; }"
                                    "QPushButton:disabled { background: %5; color: %6; }"
    )
                                    .arg(Th::qss(th.accent.def))
                                    .arg(th.fonts.md)
                                    .arg(
                                        Th::qss(th.accent.hover),
                                        Th::qss(th.accent.pressed),
                                        Th::qss(th.surface.highlight),
                                        Th::qss(th.text.tertiary)
                                    );
    const QString aiNeutralBtn = QString(
                                     "QPushButton {"
                                     "  background: %1; color: %2; border: none;"
                                     "  border-radius: 4px; font-size: %3px; padding: 0 14px;"
                                     "}"
                                     "QPushButton:hover   { background: %4; }"
                                     "QPushButton:pressed { background: %4; }"
    )
                                     .arg(Th::qss(th.surface.highlight), Th::qss(th.text.primary))
                                     .arg(th.fonts.md)
                                     .arg(Th::qss(th.surface.highlightStrong));
    for (auto *w : _panel->findChildren<QPushButton *>("aiConnectBtn"))
        w->setStyleSheet(aiAccentBtn);
    for (auto *w : _panel->findChildren<QPushButton *>("aiSaveKeyBtn"))
        w->setStyleSheet(aiAccentBtn);
    for (auto *w : _panel->findChildren<QPushButton *>("aiDisconnectBtn"))
        w->setStyleSheet(aiNeutralBtn);
    for (auto *w : _panel->findChildren<QLineEdit *>("aiKeyEdit")) {
        w->setStyleSheet(
            QString(
                "QLineEdit {"
                "  font-size: %1px; color: %2;"
                "  border: 1px solid %3; border-radius: 4px; padding: 4px 6px;"
                "}"
                "QLineEdit:focus { border-color: %4; }"
            )
                .arg(th.fonts.md)
                .arg(Th::qss(th.text.primary), Th::qss(th.divider.strong), Th::qss(th.text.link))
        );
    }
    for (auto *w : _panel->findChildren<QPushButton *>("aiKeyLink")) {
        w->setStyleSheet(QString(
                             "QPushButton {"
                             "  background: transparent; border: none; padding: 0;"
                             "  color: %1; font-size: %2px; text-decoration: underline;"
                             "  text-align: left;"
                             "}"
                             "QPushButton:hover { color: %3; }"
        )
                             .arg(Th::qss(th.text.link))
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.accent.hover)));
    }
    if (_aiError) {
        _aiError->setStyleSheet(QString("font-size: %1px; color: %2;")
                                    .arg(th.fonts.caption)
                                    .arg(Th::qss(th.text.danger)));
    }

    // ── Storage page ──────────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("storageHeading")) {
        w->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                             .arg(th.fonts.base)
                             .arg(Th::qss(th.text.primary)));
    }
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
    _cacheCap->setStyleSheet(
        QString(
            "QSpinBox {"
            "  font-size: %1px; color: %2;"
            "  border: 1px solid %3; border-radius: 4px; padding: 3px 6px;"
            "}"
            "QSpinBox:focus { border-color: %4; }"
        )
            .arg(th.fonts.md)
            .arg(Th::qss(th.text.primary), Th::qss(th.divider.strong), Th::qss(th.text.link))
    );
    if (auto *w = _panel->findChild<QLabel *>("capDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    if (auto *w = _panel->findChild<QPushButton *>("clearCacheBtn")) {
        w->setStyleSheet(
            QString(
                "QPushButton {"
                "  background: #CC0000; color: white; border: none;"
                "  border-radius: 4px; font-size: %1px; font-weight: 600; padding: 0 16px;"
                "}"
                "QPushButton:hover    { background: #E00000; }"
                "QPushButton:pressed  { background: #AA0000; }"
                "QPushButton:disabled { background: %2; color: %3; }"
            )
                .arg(th.fonts.md)
                .arg(Th::qss(th.surface.raised))
                .arg(Th::qss(th.text.secondary))
        );
    }
    if (auto *w = _panel->findChild<QFrame *>("storageSep")) {
        w->setStyleSheet(QString("color: %1;").arg(Th::qss(th.divider.def)));
    }
    if (auto *w = _panel->findChild<QLabel *>("stateHeading")) {
        w->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                             .arg(th.fonts.md)
                             .arg(Th::qss(th.text.primary)));
    }
    if (auto *w = _panel->findChild<QLabel *>("stateDesc")) {
        w->setStyleSheet(QString("font-size: %1px; color: %2;")
                             .arg(th.fonts.caption)
                             .arg(Th::qss(th.text.secondary)));
    }
    if (auto *w = _panel->findChild<QPushButton *>("clearStateBtn")) {
        w->setStyleSheet(
            QString(
                "QPushButton {"
                "  background: #CC0000; color: white; border: none;"
                "  border-radius: 4px; font-size: %1px; font-weight: 600; padding: 0 16px;"
                "}"
                "QPushButton:hover    { background: #E00000; }"
                "QPushButton:pressed  { background: #AA0000; }"
                "QPushButton:disabled { background: %2; color: %3; }"
            )
                .arg(th.fonts.md)
                .arg(Th::qss(th.surface.raised))
                .arg(Th::qss(th.text.secondary))
        );
    }

    // ── System page ───────────────────────────────────────────────────
    if (auto *w = _panel->findChild<QLabel *>("sysHeading")) {
        w->setStyleSheet(QString("font-size: %1px; font-weight: 600; color: %2;")
                             .arg(th.fonts.base)
                             .arg(Th::qss(th.text.primary)));
    }
    if (auto *w = _panel->findChild<QLabel *>("verLabel")) {
        w->setStyleSheet(
            QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.secondary))
        );
    }
    if (auto *w = _panel->findChild<QGroupBox *>("updBox")) {
        w->setStyleSheet(
            QString(
                "QGroupBox { font-size: %1px; color: %2; border: none; margin-top: 4px; }"
                "QGroupBox::title { subcontrol-origin: margin; left: 0; }"
            )
                .arg(th.fonts.caption)
                .arg(Th::qss(th.text.secondary))
        );
    }
    _checkBtn->setStyleSheet(
        QString(
            "QPushButton {"
            "  background: %1; color: %2; border: none;"
            "  border-radius: 4px; font-size: %3px; padding: 0 14px;"
            "}"
            "QPushButton:hover   { background: %4; }"
            "QPushButton:pressed { background: %4; }"
            "QPushButton:disabled { color: %5; }"
        )
            .arg(Th::qss(th.surface.highlight), Th::qss(th.text.primary))
            .arg(th.fonts.md)
            .arg(Th::qss(th.surface.highlightStrong), Th::qss(th.text.tertiary))
    );
    _updateStatus->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.caption).arg(Th::qss(th.text.secondary))
    );
    _lastChecked->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.sm).arg(Th::qss(th.text.tertiary))
    );
    if (auto *w = _panel->findChild<QGroupBox *>("memBox")) {
        w->setStyleSheet(
            QString(
                "QGroupBox { font-size: %1px; color: %2; border: none; margin-top: 4px; }"
                "QGroupBox::title { subcontrol-origin: margin; left: 0; }"
            )
                .arg(th.fonts.caption)
                .arg(Th::qss(th.text.secondary))
        );
    }
    _ramLabel->setStyleSheet(
        QString("font-size: %1px; color: %2;").arg(th.fonts.md).arg(Th::qss(th.text.primary))
    );
}

void SettingsDialog::loadNotifications() {
    QSettings s("msga", "msga");
    _notifEnabled->setChecked(s.value("notifications/enabled", true).toBool());
    _notifSound->setChecked(s.value("notifications/sound", true).toBool());
    const int level = s.value("notifications/level", 1).toInt();
    (level == 0 ? _notifAll : _notifMentions)->setChecked(true);
    // Sync enabled state of child controls
    const bool on = _notifEnabled->isChecked();
    _notifAll->parentWidget()->setEnabled(on);
    _notifSound->setEnabled(on);
}

void SettingsDialog::saveNotifications() {
    QSettings s("msga", "msga");
    s.setValue("notifications/enabled", _notifEnabled->isChecked());
    s.setValue("notifications/sound", _notifSound->isChecked());
    s.setValue("notifications/level", _notifAll->isChecked() ? 0 : 1);
}

void SettingsDialog::loadAppearance() {
    const int days = QSettings("msga", "msga").value("appearance/relevantDays", 14).toInt();
    _relevantDays->setValue(std::max(1, days));

    const int idx = _language->findData(TimeFmt::language());
    _language->setCurrentIndex(idx >= 0 ? idx : 0);
    _langRestartNote->setVisible(_language->currentData().toString() != _startupLanguage);
    (TimeFmt::use24h() ? _time24 : _time12)->setChecked(true);

    for (auto *card : _themeCards)
        card->setChecked(card->themeId() == ThemeManager::instance().themeId());
}

void SettingsDialog::saveAppearance() {
    const int days = _relevantDays->value();
    QSettings("msga", "msga").setValue("appearance/relevantDays", days);

    TimeFmt::setLanguage(_language->currentData().toString());
    TimeFmt::setUse24h(_time24->isChecked());

    emit appearanceChanged(days);
    emit timeFormatChanged();
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
