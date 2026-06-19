// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "status_dialog.h"

#include "backend/domain.h"
#include "session/session.h"
#include "ui/dropdown/dropdown.h"
#include "ui/emoji_picker/emoji_picker_popup.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/message_list/message_render.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/emoji.h"
#include "util/emoji_font.h"

#include <QDateTime>
#include <QEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>

namespace {
constexpr int kMaxStatusLen = 100; // Slack caps status_text at 100 chars
constexpr int kEmojiBtn     = 30;  // emoji prefix button side
constexpr int kPresetEmoji  = 20;  // preset-row emoji glyph pixel size

// "Clear after" combo indices — kept in sync with buildClearAfter().
enum ClearAfter { DontClear = 0, Min30, Hour1, Hour4, Today, ThisWeek };

// A flat, full-width clickable row (emoji + rich label) used for the preset
// suggestions. No Q_OBJECT/signals — a plain std::function avoids a moc pass.
class PresetRow : public QWidget {
public:
    PresetRow(const QString &emoji, const QString &name, const QString &duration, QWidget *parent)
        : QWidget(parent) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_StyledBackground);
        auto       *row = new QHBoxLayout(this);
        const auto &sp  = Th::c().spacing;
        row->setContentsMargins(sp.md, sp.sm, sp.md, sp.sm);
        row->setSpacing(sp.md);

        auto *glyph = new QLabel(Emoji::fromName(emoji), this);
        glyph->setFont(emojiFont(kPresetEmoji));
        glyph->setFixedWidth(kPresetEmoji + 6);
        row->addWidget(glyph);

        _label = new QLabel(this);
        _label->setTextFormat(Qt::RichText);
        _name     = name;
        _duration = duration;
        row->addWidget(_label, 1);

        applyTheme();
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
            applyTheme();
        });
    }

    void applyTheme() {
        _label->setText(QString(
                            "<span style=\"color:%1;font-weight:bold;\">%2</span>"
                            "<span style=\"color:%3;\">&nbsp;&nbsp;—&nbsp;&nbsp;%4</span>"
        )
                            .arg(
                                Th::qss(Th::c().text.primary),
                                _name.toHtmlEscaped(),
                                Th::qss(Th::c().text.secondary),
                                _duration.toHtmlEscaped()
                            ));
        setStyleSheet(QString(
                          "PresetRow { border-radius: 6px; background: transparent; }"
                          "PresetRow:hover { background: %1; }"
        )
                          .arg(Th::qss(Th::c().surface.sunken)));
    }

    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && onClick)
            onClick();
    }

private:
    QLabel *_label = nullptr;
    QString _name;
    QString _duration;
};
} // namespace

StatusDialog::StatusDialog(
    Session *session, ImageCache *imgCache, const QString &workspaceName, QWidget *parent
)
    : AppDialog(tr("Set a status"), parent), _session(session), _imgCache(imgCache) {
    buildInput();
    buildClearAfter();
    buildPresets(workspaceName);
    buildButtons();

    // Prefill with the current status so re-opening shows what's set. Only
    // offer "Clear status" when there's actually something set to clear.
    if (_session) {
        if (const auto *me = _session->findUser(_session->meUserId())) {
            _textEdit->setText(me->statusText);
            setEmoji(me->statusEmoji);
            _clearBtn->setVisible(!me->statusText.isEmpty() || !me->statusEmoji.isEmpty());
        }
    }

    applyTheme();
    updateCard();
    _textEdit->setFocus();
}

void StatusDialog::buildInput() {
    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;

    // Bordered input box: clickable emoji button + text field (mirrors Slack).
    _inputBox = new QFrame;
    _inputBox->setObjectName("statusInputBox");
    auto *box = _inputBox;
    auto *row = new QHBoxLayout(box);
    row->setContentsMargins(sp.md, sp.sm, sp.md, sp.sm);
    row->setSpacing(sp.md);

    _emojiBtn = new QToolButton(box);
    _emojiBtn->setAutoRaise(true);
    _emojiBtn->setFixedSize(kEmojiBtn, kEmojiBtn);
    _emojiBtn->setCursor(Qt::PointingHandCursor);
    _emojiBtn->setIconSize(QSize(20, 20));
    _emojiBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(_emojiBtn, &QToolButton::clicked, this, &StatusDialog::openEmojiPicker);
    row->addWidget(_emojiBtn);

    _textEdit = new QLineEdit(box);
    _textEdit->setFrame(false);
    _textEdit->setMaxLength(kMaxStatusLen);
    _textEdit->setPlaceholderText(tr("What's your status?"));
    _textEdit->installEventFilter(this);
    connect(_textEdit, &QLineEdit::returnPressed, this, &StatusDialog::save);
    row->addWidget(_textEdit, 1);

    cl->addWidget(box);
    cl->addSpacing(sp.lg);
}

