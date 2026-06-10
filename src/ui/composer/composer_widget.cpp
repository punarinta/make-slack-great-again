// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "composer_widget.h"
#include "formatting_toolbar.h"
#include "attachment_strip.h"
#include "edit_mode_banner.h"
#include "ui/emoji_picker/emoji_picker_popup.h"
#include "mention_completer.h"
#include "ui/mention_popup/mention_popup.h"
#include "session/session.h"
#include "ui/icon_utils.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QKeyEvent>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QTextCursor>
#include <QCursor>
#include <QResizeEvent>
#include <QDateTimeEdit>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QPixmap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtMath>
#include <QAbstractTextDocumentLayout>
#include <QTimer>

static constexpr int kMinEditHeight = 40;

static constexpr QSize kToolIconSize{18, 18};

// ── Helpers ───────────────────────────────────────────────────────────────────

static QString sc(const char *keys) {
#ifdef Q_OS_MAC
    QString s = QString::fromLatin1(keys);
    s.replace("Ctrl+Alt+Shift+", "⌘⌥⇧");
    s.replace("Ctrl+Shift+", "⌘⇧");
    s.replace("Ctrl+", "⌘");
    return s;
#else
    return QString::fromLatin1(keys);
#endif
}

static QString tip(const QString &label, const char *shortcut = nullptr) {
    return shortcut ? label + " (" + sc(shortcut) + ")" : label;
}

static QFrame *makeVSep(QWidget *parent) {
    auto *sep = new QFrame(parent);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedSize(1, 16);
    sep->setStyleSheet("QFrame { color: " + Th::qss(Th::c().composer.toolbarBorder) + "; }");
    return sep;
}

// ── LinkPopup ─────────────────────────────────────────────────────────────────

struct LinkPopupTexts {
    QString urlLabel;
    QString displayLabel;
    QString insertLabel;
    QString cancelLabel;
};

class LinkPopup : public QWidget {
public:
    using Callback = std::function<void(const QString &url, const QString &label)>;

    LinkPopup(QWidget *parent, const LinkPopupTexts &t)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setObjectName("linkPopup");
        setStyleSheet(
            QString(
                "QWidget#linkPopup {"
                "  background: %1;"
                "  border: 1px solid %2;"
                "  border-radius: 8px;"
                "}"
                "QLabel { border: none; font-size: %6px; color: %3; background: transparent; }"
                "QLineEdit {"
                "  border: 1px solid %2;"
                "  border-radius: 4px;"
                "  padding: 4px 8px;"
                "  font-size: %7px;"
                "  color: %4;"
                "  background: %1;"
                "}"
                "QLineEdit:focus { border-color: %5; }"
            )
                .arg(Th::qss(Th::c().surface.raised))
                .arg(Th::qss(Th::c().divider.strong))
                .arg(Th::qss(Th::c().text.secondary))
                .arg(Th::qss(Th::c().text.primary))
                .arg(Th::qss(Th::c().accent.def))
                .arg(Th::c().fonts.caption)
                .arg(Th::c().fonts.md)
        );

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(12, 12, 12, 12);
        lay->setSpacing(8);

        _urlEdit = new QLineEdit(this);
        _urlEdit->setPlaceholderText("https://");
        _urlEdit->setMinimumWidth(280);
        _textEdit = new QLineEdit(this);

        auto *insertBtn = new QPushButton(t.insertLabel, this);
        insertBtn->setCursor(Qt::PointingHandCursor);
        insertBtn->setStyleSheet(
            QString(
                "QPushButton { background:%1; color:white; border:none;"
                "  border-radius:4px; padding:4px 14px; font-size:%4px; font-weight:600; }"
                "QPushButton:hover   { background:%2; }"
                "QPushButton:pressed { background:%3; }"
            )
                .arg(Th::qss(Th::c().accent.def))
                .arg(Th::qss(Th::c().accent.hover))
                .arg(Th::qss(Th::c().accent.pressed))
                .arg(Th::c().fonts.md)
        );

        auto *cancelBtn = new QPushButton(t.cancelLabel, this);
        cancelBtn->setCursor(Qt::PointingHandCursor);
        cancelBtn->setStyleSheet(
            QString(
                "QPushButton { background:transparent; color:%1; border:1px solid %2;"
                "  border-radius:4px; padding:4px 14px; font-size:%4px; }"
                "QPushButton:hover { background:%3; }"
            )
                .arg(Th::qss(Th::c().text.secondary))
                .arg(Th::qss(Th::c().divider.strong))
                .arg(Th::qss(Th::c().surface.highlight))
                .arg(Th::c().fonts.md)
        );

        auto *btnRow = new QHBoxLayout;
        btnRow->setSpacing(8);
        btnRow->addStretch();
        btnRow->addWidget(cancelBtn);
        btnRow->addWidget(insertBtn);

        lay->addWidget(new QLabel(t.urlLabel, this));
        lay->addWidget(_urlEdit);
        lay->addWidget(new QLabel(t.displayLabel, this));
        lay->addWidget(_textEdit);
        lay->addLayout(btnRow);

