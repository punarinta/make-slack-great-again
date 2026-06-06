// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "settings_dialog.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
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
#include <QDirIterator>
#include <QStandardPaths>

static constexpr int kPanelW    = 700;
static constexpr int kPanelH    = 540;
static constexpr int kPanelMinW = 480;
static constexpr int kPanelMinH = 360;
static constexpr int kEdge      = 7;

SettingsDialog::SettingsDialog(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    setMouseTracking(true);
    setGeometry(parent->rect());
    parent->installEventFilter(this);
    buildPanel();
    hide();
}

void SettingsDialog::open() {
    loadNotifications();
    refreshCacheSize();
    setGeometry(parentWidget()->rect());
    updatePanelGeometry();
    show();
    raise();
}

// ── Panel construction ────────────────────────────────────────────────────────

void SettingsDialog::buildPanel() {
    _panel = new QFrame(this);
    _panel->setObjectName("settingsPanel");
    _panel->setMinimumSize(kPanelMinW, kPanelMinH);
    _panel->resize(kPanelW, kPanelH);
    _panel->setStyleSheet(
        "QFrame#settingsPanel {"
        "  background: #FFFFFF;"
        "  border-radius: 8px;"
        "  border: 1px solid #D0D0D0;"
        "}"
    );

    auto *root = new QVBoxLayout(_panel);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header ────────────────────────────────────────────────────────
    auto *header = new QWidget(_panel);
    header->setFixedHeight(48);
    header->setStyleSheet(
        "background: #F8F8F8;"
        "border-bottom: 1px solid #E4E4E4;"
        "border-top-left-radius: 8px;"
        "border-top-right-radius: 8px;"
    );
    auto *hlay = new QHBoxLayout(header);
    hlay->setContentsMargins(20, 0, 12, 0);

    auto *titleLabel = new QLabel(tr("Settings"), header);
    titleLabel->setStyleSheet(
        "font-size: 15px; font-weight: 600; color: #1D1C1D;"
        "background: transparent; border: none;");
    hlay->addWidget(titleLabel);
    hlay->addStretch();

    auto *closeBtn = new QPushButton("\xC3\x97", header);
    closeBtn->setFixedSize(28, 28);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(
        "QPushButton {"
        "  background: transparent; color: #616061; border: none;"
        "  border-radius: 4px; font-size: 16px;"
        "}"
        "QPushButton:hover   { background: #EEEEEE; color: #1D1C1D; }"
        "QPushButton:pressed { background: #E0E0E0; }"
    );
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
    _tabs->setStyleSheet(
        "QListWidget {"
        "  background: #F4F4F4;"
        "  border: none;"
        "  border-right: 1px solid #E4E4E4;"
        "  border-bottom-left-radius: 8px;"
        "  outline: none;"
        "  padding: 8px 0;"
        "}"
        "QListWidget::item {"
        "  padding: 9px 14px;"
        "  color: #1D1C1D;"
        "  font-size: 13px;"
        "  border-radius: 4px;"
        "  margin: 1px 6px;"
        "}"
        "QListWidget::item:selected {"
        "  background: #E0E0E0;"
        "  color: #1D1C1D;"
        "}"
        "QListWidget::item:hover:!selected {"
        "  background: #EBEBEB;"
        "}"
    );
    _tabs->addItem(tr("Notifications"));
    _tabs->addItem(tr("Storage"));
    _tabs->setCurrentRow(0);
    blay->addWidget(_tabs);

    _stack = new QStackedWidget(body);
    blay->addWidget(_stack, 1);

    connect(_tabs, &QListWidget::currentRowChanged,
            _stack, &QStackedWidget::setCurrentIndex);

    // ── Notifications page ────────────────────────────────────────────
    auto *notifPage = new QWidget;
    auto *nlay = new QVBoxLayout(notifPage);
    nlay->setContentsMargins(24, 20, 24, 20);
    nlay->setSpacing(16);

    auto *notifHeading = new QLabel(tr("Notifications"), notifPage);
    notifHeading->setStyleSheet("font-size: 14px; font-weight: 600; color: #1D1C1D;");
    nlay->addWidget(notifHeading);

    _notifEnabled = new QCheckBox(tr("Enable desktop notifications"), notifPage);
    _notifEnabled->setStyleSheet("font-size: 13px; color: #1D1C1D;");
    nlay->addWidget(_notifEnabled);

    auto *levelBox = new QGroupBox(tr("Notify me about"), notifPage);
    levelBox->setStyleSheet(
        "QGroupBox { font-size: 12px; color: #616061; border: none; margin-top: 4px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 0; }");
    auto *levelLayout = new QVBoxLayout(levelBox);
    levelLayout->setSpacing(6);
    levelLayout->setContentsMargins(0, 12, 0, 0);

    _notifAll      = new QRadioButton(tr("All new messages"), levelBox);
    _notifMentions = new QRadioButton(tr("Direct messages and mentions only"), levelBox);
    _notifAll->setStyleSheet("font-size: 13px; color: #1D1C1D;");
    _notifMentions->setStyleSheet("font-size: 13px; color: #1D1C1D;");

    auto *group = new QButtonGroup(levelBox);
    group->addButton(_notifAll,      0);
    group->addButton(_notifMentions, 1);

    levelLayout->addWidget(_notifAll);
    levelLayout->addWidget(_notifMentions);
    nlay->addWidget(levelBox);

    _notifSound = new QCheckBox(tr("Play a sound for notifications"), notifPage);
    _notifSound->setStyleSheet("font-size: 13px; color: #1D1C1D;");
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
    saveBtn->setFixedHeight(34);
    saveBtn->setMinimumWidth(80);
    saveBtn->setCursor(Qt::PointingHandCursor);
    saveBtn->setStyleSheet(
        "QPushButton {"
        "  background: #007A5A; color: white; border: none;"
        "  border-radius: 4px; font-size: 13px; font-weight: 600; padding: 0 16px;"
        "}"
        "QPushButton:hover   { background: #148567; }"
        "QPushButton:pressed { background: #005E45; }"
    );
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        saveNotifications();
        hide();
    });
    btnRow->addWidget(saveBtn);
    nlay->addLayout(btnRow);

    _stack->addWidget(notifPage);

    // ── Storage page ──────────────────────────────────────────────────
    auto *storagePage = new QWidget;
    auto *slay = new QVBoxLayout(storagePage);
    slay->setContentsMargins(24, 20, 24, 20);
    slay->setSpacing(16);

    auto *storageHeading = new QLabel(tr("Storage"), storagePage);
    storageHeading->setStyleSheet("font-size: 14px; font-weight: 600; color: #1D1C1D;");
    slay->addWidget(storageHeading);

    auto *sizeRow = new QHBoxLayout;
    auto *sizePrefixLabel = new QLabel(tr("Cache size:"), storagePage);
    sizePrefixLabel->setStyleSheet("font-size: 13px; color: #616061;");
    sizeRow->addWidget(sizePrefixLabel);

    _cacheSize = new QLabel("–", storagePage);
    _cacheSize->setStyleSheet("font-size: 13px; font-weight: 600; color: #1D1C1D;");
    sizeRow->addWidget(_cacheSize);
    sizeRow->addStretch();
    slay->addLayout(sizeRow);

    auto *cacheDesc = new QLabel(
        tr("Conversations, user names, message history, and image thumbnails\n"
           "stored locally to speed up startup."),
        storagePage);
    cacheDesc->setStyleSheet("font-size: 12px; color: #616061;");
    cacheDesc->setWordWrap(true);
    slay->addWidget(cacheDesc);

    slay->addStretch();

    auto *clearRow = new QHBoxLayout;
    clearRow->addStretch();
    auto *clearBtn = new QPushButton(tr("Clear Cache"), storagePage);
    clearBtn->setFixedHeight(34);
    clearBtn->setMinimumWidth(110);
    clearBtn->setCursor(Qt::PointingHandCursor);
    clearBtn->setStyleSheet(
        "QPushButton {"
        "  background: #CC0000; color: white; border: none;"
        "  border-radius: 4px; font-size: 13px; font-weight: 600; padding: 0 16px;"
        "}"
        "QPushButton:hover   { background: #E00000; }"
        "QPushButton:pressed { background: #AA0000; }"
    );
    connect(clearBtn, &QPushButton::clicked, this, &SettingsDialog::clearCache);
    clearRow->addWidget(clearBtn);
    slay->addLayout(clearRow);

    _stack->addWidget(storagePage);
    root->addWidget(body, 1);

    updatePanelGeometry();
}