void StatusDialog::buildPresets(const QString &workspaceName) {
    static const QVector<Preset> kPresets = {
        {QStringLiteral("calendar"), tr("In a meeting"), ClearAfter::Hour1},
        {QStringLiteral("bus"), tr("Commuting"), ClearAfter::Min30},
        {QStringLiteral("face_with_thermometer"), tr("Out sick"), ClearAfter::Today},
        {QStringLiteral("palm_tree"), tr("Vacationing"), ClearAfter::DontClear},
        {QStringLiteral("house_with_garden"), tr("Working remotely"), ClearAfter::Today},
    };
    static const QStringList kDurations = {
        tr("Don't clear"),
        tr("30 minutes"),
        tr("1 hour"),
        tr("4 hours"),
        tr("Today"),
        tr("This week")
    };

    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;
    auto       *header =
        new QLabel(workspaceName.isEmpty() ? tr("Suggestions") : tr("For %1").arg(workspaceName));
    header->setObjectName("statusSectionHeader");
    QFont hf = header->font();
    hf.setBold(true);
    hf.setPixelSize(Th::c().fonts.sm);
    header->setFont(hf);
    cl->addWidget(header);
    cl->addSpacing(sp.xs);

    for (const Preset &p : kPresets) {
        auto *roww    = new PresetRow(p.emoji, p.text, kDurations.value(p.clearAfter), this);
        roww->onClick = [this, p] { applyPreset(p); };
        cl->addWidget(roww);
    }
    cl->addSpacing(sp.lg);
}

void StatusDialog::buildClearAfter() {
    auto       *cl  = contentLayout();
    const auto &sp  = Th::c().spacing;
    auto       *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(sp.md);

    auto *lbl = new QLabel(tr("Clear after"));
    lbl->setObjectName("statusClearAfterLabel");
    row->addWidget(lbl);

    _clearAfter = new Dropdown;
    // Order MUST match the ClearAfter enum.
    _clearAfter->setItems({
        tr("Don't clear"),
        tr("30 minutes"),
        tr("1 hour"),
        tr("4 hours"),
        tr("Today"),
        tr("This week"),
    });
    row->addWidget(_clearAfter, 1);

    cl->addLayout(row);
    cl->addSpacing(sp.md);
}

