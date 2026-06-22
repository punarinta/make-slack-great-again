// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "profile_dialog.h"

#include "backend/domain.h"
#include "session/session.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"

#include <QApplication>

#include <memory>
#include <QEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kAvatarDiameter = 120;
constexpr int kMaxNameLen     = 80;

// Rounded-square corner radius for the avatar.
int avatarRadius(int diameter) {
    return diameter / 6;
}
} // namespace

// ── ProfileAvatarWidget ─────────────────────────────────────────────────────

ProfileAvatarWidget::ProfileAvatarWidget(int diameter, QWidget *parent)
    : QWidget(parent), _diameter(diameter) {
    setFixedSize(diameter, diameter);
    setCursor(Qt::PointingHandCursor);
    _tooltip = new PopupTooltip(this);
}

void ProfileAvatarWidget::setAvatar(const QPixmap &pixmap, const QString &initial) {
    _avatar  = pixmap;
    _initial = initial;
    update();
}

void ProfileAvatarWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r(0, 0, _diameter, _diameter);
    const int   radius = avatarRadius(_diameter);

    QPainterPath clip;
    clip.addRoundedRect(QRectF(r), radius, radius);
    p.setClipPath(clip);

    if (!_avatar.isNull()) {
        const qreal dpr    = devicePixelRatioF();
        QPixmap     scaled = _avatar.scaled(
            r.size() * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation
        );
        scaled.setDevicePixelRatio(dpr);
        p.drawPixmap(r, scaled);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(Th::c().presence.away);
        p.drawRoundedRect(r, radius, radius);
        if (!_initial.isEmpty()) {
            p.setPen(Qt::white);
            QFont f = QApplication::font();
            f.setBold(true);
            f.setPointSizeF(_diameter * 0.34);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, _initial.left(1).toUpper());
        }
    }

    if (_hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawRoundedRect(r, radius, radius);
        p.setClipping(false);
        const int     icon = qRound(_diameter * 0.30);
        const QPixmap cam =
            svgPixmap(QStringLiteral(":/ui/camera.svg"), QSize(icon, icon), QColor(255, 255, 255));
        p.drawPixmap(QRect((_diameter - icon) / 2, (_diameter - icon) / 2, icon, icon), cam);
    }
}

void ProfileAvatarWidget::enterEvent(QEnterEvent *) {
    _hover = true;
    _tooltip->showAbove(tr("Change photo"), QRect(mapToGlobal(QPoint(0, 0)), size()));
    update();
}

void ProfileAvatarWidget::leaveEvent(QEvent *) {
    _hover = false;
    _tooltip->hide();
    update();
}

void ProfileAvatarWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        _tooltip->hide();
        emit clicked();
    }
}

// ── ProfileDialog ───────────────────────────────────────────────────────────

ProfileDialog::ProfileDialog(Session *session, ImageCache *imgCache, QWidget *parent)
    : AppDialog(tr("Profile"), parent), _session(session), _imgCache(imgCache) {
    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;

    // ── Avatar (centered, hover-to-change) ────────────────────────────────
    auto *avatarRow = new QHBoxLayout;
    avatarRow->addStretch();
    _avatar = new ProfileAvatarWidget(kAvatarDiameter);
    avatarRow->addWidget(_avatar);
    avatarRow->addStretch();
    cl->addLayout(avatarRow);
    cl->addSpacing(sp.xl);

    auto addField = [&](const QString &labelText, QLabel *&label, StyledLineEdit *&edit) {
        label    = new QLabel(labelText);
        QFont lf = label->font();
        lf.setBold(true);
        label->setFont(lf);
        cl->addWidget(label);
        edit = new StyledLineEdit;
        cl->addWidget(edit);
        cl->addSpacing(sp.md);
    };

    addField(tr("Name"), _nameLabel, _nameEdit);
    _nameEdit->setMaxLength(kMaxNameLen);
    _nameEdit->setPlaceholderText(tr("Your display name"));
    addField(tr("Email"), _emailLabel, _emailEdit);
    _emailEdit->setPlaceholderText(tr("name@example.com"));
    addField(tr("Phone"), _phoneLabel, _phoneEdit);
    _phoneEdit->setPlaceholderText(tr("Optional"));

    _status = new QLabel;
    _status->setWordWrap(true);
    _status->setVisible(false);
    cl->addWidget(_status);

    cl->addSpacing(sp.md);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _saveBtn   = new StyledButton(tr("Save Changes"), StyledButton::Variant::Primary);
    btnRow->addWidget(_cancelBtn);
    btnRow->addSpacing(sp.md);
    btnRow->addWidget(_saveBtn);
    cl->addLayout(btnRow);

    connect(_avatar, &ProfileAvatarWidget::clicked, this, &ProfileDialog::pickAndUploadPhoto);
    connect(_cancelBtn, &QPushButton::clicked, this, &AppDialog::reject);
    connect(_saveBtn, &QPushButton::clicked, this, &ProfileDialog::save);

    // Seed the avatar/initial from the already-known user entry so something
    // shows before users.profile.get returns.
    if (_session) {
        if (const auto *me = _session->findUser(_session->meUserId())) {
            _initial = me->displayLabel();
            setAvatarUrl(me->avatarUrl);
        }
    }

    applyTheme();
    updateCard();

    loadProfile();
}