void SettingsDialog::loadNotifications() {
    QSettings s("msga", "msga");
    _notifEnabled->setChecked( s.value("notifications/enabled",  false).toBool());
    _notifSound->setChecked(   s.value("notifications/sound",    true).toBool());
    const int level =          s.value("notifications/level",    1).toInt();
    (level == 0 ? _notifAll : _notifMentions)->setChecked(true);
    // Sync enabled state of child controls
    const bool on = _notifEnabled->isChecked();
    _notifAll->parentWidget()->setEnabled(on);
    _notifSound->setEnabled(on);
}

void SettingsDialog::saveNotifications() {
    QSettings s("msga", "msga");
    s.setValue("notifications/enabled", _notifEnabled->isChecked());
    s.setValue("notifications/sound",   _notifSound->isChecked());
    s.setValue("notifications/level",   _notifAll->isChecked() ? 0 : 1);
}

static QString formatBytes(qint64 bytes) {
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QString("%1 KB").arg(bytes / 1024);
    return QString("%1 MB").arg(bytes / (1024 * 1024));
}

static qint64 dirSizeBytes(const QString &path) {
    qint64 total = 0;
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
    const QString cacheDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cache";
    QDir(cacheDir).removeRecursively();
    refreshCacheSize();
}

void SettingsDialog::updatePanelGeometry() {
    if (!_panel) return;
    const QSize ps = _panel->size();
    _panel->move((width() - ps.width()) / 2, (height() - ps.height()) / 2);
}

