// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "composer_widget.h"
#include "ui/icon_utils.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMenu>
#include <QTextCursor>
#include <QCursor>
#include <QResizeEvent>
#include <QtMath>
#include <QAbstractTextDocumentLayout>

static constexpr int kMinEditHeight = 40;

// ── Helpers ───────────────────────────────────────────────────────────────────

// No stylesheet set here — the toolbar parent stylesheet drives all colors.
static QToolButton *makeToolBtn(const QString &svgPath, const QString &tooltip, QWidget *parent) {
    auto *btn = new QToolButton(parent);
    btn->setToolTip(tooltip);
    btn->setFixedSize(26, 24);
    btn->setIconSize(QSize(14, 14));
    btn->setIcon(svgIcon(svgPath, QSize(14, 14), QColor("#888888")));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    return btn;
}

static QFrame *makeVSep(QWidget *parent) {
    auto *sep = new QFrame(parent);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedSize(1, 16);
    sep->setStyleSheet("QFrame { color: #DDDDDD; }");
    return sep;
}

// ── Constructor ───────────────────────────────────────────────────────────────

ComposerWidget::ComposerWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("composerWidget");

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(12, 8, 12, 8);
    outerLayout->setSpacing(0);

    // Rounded bordered container — border color updated by setFocused()
    _box = new QFrame(this);
    _box->setObjectName("composerBox");
    auto *boxLayout = new QVBoxLayout(_box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(0);

    // ── Formatting toolbar ────────────────────────────────────────────────────
    _toolbar = new QWidget(_box);
    _toolbar->setObjectName("composerToolbar");
    _toolbar->setFixedHeight(32);
    auto *tbLayout = new QHBoxLayout(_toolbar);
    tbLayout->setContentsMargins(8, 3, 8, 3);
    tbLayout->setSpacing(1);

    auto *boldBtn   = makeToolBtn(":/ui/bold.svg",          "Bold (*text*)",          _toolbar);
    auto *italicBtn = makeToolBtn(":/ui/italic.svg",        "Italic (_text_)",        _toolbar);
    auto *underBtn  = makeToolBtn(":/ui/underline.svg",     "Underline",              _toolbar);
    auto *strikeBtn = makeToolBtn(":/ui/strikethrough.svg", "Strikethrough (~text~)", _toolbar);
    auto *linkBtn   = makeToolBtn(":/ui/link.svg",          "Link",                   _toolbar);
    auto *olBtn     = makeToolBtn(":/ui/list-ordered.svg",  "Ordered list",           _toolbar);
    auto *ulBtn     = makeToolBtn(":/ui/list.svg",          "Bullet list",            _toolbar);
    auto *bqBtn     = makeToolBtn(":/ui/quote.svg",         "Blockquote",             _toolbar);
    auto *codeBtn   = makeToolBtn(":/ui/code.svg",          "Inline code (`code`)",   _toolbar);
    auto *snipBtn   = makeToolBtn(":/ui/braces.svg",        "Code block",             _toolbar);

    tbLayout->addWidget(boldBtn);
    tbLayout->addWidget(italicBtn);
    tbLayout->addWidget(underBtn);
    tbLayout->addWidget(strikeBtn);
    tbLayout->addWidget(makeVSep(_toolbar));
    tbLayout->addWidget(linkBtn);
    tbLayout->addWidget(olBtn);
    tbLayout->addWidget(ulBtn);
    tbLayout->addWidget(makeVSep(_toolbar));
    tbLayout->addWidget(bqBtn);
    tbLayout->addWidget(codeBtn);
    tbLayout->addWidget(snipBtn);
    tbLayout->addStretch();

    boxLayout->addWidget(_toolbar);

    // ── Text input ────────────────────────────────────────────────────────────
    _edit = new QTextEdit(_box);
    _edit->setObjectName("composerEdit");
    _edit->setPlaceholderText("Message #channel");
    _edit->setMinimumHeight(kMinEditHeight);
    _edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _edit->setAcceptRichText(false);
    _edit->setFrameShape(QFrame::NoFrame);
    _edit->setStyleSheet(
        "QTextEdit {"
        "  border: none;"
        "  padding: 6px 10px;"
        "  font-size: 14px;"
        "  color: #1D1C1D;"
        "  background: transparent;"
        "}"
    );
    _edit->installEventFilter(this);
    connect(_edit, &QTextEdit::textChanged, this, &ComposerWidget::updateSendState);
    connect(_edit->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &ComposerWidget::adjustEditorHeight);

    boxLayout->addWidget(_edit, 1);

    // ── Bottom action bar ─────────────────────────────────────────────────────
    auto *bottomBar = new QWidget(_box);
    bottomBar->setFixedHeight(36);
    bottomBar->setStyleSheet("QWidget { background: transparent; }");
    auto *bbLayout = new QHBoxLayout(bottomBar);
    bbLayout->setContentsMargins(8, 3, 4, 3);
    bbLayout->setSpacing(4);

    auto *attachBtn = new QToolButton(bottomBar);
    attachBtn->setToolTip("Attach file");
    attachBtn->setFixedSize(26, 26);
    attachBtn->setIconSize(QSize(15, 15));
    attachBtn->setIcon(svgIcon(":/ui/paperclip.svg", QSize(15, 15), QColor("#616061")));
    attachBtn->setCursor(Qt::PointingHandCursor);
    attachBtn->setFocusPolicy(Qt::NoFocus);
    attachBtn->setStyleSheet(
        "QToolButton {"
        "  border: none;"
        "  border-radius: 4px;"
        "  background: transparent;"
        "}"
        "QToolButton:hover   { background: #E8E8E8; }"
        "QToolButton:pressed { background: #E0E0E0; }"
    );
    connect(attachBtn, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Attach File");
        if (!path.isEmpty()) emit uploadRequested(path);
    });

    auto *emojiBtn   = makeToolBtn(":/ui/smile.svg",   "Emoji",   bottomBar);
    auto *mentionBtn = makeToolBtn(":/ui/at-sign.svg", "Mention", bottomBar);
    const QString bottomBtnStyle =
        "QToolButton { border: none; border-radius: 3px; background: transparent; }"
        "QToolButton:hover { background: #E8E8E8; }";
    emojiBtn->setStyleSheet(bottomBtnStyle);
    mentionBtn->setStyleSheet(bottomBtnStyle);

    bbLayout->addWidget(attachBtn);
    bbLayout->addWidget(emojiBtn);
    bbLayout->addWidget(mentionBtn);
    bbLayout->addStretch();

    _sendBtn = new QPushButton(bottomBar);
    _sendBtn->setFixedSize(32, 26);
    _sendBtn->setIconSize(QSize(14, 14));
    _sendBtn->setToolTip("Send message (Enter)");
    _sendBtn->setCursor(Qt::PointingHandCursor);
    _sendBtn->setFocusPolicy(Qt::NoFocus);

    _dropBtn = new QPushButton(bottomBar);
    _dropBtn->setFixedSize(18, 26);
    _dropBtn->setIconSize(QSize(10, 10));
    _dropBtn->setToolTip("Schedule send");
    _dropBtn->setCursor(Qt::PointingHandCursor);
    _dropBtn->setFocusPolicy(Qt::NoFocus);

    bbLayout->addWidget(_sendBtn);
    bbLayout->addSpacing(-1);
    bbLayout->addWidget(_dropBtn);

    boxLayout->addWidget(bottomBar);
    outerLayout->addWidget(_box);

    // ── Formatting actions ────────────────────────────────────────────────────
    connect(boldBtn,   &QToolButton::clicked, this, [this] { applyInlineFormat("*"); });
    connect(italicBtn, &QToolButton::clicked, this, [this] { applyInlineFormat("_"); });
    connect(strikeBtn, &QToolButton::clicked, this, [this] { applyInlineFormat("~"); });
    connect(codeBtn,   &QToolButton::clicked, this, [this] { applyInlineFormat("`"); });

    connect(_sendBtn, &QPushButton::clicked, this, &ComposerWidget::trySend);
    connect(_dropBtn, &QPushButton::clicked, this, [this] {
        auto *menu = new QMenu(this);
        menu->addAction("Monday at 09:00");
        menu->addAction("Custom time…");
        menu->exec(QCursor::pos());
    });

    setFocused(false);
    updateSendState();
}