        connect(cancelBtn, &QPushButton::clicked, this, &LinkPopup::close);
        connect(insertBtn, &QPushButton::clicked, this, [this] { tryInsert(); });
        connect(
            _urlEdit, &QLineEdit::returnPressed, _textEdit, QOverload<>::of(&QLineEdit::setFocus)
        );
        connect(_textEdit, &QLineEdit::returnPressed, this, [this] { tryInsert(); });
    }

    void open(const QPoint &belowLeft, const QString &selectedText, Callback cb) {
        _urlEdit->clear();
        _textEdit->setText(selectedText);
        _cb = std::move(cb);
        adjustSize();
        move(belowLeft);
        show();
        raise();
        _urlEdit->setFocus();
    }

private:
    void tryInsert() {
        const QString url = _urlEdit->text().trimmed();
        if (url.isEmpty()) {
            _urlEdit->setFocus();
            return;
        }
        const QString label = _textEdit->text().trimmed();
        close();
        if (_cb)
            _cb(url, label);
    }

    QLineEdit *_urlEdit  = nullptr;
    QLineEdit *_textEdit = nullptr;
    Callback   _cb;
};

// ── SchedulePopup ─────────────────────────────────────────────────────────────
// Small popup that lets the user pick a date/time for scheduled send.

class SchedulePopup : public QWidget {
public:
    using Callback = std::function<void(qint64 unixTs)>;

    SchedulePopup(
        QWidget       *parent,
        const QString &sendAtLabel,
        const QString &cancelLabel,
        const QString &confirmLabel
    )
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setObjectName("schedulePopup");
        setStyleSheet(
            QString(
                "QWidget#schedulePopup {"
                "  background:%1; border:1px solid %2; border-radius:8px;"
                "}"
                "QLabel  { font-size:%6px; color:%3; border:none; background:transparent; }"
                "QDateTimeEdit {"
                "  border:1px solid %2; border-radius:4px;"
                "  padding:4px 8px; font-size:%7px; color:%4; background:%1;"
                "}"
                "QDateTimeEdit:focus { border-color:%5; }"
                "QDateTimeEdit::up-button, QDateTimeEdit::down-button {"
                "  width:14px;"
                "}"
            )
                .arg(Th::qss(Th::c().surface.raised))
                .arg(Th::qss(Th::c().divider.strong))
                .arg(Th::qss(Th::c().text.secondary))
                .arg(Th::qss(Th::c().text.primary))
                .arg(Th::qss(Th::c().accent.def))
                .arg(Th::c().fonts.caption)
                .arg(Th::c().fonts.md)
        );

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(12, 12, 12, 12);
        lay->setSpacing(8);

        lay->addWidget(new QLabel(sendAtLabel, this));

        // Use a simple QDateTimeEdit
        _dt = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600), this);
        _dt->setDisplayFormat("MMM d, yyyy h:mm AP");
        _dt->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
        _dt->setCalendarPopup(true);
        _dt->setMinimumWidth(240);
        lay->addWidget(_dt);

        auto *cancelBtn  = new QPushButton(cancelLabel, this);
        auto *confirmBtn = new QPushButton(confirmLabel, this);
        cancelBtn->setCursor(Qt::PointingHandCursor);
        confirmBtn->setCursor(Qt::PointingHandCursor);
        cancelBtn->setStyleSheet(
            QString(
                "QPushButton { background:transparent; color:%1; border:1px solid %2;"
                "  border-radius:4px; padding:4px 14px; font-size:%4px; }"
                "QPushButton:hover { background:%3; }"
            )
                .arg(Th::qss(Th::c().text.secondary))
                .arg(Th::qss(Th::c().divider.strong))
                .arg(Th::qss(Th::c().surface.highlight))
                .arg(Th::c().fonts.md)
        );
        confirmBtn->setStyleSheet(
            QString(
                "QPushButton { background:%1; color:white; border:none;"
                "  border-radius:4px; padding:4px 14px; font-size:%4px; font-weight:600; }"
                "QPushButton:hover   { background:%2; }"
                "QPushButton:pressed { background:%3; }"
            )
                .arg(Th::qss(Th::c().accent.def))
                .arg(Th::qss(Th::c().accent.hover))
                .arg(Th::qss(Th::c().accent.pressed))
                .arg(Th::c().fonts.md)
        );

        auto *btnRow = new QHBoxLayout;
        btnRow->addStretch();
        btnRow->addWidget(cancelBtn);
        btnRow->addWidget(confirmBtn);
        lay->addLayout(btnRow);

        connect(cancelBtn, &QPushButton::clicked, this, &SchedulePopup::close);
        connect(confirmBtn, &QPushButton::clicked, this, [this] {
            const qint64 ts = _dt->dateTime().toSecsSinceEpoch();
            close();
            if (_cb)
                _cb(ts);
        });
    }

    void open(const QPoint &pos, Callback cb) {
        _cb = std::move(cb);
        _dt->setDateTime(QDateTime::currentDateTime().addSecs(3600));
        adjustSize();
        move(pos);
        show();
        raise();
        _dt->setFocus();
    }

private:
    QDateTimeEdit *_dt = nullptr;
    Callback       _cb;
};

// ── Constructor ───────────────────────────────────────────────────────────────