// ── Edge detection & resize helpers ──────────────────────────────────────────

SettingsDialog::Dir SettingsDialog::edgeAt(const QPoint &p) const {
    if (!_panel) return Dir::None;
    const QRect r = _panel->geometry();
    if (!r.adjusted(-kEdge, -kEdge, kEdge, kEdge).contains(p) || r.contains(p))
        return Dir::None;

    const bool n = p.y() < r.top()    + kEdge;
    const bool s = p.y() > r.bottom() - kEdge;
    const bool w = p.x() < r.left()   + kEdge;
    const bool e = p.x() > r.right()  - kEdge;

    if (n && w) return Dir::NW;
    if (n && e) return Dir::NE;
    if (s && w) return Dir::SW;
    if (s && e) return Dir::SE;
    if (n)      return Dir::N;
    if (s)      return Dir::S;
    if (w)      return Dir::W;
    if (e)      return Dir::E;
    return Dir::None;
}

Qt::CursorShape SettingsDialog::cursorFor(Dir d) {
    switch (d) {
    case Dir::N:  case Dir::S:  return Qt::SizeVerCursor;
    case Dir::E:  case Dir::W:  return Qt::SizeHorCursor;
    case Dir::NE: case Dir::SW: return Qt::SizeBDiagCursor;
    case Dir::NW: case Dir::SE: return Qt::SizeFDiagCursor;
    default:                    return Qt::ArrowCursor;
    }
}

// ── Painting & events ─────────────────────────────────────────────────────────

void SettingsDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 150));
}

void SettingsDialog::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const Dir dir = edgeAt(e->pos());
    if (dir == Dir::None) return;
    _resizeDir   = dir;
    _dragStart   = e->pos();
    _panelAtDrag = _panel->geometry();
    grabMouse(cursorFor(dir));
}

void SettingsDialog::mouseMoveEvent(QMouseEvent *e) {
    if (_resizeDir != Dir::None) {
        const QPoint delta = e->pos() - _dragStart;
        QRect r = _panelAtDrag;

        switch (_resizeDir) {
        case Dir::E:  r.setRight( r.right()  + delta.x()); break;
        case Dir::W:  r.setLeft(  r.left()   + delta.x()); break;
        case Dir::S:  r.setBottom(r.bottom() + delta.y()); break;
        case Dir::N:  r.setTop(   r.top()    + delta.y()); break;
        case Dir::SE: r.setRight( r.right()  + delta.x());
                      r.setBottom(r.bottom() + delta.y()); break;
        case Dir::SW: r.setLeft(  r.left()   + delta.x());
                      r.setBottom(r.bottom() + delta.y()); break;
        case Dir::NE: r.setRight( r.right()  + delta.x());
                      r.setTop(   r.top()    + delta.y()); break;
        case Dir::NW: r.setLeft(  r.left()   + delta.x());
                      r.setTop(   r.top()    + delta.y()); break;
        default: break;
        }

        const QSize minS = _panel->minimumSize();
        if (r.width() < minS.width()) {
            const bool movingLeft = (_resizeDir == Dir::W || _resizeDir == Dir::NW || _resizeDir == Dir::SW);
            if (movingLeft) r.setLeft(r.right()   - minS.width());
            else            r.setRight(r.left()   + minS.width());
        }
        if (r.height() < minS.height()) {
            const bool movingTop = (_resizeDir == Dir::N || _resizeDir == Dir::NW || _resizeDir == Dir::NE);
            if (movingTop) r.setTop(r.bottom()    - minS.height());
            else           r.setBottom(r.top()    + minS.height());
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
