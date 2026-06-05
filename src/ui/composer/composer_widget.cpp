// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "composer_widget.h"

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
static QToolButton *makeToolBtn(const QString &label, const QString &tooltip, QWidget *parent) {
    auto *btn = new QToolButton(parent);
    btn->setText(label);
    btn->setToolTip(tooltip);
    btn->setFixedSize(26, 24);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus); // don't steal focus from the editor
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

    auto *boldBtn   = makeToolBtn("B",  "Bold (*text*)",          _toolbar);
    auto *italicBtn = makeToolBtn("I",  "Italic (_text_)",        _toolbar);
    auto *underBtn  = makeToolBtn("U",  "Underline",              _toolbar);
    auto *strikeBtn = makeToolBtn("S",  "Strikethrough (~text~)", _toolbar);
    auto *linkBtn   = makeToolBtn("⊞", "Link",                   _toolbar);
    auto *olBtn     = makeToolBtn("1.", "Ordered list",           _toolbar);
    auto *ulBtn     = makeToolBtn("•",  "Bullet list",            _toolbar);
    auto *bqBtn     = makeToolBtn("|",  "Blockquote",             _toolbar);
    auto *codeBtn   = makeToolBtn("<>", "Inline code (`code`)",   _toolbar);
    auto *snipBtn   = makeToolBtn("{}","Code block",              _toolbar);

    // Font decorations go via QFont so they survive stylesheet color updates
    { QFont f = boldBtn->font();   f.setBold(true);      boldBtn->setFont(f); }
    { QFont f = italicBtn->font(); f.setItalic(true);    italicBtn->setFont(f); }
    { QFont f = underBtn->font();  f.setUnderline(true); underBtn->setFont(f); }
    { QFont f = strikeBtn->font(); f.setStrikeOut(true); strikeBtn->setFont(f); }

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
    attachBtn->setText("+");
    attachBtn->setToolTip("Attach file");
    attachBtn->setFixedSize(26, 26);
    attachBtn->setCursor(Qt::PointingHandCursor);
    attachBtn->setFocusPolicy(Qt::NoFocus);
    attachBtn->setStyleSheet(
        "QToolButton {"
        "  border: 1px solid #CCCCCC;"
        "  border-radius: 13px;"
        "  color: #616061;"
        "  font-size: 16px; font-weight: bold;"
        "  background: transparent;"
        "}"
        "QToolButton:hover   { background: #F0F0F0; border-color: #999; }"
        "QToolButton:pressed { background: #E0E0E0; }"
    );
    connect(attachBtn, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Attach File");
        if (!path.isEmpty()) emit uploadRequested(path);
    });

    auto *emojiBtn   = makeToolBtn("☺", "Emoji",   bottomBar);
    auto *mentionBtn = makeToolBtn("@", "Mention", bottomBar);
    emojiBtn->setStyleSheet(
        "QToolButton { border: none; border-radius: 3px; color: #616061; font-size: 12px; background: transparent; }"
        "QToolButton:hover { background: #E8E8E8; color: #1D1C1D; }"
    );
    mentionBtn->setStyleSheet(emojiBtn->styleSheet());

    bbLayout->addWidget(attachBtn);
    bbLayout->addWidget(emojiBtn);
    bbLayout->addWidget(mentionBtn);
    bbLayout->addStretch();

    _sendBtn = new QPushButton(bottomBar);
    _sendBtn->setText("▶");
    _sendBtn->setFixedSize(32, 26);
    _sendBtn->setToolTip("Send message (Enter)");
    _sendBtn->setCursor(Qt::PointingHandCursor);
    _sendBtn->setFocusPolicy(Qt::NoFocus);

    _dropBtn = new QPushButton(bottomBar);
    _dropBtn->setText("∨");
    _dropBtn->setFixedSize(18, 26);
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

    const QString iconColor = focused ? "#1D1C1D" : "#888888";
    _toolbar->setStyleSheet(QString(
        "QWidget#composerToolbar {"
        "  background: #F5F5F5;"
        "  border-radius: 7px 7px 0 0;"
        "}"
        "QWidget#composerToolbar QToolButton {"
        "  border: none; border-radius: 3px;"
        "  color: %1; font-size: 12px; background: transparent;"
        "}"
        "QWidget#composerToolbar QToolButton:hover   { background: #E0E0E0; color: #1D1C1D; }"
        "QWidget#composerToolbar QToolButton:pressed { background: #D0D0D0; }"
    ).arg(iconColor));
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

    const QString sendStyle = active
        ? "QPushButton {"
          "  background: #007A5A; color: white; border: none;"
          "  border-radius: 4px 0 0 4px; font-size: 12px;"
          "}"
          "QPushButton:hover   { background: #148567; }"
          "QPushButton:pressed { background: #005E45; }"
        : "QPushButton {"
          "  background: transparent; color: #CCCCCC; border: none;"
          "  border-radius: 4px 0 0 4px; font-size: 12px;"
          "}"
          "QPushButton:hover { background: #F0F0F0; color: #999; }";

    const QString dropStyle = active
        ? "QPushButton {"
          "  background: #007A5A; color: white; border: none;"
          "  border-left: 1px solid #148567;"
          "  border-radius: 0 4px 4px 0; font-size: 9px;"
          "}"
          "QPushButton:hover   { background: #148567; }"
          "QPushButton:pressed { background: #005E45; }"
        : "QPushButton {"
          "  background: transparent; color: #CCCCCC; border: none;"
          "  border-radius: 0 4px 4px 0; font-size: 9px;"
          "}"
          "QPushButton:hover { background: #F0F0F0; color: #999; }";

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