void ProfileDialog::loadProfile() {
    if (!_session)
        return;
    _session->loadMyProfile([this](MyProfile p) {
        _loaded            = true;
        _loadedDisplayName = p.displayName;
        _loadedEmail       = p.email;
        _loadedPhone       = p.phone;
        // Prefer the display name; fall back to the full name so the field is
        // never blank when only real_name is set.
        _nameEdit->setText(p.displayName.isEmpty() ? p.realName : p.displayName);
        _emailEdit->setText(p.email);
        _phoneEdit->setText(p.phone);
        if (!p.displayName.isEmpty())
            _initial = p.displayName;
        else if (!p.realName.isEmpty())
            _initial = p.realName;
        if (!p.avatarUrl.isEmpty())
            setAvatarUrl(p.avatarUrl);
        else
            _avatar->setAvatar({}, _initial);
        updateCard();
    });
}

void ProfileDialog::setAvatarUrl(const QString &url) {
    _avatarUrl = url;
    if (url.isEmpty() || !_imgCache) {
        _avatar->setAvatar({}, _initial);
        return;
    }
    const QPixmap cached = _imgCache->get(url);
    if (!cached.isNull()) {
        _avatar->setAvatar(cached, _initial);
        return;
    }
    _avatar->setAvatar({}, _initial);
    // Persistent (not single-shot): `loaded` fires for every image, so a
    // single-shot connection would be consumed by the first unrelated image to
    // finish, and we'd miss our own. Tear down once OUR url arrives.
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn =
        connect(_imgCache, &ImageCache::loaded, this, [this, url, conn](const QString &loadedUrl) {
            if (loadedUrl != url)
                return;
            QObject::disconnect(*conn);
            if (url == _avatarUrl)
                _avatar->setAvatar(_imgCache->get(url), _initial);
        });
}

void ProfileDialog::pickAndUploadPhoto() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a profile photo"), {}, tr("Images (*.png *.jpg *.jpeg *.gif)")
    );
    if (path.isEmpty() || !_session)
        return;

    setStatusMessage(tr("Uploading photo…"), /*error=*/false);
    _avatar->setEnabled(false);
    _session->setPhoto(path, [this](bool ok, QString err) {
        _avatar->setEnabled(true);
        if (!ok) {
            setStatusMessage(tr("Could not upload photo: %1").arg(err), /*error=*/true);
            return;
        }
        setStatusMessage(tr("Photo updated."), /*error=*/false);
        // Refresh from the (now patched) user entry.
        if (const auto *me = _session->findUser(_session->meUserId()))
            setAvatarUrl(me->avatarUrl);
    });
}

void ProfileDialog::save() {
    if (!_session || !_loaded)
        return;

    QHash<QString, QString> fields;
    const QString           name  = _nameEdit->text().trimmed();
    const QString           email = _emailEdit->text().trimmed();
    const QString           phone = _phoneEdit->text().trimmed();

    if (name != _loadedDisplayName)
        fields.insert(QStringLiteral("display_name"), name);
    if (email != _loadedEmail)
        fields.insert(QStringLiteral("email"), email);
    if (phone != _loadedPhone)
        fields.insert(QStringLiteral("phone"), phone);

    if (fields.isEmpty()) {
        accept();
        return;
    }

    _saveBtn->setEnabled(false);
    setStatusMessage(tr("Saving…"), /*error=*/false);
    _session->updateProfile(fields, [this](bool ok, QString err) {
        if (!ok) {
            _saveBtn->setEnabled(true);
            setStatusMessage(tr("Could not save: %1").arg(err), /*error=*/true);
            return;
        }
        accept();
    });
}

void ProfileDialog::setStatusMessage(const QString &text, bool error) {
    _status->setText(text);
    _status->setVisible(!text.isEmpty());
    _status->setStyleSheet(QString("color: %1; font-size: %2px;")
                               .arg(Th::qss(error ? Th::c().text.danger : Th::c().text.secondary))
                               .arg(Th::c().fonts.sm));
    updateCard();
}

void ProfileDialog::applyTheme() {
    AppDialog::applyTheme();

    const QString labelStyle = QString("color: %1;").arg(Th::qss(Th::c().text.primary));
    for (QLabel *l : {_nameLabel, _emailLabel, _phoneLabel})
        if (l)
            l->setStyleSheet(labelStyle);

    // Cancel/Save buttons self-theme (StyledButton).
}