ComposerWidget::ComposerWidget(QWidget *parent) : QWidget(parent) {
    setObjectName("composerWidget");
    _tooltip = new PopupTooltip(this);

    // Typing debounce timer: 3 s of silence before re-arming
    _typingTimer.setSingleShot(true);
    _typingTimer.setInterval(3000);
    connect(&_typingTimer, &QTimer::timeout, this, [this] {
        _typingPending = true; // re-arm: next keypress will emit again
    });

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(12, 8, 12, 8);
    outerLayout->setSpacing(0);

    _box = new QFrame(this);
    _box->setObjectName("composerBox");
    auto *boxLayout = new QVBoxLayout(_box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(0);

    // ── Formatting toolbar ────────────────────────────────────────────────────
    _formattingTb = new FormattingToolbar(_box);
    boxLayout->addWidget(_formattingTb);

    connect(_formattingTb, &FormattingToolbar::boldClicked, this, [this] {
        applyInlineFormat("*");
    });
    connect(_formattingTb, &FormattingToolbar::italicClicked, this, [this] {
        applyInlineFormat("_");
    });
    connect(_formattingTb, &FormattingToolbar::underlineClicked, this, [this] {
        applyInlineFormat("__");
    });
    connect(_formattingTb, &FormattingToolbar::strikeClicked, this, [this] {
        applyInlineFormat("~");
    });
    connect(_formattingTb, &FormattingToolbar::inlineCodeClicked, this, [this] {
        applyInlineFormat("`");
    });
    connect(_formattingTb, &FormattingToolbar::codeBlockClicked, this, [this] {
        applyBlockFormat("```");
    });
    connect(_formattingTb, &FormattingToolbar::orderedListClicked, this, [this] {
        prefixSelectedLines("", true);
    });
    connect(_formattingTb, &FormattingToolbar::bulletListClicked, this, [this] {
        prefixSelectedLines("- ");
    });
    connect(_formattingTb, &FormattingToolbar::blockquoteClicked, this, [this] {
        prefixSelectedLines("> ");
    });
    connect(_formattingTb, &FormattingToolbar::linkClicked, this, &ComposerWidget::openLinkDialog);

    // ── Edit-mode banner ──────────────────────────────────────────────────────
    _editBanner = new EditModeBanner(_box);
    connect(_editBanner, &EditModeBanner::cancelClicked, this, &ComposerWidget::exitEditMode);
    boxLayout->addWidget(_editBanner); // hidden by default; shown in enterEditMode

    // ── File attachment strip ─────────────────────────────────────────────────
    _attachStrip = new AttachmentStrip(_box);
    connect(_attachStrip, &AttachmentStrip::removeRequested, this, [this](const QString &path) {
        _pendingFiles.removeAll(path);
        _attachStrip->rebuild(_pendingFiles, _editModeFiles);
    });
    boxLayout->addWidget(_attachStrip);

    // ── Text input ────────────────────────────────────────────────────────────
    _edit = new QTextEdit(_box);
    _edit->setObjectName("composerEdit");
    _edit->setPlaceholderText(tr("Message #channel"));
    _edit->setMinimumHeight(kMinEditHeight);
    _edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _edit->setAcceptRichText(false);
    _edit->setFrameShape(QFrame::NoFrame);
    _edit->setAcceptDrops(false); // we handle drops via event filter
    _edit->installEventFilter(this);
    setAcceptDrops(true); // drops on the whole composer widget
    connect(_edit, &QTextEdit::textChanged, this, &ComposerWidget::updateSendState);
    connect(
        _edit->document()->documentLayout(),
        &QAbstractTextDocumentLayout::documentSizeChanged,
        this,
        &ComposerWidget::adjustEditorHeight
    );

    boxLayout->addWidget(_edit, 1);

    // ── Bottom action bar ─────────────────────────────────────────────────────
    _bottomBar = new QWidget(_box);
    _bottomBar->setFixedHeight(36);
    _bottomBar->setStyleSheet("QWidget { background: transparent; }");
    auto *bbLayout = new QHBoxLayout(_bottomBar);
    bbLayout->setContentsMargins(8, 3, 4, 3);
    bbLayout->setSpacing(4);

    auto registerTip = [&](QWidget *btn, const QString &text) {
        btn->setAttribute(Qt::WA_Hover);
        btn->installEventFilter(this);
        _tooltipBtns[btn] = text;
    };

    static constexpr QSize kAttachIconSize{19, 19};
    auto                  *attachBtn = new QToolButton(_bottomBar);
    attachBtn->setFixedSize(26, 26);
    attachBtn->setIconSize(kAttachIconSize);
    attachBtn->setIcon(
        svgIcon(":/ui/paperclip.svg", kAttachIconSize, Th::c().composer.toolbarIcon)
    );
    attachBtn->setCursor(Qt::PointingHandCursor);
    attachBtn->setFocusPolicy(Qt::NoFocus);
    _iconBtns.append({attachBtn, ":/ui/paperclip.svg"});
    registerTip(attachBtn, tip(tr("Attach file"), "Ctrl+O"));
    connect(attachBtn, &QToolButton::clicked, this, &ComposerWidget::openAttachDialog);

    auto makeBbBtn = [&](const QString &svgPath, const QString &tooltipText) {
        auto *btn = new QToolButton(_bottomBar);
        btn->setFixedSize(26, 26);
        btn->setIconSize(kToolIconSize);
        btn->setIcon(svgIcon(svgPath, kToolIconSize, Th::c().composer.toolbarIcon));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        _iconBtns.append({btn, svgPath});
        registerTip(btn, tooltipText);
        return btn;
    };

    auto *emojiBtn   = makeBbBtn(":/ui/smile.svg", tip(tr("Emoji"), "Ctrl+Shift+\\"));
    auto *mentionBtn = makeBbBtn(":/ui/at-sign.svg", tip(tr("Mention"), "@"));

    bbLayout->addWidget(attachBtn);
    bbLayout->addWidget(emojiBtn);
    bbLayout->addWidget(mentionBtn);
    bbLayout->addStretch();

    _sendBtn = new QPushButton(_bottomBar);
    _sendBtn->setFixedSize(38, 28);
    _sendBtn->setIconSize(QSize(18, 18));
    _sendBtn->setCursor(Qt::PointingHandCursor);
    _sendBtn->setFocusPolicy(Qt::NoFocus);
    registerTip(_sendBtn, tip(tr("Send message"), "Enter"));

    // Schedule-send dropdown (chevron beside send button)
    _dropBtn = new QPushButton(_bottomBar);
    _dropBtn->setFixedSize(18, 28);
    _dropBtn->setIconSize(QSize(12, 12));
    _dropBtn->setIcon(svgIcon(":/ui/chevron-down.svg", QSize(12, 12), Th::c().composer.dropArrow));
    _dropBtn->setCursor(Qt::PointingHandCursor);
    _dropBtn->setFocusPolicy(Qt::NoFocus);
    registerTip(_dropBtn, tr("Schedule send"));
    connect(_dropBtn, &QPushButton::clicked, this, &ComposerWidget::trySchedule);

    // Group send + drop; the group provides the unified pill background/shape.
    // Buttons are fully transparent so they never create a visible seam.
    _sendGroup     = new QWidget(_bottomBar);
    auto *sgLayout = new QHBoxLayout(_sendGroup);
    sgLayout->setContentsMargins(0, 0, 0, 0);
    sgLayout->setSpacing(0);
    sgLayout->addWidget(_sendBtn);
    sgLayout->addWidget(_dropBtn);
    bbLayout->addWidget(_sendGroup);

    boxLayout->addWidget(_bottomBar);
    outerLayout->addWidget(_box);

    // ── Bottom bar actions ────────────────────────────────────────────────────
    connect(_sendBtn, &QPushButton::clicked, this, &ComposerWidget::trySend);

    connect(emojiBtn, &QToolButton::clicked, this, [this, emojiBtn] {
        if (!_emojiPicker) {
            _emojiPicker = new EmojiPickerPopup(this);
            connect(
                _emojiPicker, &EmojiPickerPopup::emojiSelected, this, [this](const QString &name) {
                    auto cursor = _edit->textCursor();
                    cursor.insertText(":" + name + ":");
                    _edit->setFocus();
                }
            );
        }
        if (_session)
            _emojiPicker->setSession(_session);
        const QPoint pos = emojiBtn->mapToGlobal(QPoint(0, -_emojiPicker->sizeHint().height() - 4));
        _emojiPicker->open(pos);
    });

    connect(mentionBtn, &QToolButton::clicked, this, [this] {
        auto cursor = _edit->textCursor();
        cursor.insertText("@");
        _edit->setTextCursor(cursor);
        _edit->setFocus();
        QTimer::singleShot(0, this, &ComposerWidget::checkMentionPopup);
    });

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });

    setFocused(false);
    updateSendState();
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void ComposerWidget::applyTheme() {
    _edit->setStyleSheet(QString(
                             "QTextEdit {"
                             "  border: none;"
                             "  padding: 6px 10px;"
                             "  font-size: %2px;"
                             "  color: %1;"
                             "  background: transparent;"
                             "}"
    )
                             .arg(Th::qss(Th::c().text.primary))
                             .arg(Th::c().fonts.base));

    // Re-apply bottom-bar tool button styles
    const QString bbToolBtnStyle =
        QString(
            "QToolButton { border: none; border-radius: 3px; background: transparent; }"
            "QToolButton:hover   { background: %1; }"
            "QToolButton:pressed { background: %2; }"
        )
            .arg(Th::qss(Th::c().divider.def), Th::qss(Th::c().surface.highlightStrong));
    const auto toolBtns = _bottomBar->findChildren<QToolButton *>();
    for (auto *btn : toolBtns)
        btn->setStyleSheet(bbToolBtnStyle);
}