// ── Public ────────────────────────────────────────────────────────────────────

void ComposerWidget::setPlaceholderText(const QString &text) {
    _edit->setPlaceholderText(text);
}

// ── Private ───────────────────────────────────────────────────────────────────

void ComposerWidget::setFocused(bool focused) {
    const QString borderColor = focused ? "#999999" : "#DDDDDD";
    _box->setStyleSheet(QString(
        "QFrame#composerBox {"
        "  border: 1px solid %1;"
        "  border-radius: 8px;"
        "  background: #FFFFFF;"
        "}").arg(borderColor));

    _toolbar->setStyleSheet(
        "QWidget#composerToolbar {"
        "  background: #F5F5F5;"
        "  border-radius: 7px 7px 0 0;"
        "}"
        "QWidget#composerToolbar QToolButton {"
        "  border: none; border-radius: 3px;"
        "  background: transparent;"
        "}"
        "QWidget#composerToolbar QToolButton:hover   { background: #E0E0E0; }"
        "QWidget#composerToolbar QToolButton:pressed { background: #D0D0D0; }"
    );
}

void ComposerWidget::adjustEditorHeight() {
    const int docH   = qCeil(_edit->document()->size().height());
    const int pad    = 12; // 6px top + 6px bottom from stylesheet padding
    const int needed = qMax(kMinEditHeight, docH + pad);
    const int maxH   = window() ? window()->height() / 2 : 300;
    _edit->setFixedHeight(qMin(needed, maxH));
}

void ComposerWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    adjustEditorHeight();
}

void ComposerWidget::updateSendState() {
    const bool active = !_edit->toPlainText().trimmed().isEmpty();

    _sendBtn->setIcon(svgIcon(":/ui/send.svg", QSize(14, 14),
                               active ? Qt::white : QColor("#CCCCCC")));
    _dropBtn->setIcon(svgIcon(":/ui/chevron-down.svg", QSize(10, 10),
                               active ? Qt::white : QColor("#CCCCCC")));

    const QString sendStyle = active
        ? "QPushButton {"
          "  background: #007A5A; border: none;"
          "  border-radius: 4px 0 0 4px;"
          "}"
          "QPushButton:hover   { background: #148567; }"
          "QPushButton:pressed { background: #005E45; }"
        : "QPushButton {"
          "  background: transparent; border: none;"
          "  border-radius: 4px 0 0 4px;"
          "}"
          "QPushButton:hover { background: #F0F0F0; }";

    const QString dropStyle = active
        ? "QPushButton {"
          "  background: #007A5A; border: none;"
          "  border-left: 1px solid #148567;"
          "  border-radius: 0 4px 4px 0;"
          "}"
          "QPushButton:hover   { background: #148567; }"
          "QPushButton:pressed { background: #005E45; }"
        : "QPushButton {"
          "  background: transparent; border: none;"
          "  border-radius: 0 4px 4px 0;"
          "}"
          "QPushButton:hover { background: #F0F0F0; }";

    _sendBtn->setStyleSheet(sendStyle);
    _dropBtn->setStyleSheet(dropStyle);
}

bool ComposerWidget::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _edit) {
        if (event->type() == QEvent::FocusIn)
            setFocused(true);
        else if (event->type() == QEvent::FocusOut)
            setFocused(false);
        else if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_Return && !(ke->modifiers() & Qt::ShiftModifier)) {
                trySend();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ComposerWidget::trySend() {
    const auto text = _edit->toPlainText().trimmed();
    if (text.isEmpty()) return;
    _edit->clear();
    emit sendRequested(text);
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