void StatusDialog::buildButtons() {
    auto *cl     = contentLayout();
    auto *btnRow = new QHBoxLayout;
    _clearBtn    = new QPushButton(tr("Clear status"));
    _clearBtn->setCursor(Qt::PointingHandCursor);
    _clearBtn->hide(); // shown in the ctor only when a status is set
    btnRow->addWidget(_clearBtn);
    btnRow->addStretch();
    _cancelBtn = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _saveBtn   = new StyledButton(tr("Save"), StyledButton::Variant::Primary);
    btnRow->addWidget(_cancelBtn);
    btnRow->addSpacing(Th::c().spacing.md);
    btnRow->addWidget(_saveBtn);
    cl->addLayout(btnRow);

    connect(_clearBtn, &QPushButton::clicked, this, [this] {
        if (_session)
            _session->setStatus({}, {}, 0);
        accept();
    });
    connect(_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(_saveBtn, &QPushButton::clicked, this, &StatusDialog::save);
}

void StatusDialog::openEmojiPicker() {
    if (!_emojiPicker) {
        _emojiPicker = new EmojiPickerPopup(this);
        connect(_emojiPicker, &EmojiPickerPopup::emojiSelected, this, [this](const QString &name) {
            setEmoji(name);
            _textEdit->setFocus();
        });
    }
    if (_session)
        _emojiPicker->setSession(_session);
    _emojiPicker->setImageCache(_imgCache);
    const QPoint pos = _emojiBtn->mapToGlobal(QPoint(0, _emojiBtn->height() + 4));
    _emojiPicker->open(pos);
}

void StatusDialog::setEmoji(const QString &bareName) {
    _emoji = bareName;
    updateEmojiButton();
}

void StatusDialog::updateEmojiButton() {
    if (!_emojiBtn)
        return;
    if (_emoji.isEmpty()) {
        // Default smiley placeholder, like Slack's empty-status sheet.
        _emojiBtn->setText({});
        _emojiBtn->setIcon(
            svgIcon(QStringLiteral(":/ui/smile.svg"), QSize(20, 20), Th::c().text.tertiary)
        );
        _emojiBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        return;
    }
    const auto resolved = MsgRender::resolveEmojiRich(_emoji, _session);
    if (!resolved.imageUrl.isEmpty() && _imgCache) {
        // Workspace custom emoji: render its image into the button.
        const QString url = resolved.imageUrl;
        const QPixmap px  = _imgCache->get(url);
        auto          set = [this](const QPixmap &p) {
            _emojiBtn->setIcon(QIcon(p));
            _emojiBtn->setIconSize(QSize(20, 20));
            _emojiBtn->setText({});
            _emojiBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        };
        if (!px.isNull()) {
            set(px);
        } else {
            const QString want = _emoji;
            // Persistent (not single-shot): `loaded` fires for every image, so a
            // single-shot connection would be consumed by the first unrelated
            // image to finish and we'd miss our own. Tear down once OUR url arrives.
            auto          conn = std::make_shared<QMetaObject::Connection>();
            *conn              = connect(
                _imgCache,
                &ImageCache::loaded,
                this,
                [this, url, set, want, conn](const QString &loadedUrl) {
                    if (loadedUrl != url)
                        return;
                    QObject::disconnect(*conn);
                    if (_emoji == want) // still showing this emoji
                        set(_imgCache->get(url));
                }
            );
        }
        return;
    }
    // Built-in Unicode emoji glyph.
    _emojiBtn->setIcon({});
    _emojiBtn->setFont(emojiFont(20));
    _emojiBtn->setText(resolved.unicode);
    _emojiBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
}

void StatusDialog::applyPreset(const Preset &p) {
    setEmoji(p.emoji);
    _textEdit->setText(p.text);
    _clearAfter->setCurrentIndex(p.clearAfter);
    _textEdit->setFocus();
}

qint64 StatusDialog::expirationTs() const {
    const QDateTime now = QDateTime::currentDateTime();
    switch (_clearAfter ? _clearAfter->currentIndex() : ClearAfter::DontClear) {
    case ClearAfter::Min30:
        return now.addSecs(30 * 60).toSecsSinceEpoch();
    case ClearAfter::Hour1:
        return now.addSecs(60 * 60).toSecsSinceEpoch();
    case ClearAfter::Hour4:
        return now.addSecs(4 * 60 * 60).toSecsSinceEpoch();
    case ClearAfter::Today:
        // End of the local day (next local midnight).
        return now.date().addDays(1).startOfDay().toSecsSinceEpoch();
    case ClearAfter::ThisWeek: {
        // Next Monday at local midnight (1 = Monday … 7 = Sunday).
        const int   daysToMonday = (8 - now.date().dayOfWeek()) % 7;
        const QDate monday       = now.date().addDays(daysToMonday == 0 ? 7 : daysToMonday);
        return monday.startOfDay().toSecsSinceEpoch();
    }
    case ClearAfter::DontClear:
    default:
        return 0;
    }
}

void StatusDialog::save() {
    if (!_session) {
        accept();
        return;
    }
    const QString text = _textEdit->text().trimmed();
    // Clearing: no text and no emoji → wipe the status entirely.
    if (text.isEmpty() && _emoji.isEmpty()) {
        _session->setStatus({}, {}, 0);
        accept();
        return;
    }
    const QString emoji = _emoji.isEmpty() ? QString() : QStringLiteral(":%1:").arg(_emoji);
    _session->setStatus(emoji, text, expirationTs());
    accept();
}

void StatusDialog::applyTheme() {
    AppDialog::applyTheme();

    updateEmojiButton();
    styleInputBox(_textEdit && _textEdit->hasFocus());

    if (_textEdit)
        _textEdit->setStyleSheet(
            QString(
                "QLineEdit { background: transparent; border: none; color: %1; font-size: %2px; }"
            )
                .arg(Th::qss(Th::c().text.primary))
                .arg(Th::c().fonts.base)
        );

    if (_emojiBtn)
        _emojiBtn->setStyleSheet(QString(
                                     "QToolButton { border: none; background: transparent; "
                                     "border-radius: 6px; }"
                                     "QToolButton:hover { background: %1; }"
        )
                                     .arg(Th::qss(Th::c().surface.sunken)));

    if (auto *h = findChild<QLabel *>(QStringLiteral("statusSectionHeader")))
        h->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.secondary)));
    if (auto *l = findChild<QLabel *>(QStringLiteral("statusClearAfterLabel")))
        l->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));

    if (_clearBtn)
        _clearBtn->setStyleSheet(
            QString(
                "QPushButton {"
                "  border: none; border-radius: 6px;"
                "  padding: 8px 12px; background: transparent; color: %1;"
                "}"
                "QPushButton:hover { background: %2; }"
            )
                .arg(Th::qss(Th::c().text.danger), Th::qss(Th::c().surface.sunken))
        );

    // Cancel/Save buttons self-theme (StyledButton).
}

void StatusDialog::styleInputBox(bool focused) {
    if (!_inputBox)
        return;
    const QColor border = focused ? Th::c().composer.borderFocus : Th::c().composer.border;
    _inputBox->setStyleSheet(QString(
                                 "QFrame#statusInputBox {"
                                 "  border: %1px solid %2; border-radius: 6px; background: %3;"
                                 "}"
    )
                                 .arg(focused ? 2 : 1)
                                 .arg(Th::qss(border), Th::qss(Th::c().surface.raised)));
}

bool StatusDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _textEdit) {
        if (event->type() == QEvent::FocusIn)
            styleInputBox(true);
        else if (event->type() == QEvent::FocusOut)
            styleInputBox(false);
    }
    return AppDialog::eventFilter(obj, event);
}