// ── Public ────────────────────────────────────────────────────────────────────

void ComposerWidget::setPlaceholderText(const QString &text) {
    _edit->setPlaceholderText(text);
}

void ComposerWidget::setSession(Session *session) {
    _session = session;
    if (_emojiPicker)
        _emojiPicker->setSession(session);
    if (_mentionPopup)
        _mentionPopup->setSession(session);
}

void ComposerWidget::setConvKind(ConvKind kind) {
    _convKind = kind;
    if (_mentionPopup)
        _mentionPopup->dismiss();
}

void ComposerWidget::checkMentionPopup() {
    const QString text = _edit->toPlainText();
    const int     cur  = _edit->textCursor().position();

    auto dismiss = [this] {
        if (_mentionPopup)
            _mentionPopup->dismiss();
    };

    if (cur <= 0) {
        dismiss();
        return;
    }

    // Scan back from cursor: stop at whitespace or '@'
    int atPos = cur - 1;
    while (atPos > 0 && !text[atPos].isSpace() && text[atPos] != '@')
        --atPos;

    if (text[atPos] != '@') {
        dismiss();
        return;
    }

    // '@' must be at the start of text or preceded by whitespace (not an email address)
    if (atPos > 0 && !text[atPos - 1].isSpace()) {
        dismiss();
        return;
    }

    const QString query = text.mid(atPos + 1, cur - atPos - 1);
    if (query.contains(' ')) {
        dismiss();
        return;
    }

    if (!_session)
        return;

    if (!_mentionPopup) {
        // Parent = msgArea (our parent widget), so the popup overlays the
        // message list without being a separate window — no focus events.
        _mentionPopup = new MentionPopup(parentWidget());
        _mentionPopup->setSession(_session);
        connect(_mentionPopup, &QObject::destroyed, this, [this] { _mentionPopup = nullptr; });
        connect(_mentionPopup, &MentionPopup::selected, this, [this](const QString &insert) {
            const int cur2 = _edit->textCursor().position();
            auto      tc   = _edit->textCursor();
            tc.setPosition(_atTriggerStart);
            tc.setPosition(cur2, QTextCursor::KeepAnchor);
            tc.insertText(insert + " ");
            _edit->setFocus();
        });
    }

    _atTriggerStart = atPos;

    const bool   isDm   = (_convKind == ConvKind::Im || _convKind == ConvKind::Mpim);
    const QPoint anchor = _edit->mapToGlobal(_edit->cursorRect().bottomLeft());
    _mentionPopup->open(anchor, query, isDm);
}

void ComposerWidget::addPendingFile(const QString &filePath) {
    if (!_pendingFiles.contains(filePath))
        _pendingFiles.append(filePath);
    _attachStrip->rebuild(_pendingFiles, _editModeFiles);
}

void ComposerWidget::clearPendingFiles() {
    _pendingFiles.clear();
    _editModeFiles.clear();
    _attachStrip->rebuild(_pendingFiles, _editModeFiles);
}

void ComposerWidget::recolorBottomBarIcons(const QColor &color) {
    for (auto &[btn, path] : _iconBtns)
        btn->setIcon(svgIcon(path, btn->iconSize(), color));
}

void ComposerWidget::setFocused(bool focused) {
    const QColor iconColor =
        focused ? Th::c().composer.toolbarIconActive : Th::c().composer.toolbarIcon;
    recolorBottomBarIcons(iconColor);
    _formattingTb->recolor(iconColor);
    _box->setStyleSheet(
        QString(
            "QFrame#composerBox {"
            "  border: 1px solid %1;"
            "  border-radius: 8px;"
            "  background: %2;"
            "}"
        )
            .arg(Th::qss(focused ? Th::c().composer.borderFocus : Th::c().composer.border))
            .arg(Th::qss(Th::c().surface.raised))
    );

    // Update schedule-send dropdown icon color
    const QColor dropColor =
        _edit->toPlainText().trimmed().isEmpty() ? Th::c().composer.dropArrow : Qt::white;
    _dropBtn->setIcon(svgIcon(":/ui/chevron-down.svg", QSize(12, 12), dropColor));
}

void ComposerWidget::adjustEditorHeight() {
    const int docH   = qCeil(_edit->document()->size().height());
    const int pad    = 12;
    const int needed = qMax(kMinEditHeight, docH + pad);
    const int maxH   = window() ? window()->height() / 2 : 300;
    _edit->setFixedHeight(qMin(needed, maxH));
}

void ComposerWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    adjustEditorHeight();
}

void ComposerWidget::updateSendState() {
    const bool active = !_edit->toPlainText().trimmed().isEmpty() || !_pendingFiles.isEmpty();

    _sendBtn->setIcon(
        svgIcon(":/ui/send.svg", QSize(18, 18), active ? Qt::white : Th::c().composer.dropArrow)
    );

    const QColor dropColor = active ? Qt::white : Th::c().composer.dropArrow;
    _dropBtn->setIcon(svgIcon(":/ui/chevron-down.svg", QSize(12, 12), dropColor));

    if (active) {
        // Group paints the unified green pill; buttons are transparent windows into it.
        _sendGroup->setStyleSheet(
            QString("background:%1; border-radius:4px;").arg(Th::qss(Th::c().accent.def))
        );
        _sendBtn->setStyleSheet(
            "QPushButton { background:transparent; border:none; margin:0; padding:0; }"
            "QPushButton:hover   { background:rgba(255,255,255,40); }"
            "QPushButton:pressed { background:rgba(0,0,0,40); }"
        );
        _dropBtn->setStyleSheet(
            "QPushButton { background:transparent; border:none; margin:0; padding:0;"
            "  border-left:1px solid rgba(0,0,0,60); }"
            "QPushButton:hover   { background:rgba(255,255,255,40); }"
            "QPushButton:pressed { background:rgba(0,0,0,40); }"
        );
    } else {
        _sendGroup->setStyleSheet("background:transparent;");
        _sendBtn->setStyleSheet(
            QString(
                "QPushButton { background:transparent; border:none; margin:0; padding:0;"
                "  border-top-left-radius:4px; border-bottom-left-radius:4px; }"
                "QPushButton:hover { background:%1; }"
            )
                .arg(Th::qss(Th::c().surface.highlight))
        );
        _dropBtn->setStyleSheet(
            QString(
                "QPushButton { background:transparent; border:none; margin:0; padding:0;"
                "  border-top-right-radius:4px; border-bottom-right-radius:4px; }"
                "QPushButton:hover { background:%1; }"
            )
                .arg(Th::qss(Th::c().surface.highlight))
        );
    }
}

// ── Event filter ──────────────────────────────────────────────────────────────

bool ComposerWidget::eventFilter(QObject *obj, QEvent *event) {
    // ── Drag-and-drop on the whole composer ───────────────────────────────────
    const auto t = event->type();
    if (t == QEvent::DragEnter || t == QEvent::DragMove) {
        auto *de = static_cast<QDragMoveEvent *>(event);
        if (de->mimeData()->hasUrls()) {
            de->acceptProposedAction();
            return true;
        }
    }
    if (t == QEvent::Drop) {
        auto *de = static_cast<QDropEvent *>(event);
        if (de->mimeData()->hasUrls()) {
            de->acceptProposedAction();
            for (const QUrl &url : de->mimeData()->urls()) {
                if (url.isLocalFile())
                    addPendingFile(url.toLocalFile());
            }
            return true;
        }
    }

    // ── Editor events ─────────────────────────────────────────────────────────
    if (obj == _edit) {
        if (t == QEvent::FocusIn) {
            setFocused(true);
        } else if (t == QEvent::FocusOut) {
            setFocused(false);
            // _mentionPopup is a plain child widget (no separate window) so it
            // never steals focus — don't dismiss it here; checkMentionPopup()
            // handles its lifetime via text/cursor state.
            if (_mentionComp)
                _mentionComp->dismiss();
        } else if (t == QEvent::KeyPress) {
            auto      *ke  = static_cast<QKeyEvent *>(event);
            const auto mod = ke->modifiers();
            const int  key = ke->key();

            // Mention popup intercepts navigation keys first
            if (_mentionPopup && _mentionPopup->isOpen()) {
                if (_mentionPopup->handleKey(key))
                    return true;
            }
            if (_mentionComp && _mentionComp->isVisible()) {
                if (_mentionComp->handleKey(key))
                    return true;
            }

            if (key == Qt::Key_Return && !(mod & Qt::ShiftModifier)) {
                trySend();
                return true;
            }

            if (key == Qt::Key_Up && mod == Qt::NoModifier && _edit->toPlainText().isEmpty()) {
                emit editLastRequested();
                return true;
            }

            if (key == Qt::Key_Escape && !_editingTs.isEmpty()) {
                exitEditMode();
                return true;
            }

            // Typing indicator: fire at most once per 3 s
            if (!ke->text().isEmpty()) {
                if (_typingPending) {
                    _typingPending = false;
                    emit typingStarted();
                    _typingTimer.start();
                } else if (!_typingTimer.isActive()) {
                    // First keypress ever in this "session"
                    _typingPending = false;
                    emit typingStarted();
                    _typingTimer.start();
                }
            }

#ifdef Q_OS_MAC
            static const Qt::KeyboardModifiers kCmd{Qt::MetaModifier};
#else
            static const Qt::KeyboardModifiers kCmd{Qt::ControlModifier};
#endif
            const auto relevantMod = mod & (Qt::ControlModifier | Qt::ShiftModifier |
                                            Qt::AltModifier | Qt::MetaModifier);

            if (relevantMod == kCmd) {
                switch (key) {
                case Qt::Key_B:
                    applyInlineFormat("*");
                    return true;
                case Qt::Key_I:
                    applyInlineFormat("_");
                    return true;
                case Qt::Key_U:
                    applyInlineFormat("__");
                    return true;
                case Qt::Key_O:
                    openAttachDialog();
                    return true;
                default:
                    break;
                }
            }

            if (relevantMod == (kCmd | Qt::ShiftModifier)) {
                switch (key) {
                case Qt::Key_X:
                    applyInlineFormat("~");
                    return true;
                case Qt::Key_C:
                    applyInlineFormat("`");
                    return true;
                case Qt::Key_7:
                    prefixSelectedLines("", true);
                    return true;
                case Qt::Key_8:
                    prefixSelectedLines("- ");
                    return true;
                case Qt::Key_9:
                    prefixSelectedLines("> ");
                    return true;
                case Qt::Key_U: {
                    const QPoint p = _formattingTb->mapToGlobal(
                        QPoint(_formattingTb->width() / 4, _formattingTb->height() + 4)
                    );
                    openLinkDialog(p);
                    return true;
                }
                case Qt::Key_Backslash: {
                    if (!_emojiPicker) {
                        _emojiPicker = new EmojiPickerPopup(this);
                        connect(
                            _emojiPicker,
                            &EmojiPickerPopup::emojiSelected,
                            this,
                            [this](const QString &name) {
                                auto cursor = _edit->textCursor();
                                cursor.insertText(":" + name + ":");
                                _edit->setFocus();
                            }
                        );
                    }
                    if (_session)
                        _emojiPicker->setSession(_session);
                    const QRect  cursorRect = _edit->cursorRect();
                    const QPoint pos = _edit->mapToGlobal(cursorRect.topLeft()) - QPoint(0, 320);
                    _emojiPicker->open(pos);
                    return true;
                }
                default:
                    break;
                }
            }

            if (relevantMod == (kCmd | Qt::AltModifier | Qt::ShiftModifier)) {
                if (key == Qt::Key_C) {
                    applyBlockFormat("```");
                    return true;
                }
            }
        } else if (t == QEvent::KeyRelease) {
            auto      *ke        = static_cast<QKeyEvent *>(event);
            const int  key       = ke->key();
            // Skip modifiers and navigation/action keys that don't insert text.
            // Especially important: Up/Down/Escape/Return are consumed by the
            // popup on KeyPress, but their KeyRelease still fires — if we let it
            // trigger checkMentionPopup() the popup gets rebuilt/reopened.
            const bool isNonText = key == Qt::Key_Control || key == Qt::Key_Shift ||
                                   key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_Up ||
                                   key == Qt::Key_Down || key == Qt::Key_Left ||
                                   key == Qt::Key_Right || key == Qt::Key_Escape ||
                                   key == Qt::Key_Return || key == Qt::Key_Tab;
            if (!isNonText) {
                QTimer::singleShot(0, this, [this] {
                    checkMentionPopup(); // handles @ trigger

                    // ── # channel and :emoji: autocomplete ────────────────────
                    const QString text   = _edit->toPlainText();
                    const int     cursor = _edit->textCursor().position();
                    if (cursor <= 0) {
                        if (_mentionComp)
                            _mentionComp->dismiss();
                        return;
                    }

                    int trigStart = cursor - 1;
                    while (trigStart > 0 && !text[trigStart - 1].isSpace() &&
                           text[trigStart - 1] != '#' && text[trigStart - 1] != ':')
                        --trigStart;

                    if (trigStart < 0 || trigStart >= text.length()) {
                        if (_mentionComp)
                            _mentionComp->dismiss();
                        return;
                    }

                    const QChar   trigger = text[trigStart];
                    const QString query   = text.mid(trigStart + 1, cursor - trigStart - 1);

                    if (query.isEmpty() || query.contains(' ') ||
                        (trigger != '#' && trigger != ':')) {
                        if (_mentionComp)
                            _mentionComp->dismiss();
                        return;
                    }

                    if (!_session)
                        return;

                    QList<MentionCompleter::Item> items;
                    if (trigger == '#') {
                        const auto &convs = _session->currentConversations();
                        for (const auto &c : convs) {
                            if (c.kind != ConvKind::PublicChannel &&
                                c.kind != ConvKind::PrivateChannel)
                                continue;
                            if (c.name.contains(query, Qt::CaseInsensitive)) {
                                items.append(
                                    {"#" + c.name, "<#" + c.id.value + "|" + c.name + ">"}
                                );
                                if (items.size() >= 8)
                                    break;
                            }
                        }
                    } else { // ':'
                        static const QStringList kCommonEmoji{
                            "thumbsup", "thumbsdown", "clap",        "heart",    "fire",
                            "rocket",   "eyes",       "smile",       "laughing", "wink",
                            "grin",     "joy",        "sweat_smile", "sob",      "thinking_face",
                            "wave",     "ok_hand",    "point_right", "muscle",   "100"
                        };
                        for (const QString &name : kCommonEmoji) {
                            if (name.startsWith(query, Qt::CaseInsensitive))
                                items.append({":" + name + ":", ":" + name + ":"});
                        }
                        const auto &emap = _session->emojiMap();
                        for (auto it = emap.begin(); it != emap.end() && items.size() < 8; ++it) {
                            if (it.key().startsWith(query, Qt::CaseInsensitive))
                                items.append({":" + it.key() + ":", ":" + it.key() + ":"});
                        }
                        items = items.mid(0, 8);
                    }

                    if (items.isEmpty()) {
                        if (_mentionComp)
                            _mentionComp->dismiss();
                        return;
                    }

                    if (!_mentionComp) {
                        _mentionComp = new MentionCompleter(this);
                        _mentionComp->setWindowFlags(
                            Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                        );
                    }

                    const QRect  curRect   = _edit->cursorRect();
                    const QPoint globalCur = _edit->mapToGlobal(curRect.bottomLeft());
                    _mentionComp->show(
                        globalCur + QPoint(0, 4),
                        items,
                        [this, trigStart, cursor](const QString &insert) {
                            auto tc = _edit->textCursor();
                            tc.setPosition(trigStart);
                            tc.setPosition(cursor, QTextCursor::KeepAnchor);
                            tc.insertText(insert + " ");
                            _edit->setFocus();
                        }
                    );
                });
            }
        }
    }

    // ── PopupTooltip hover ────────────────────────────────────────────────────
    if (auto *w = qobject_cast<QWidget *>(obj); w && _tooltipBtns.contains(w)) {
        if (event->type() == QEvent::HoverEnter) {
            _tooltip->showAbove(_tooltipBtns[w], QRect(w->mapToGlobal(QPoint(0, 0)), w->size()));
        } else if (event->type() == QEvent::HoverLeave) {
            _tooltip->hide();
        }
    }

    return QWidget::eventFilter(obj, event);
}

// ── Sending ───────────────────────────────────────────────────────────────────

void ComposerWidget::trySend() {
    _tooltip->hide();
    const auto text     = _edit->toPlainText().trimmed();
    const bool hasFiles = !_pendingFiles.isEmpty();

    if (text.isEmpty() && !hasFiles)
        return;

    // Upload pending files
    for (const QString &f : std::as_const(_pendingFiles))
        emit uploadRequested(f);
    _pendingFiles.clear();
    _attachStrip->rebuild(_pendingFiles, _editModeFiles);

    if (!_editingTs.isEmpty()) {
        const Ts ts = _editingTs;
        exitEditMode();
        if (!text.isEmpty())
            emit editRequested(ts, text);
    } else {
        _edit->clear();
        if (!text.isEmpty())
            emit sendRequested(text);
    }

    _typingTimer.stop();
    _typingPending = true;
}

void ComposerWidget::trySchedule() {
    const auto text = _edit->toPlainText().trimmed();
    if (text.isEmpty() && _pendingFiles.isEmpty())
        return;

    static SchedulePopup *popup = nullptr;
    if (!popup) {
        popup = new SchedulePopup(this, tr("Send at"), tr("Cancel"), tr("Schedule"));
    }

    const QPoint pos = _dropBtn->mapToGlobal(QPoint(0, -popup->sizeHint().height() - 4));

    popup->open(pos, [this, text](qint64 unixTs) {
        _edit->clear();
        _pendingFiles.clear();
        _attachStrip->rebuild(_pendingFiles, _editModeFiles);
        emit scheduleRequested(text, unixTs);
    });
}

void ComposerWidget::applyInlineFormat(const QString &marker) {
    auto cursor = _edit->textCursor();
    if (cursor.hasSelection()) {
        const QString sel = cursor.selectedText();
        cursor.insertText(marker + sel + marker);
    } else {
        const int pos = cursor.position();
        cursor.insertText(marker + marker);
        cursor.setPosition(pos + marker.length());
        _edit->setTextCursor(cursor);
    }
    _edit->setFocus();
}

void ComposerWidget::prefixSelectedLines(const QString &prefix, bool ordered) {
    auto       cursor = _edit->textCursor();
    const bool hasSel = cursor.hasSelection();
    int        start  = hasSel ? cursor.selectionStart() : cursor.position();
    int        end    = hasSel ? cursor.selectionEnd() : cursor.position();

    cursor.setPosition(start);
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.beginEditBlock();
    int lineIdx = 0;
    while (true) {
        const QString pfx = ordered ? (QString::number(lineIdx + 1) + ". ") : prefix;
        cursor.insertText(pfx);
        end += pfx.size();
        ++lineIdx;
        if (!hasSel)
            break;
        if (!cursor.movePosition(QTextCursor::NextBlock))
            break;
        if (cursor.position() > end)
            break;
    }
    cursor.endEditBlock();
    _edit->setFocus();
}

void ComposerWidget::applyBlockFormat(const QString &fence) {
    auto cursor = _edit->textCursor();
    if (cursor.hasSelection()) {
        const QString sel = cursor.selectedText().replace(QChar(0x2029), '\n').trimmed();
        cursor.insertText(fence + "\n" + sel + "\n" + fence);
    } else {
        const int pos = cursor.position();
        cursor.insertText(fence + "\n\n" + fence);
        cursor.setPosition(pos + fence.length() + 1);
        _edit->setTextCursor(cursor);
    }
    _edit->setFocus();
}

// ── Edit mode ─────────────────────────────────────────────────────────────────

void ComposerWidget::enterEditMode(
    const Ts &ts, const QString &existingText, const std::vector<File> &existingFiles
) {
    if (!_editingTs.isEmpty())
        exitEditMode();
    _editingTs = ts;

    _edit->setPlainText(existingText);
    auto cursor = _edit->textCursor();
    cursor.movePosition(QTextCursor::End);
    _edit->setTextCursor(cursor);

    _editBanner->setVisible(true);

    _editModeFiles = existingFiles;
    _attachStrip->rebuild(_pendingFiles, _editModeFiles);

    _edit->setFocus();
}

void ComposerWidget::exitEditMode() {
    if (_editingTs.isEmpty())
        return;
    _editingTs.clear();
    _edit->clear();

    _editBanner->setVisible(false);

    _editModeFiles.clear();
    _attachStrip->rebuild(_pendingFiles, _editModeFiles);
}

// ── Draft support ─────────────────────────────────────────────────────────────

QString ComposerWidget::currentText() const {
    return _edit->toPlainText();
}

void ComposerWidget::setText(const QString &text) {
    _edit->setPlainText(text);
    if (!text.isEmpty()) {
        auto cursor = _edit->textCursor();
        cursor.movePosition(QTextCursor::End);
        _edit->setTextCursor(cursor);
    }
}

// ── Dialogs ───────────────────────────────────────────────────────────────────

void ComposerWidget::openAttachDialog() {
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Attach File"));
    for (const QString &p : paths)
        addPendingFile(p);
}

void ComposerWidget::openLinkDialog(const QPoint &pos) {
    if (!_linkPopup) {
        LinkPopupTexts t{tr("URL"), tr("Display text"), tr("Insert"), tr("Cancel")};
        _linkPopup = new LinkPopup(this, t);
    }

    const auto    savedCursor = _edit->textCursor();
    const QString sel         = savedCursor.selectedText().replace(QChar(0x2029), '\n');

    static_cast<LinkPopup *>(_linkPopup)
        ->open(pos, sel, [this, savedCursor](const QString &url, const QString &label) {
            const QString mrkdwn =
                (label.isEmpty() || label == url) ? "<" + url + ">" : "<" + url + "|" + label + ">";
            auto cursor = savedCursor;
            cursor.insertText(mrkdwn);
            _edit->setFocus();
        });
}
