// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "emoji_picker_popup.h"
#include "session/session.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/emoji.h"
#include "util/emoji_font.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QFont>
#include <QIcon>
#include <QKeyEvent>
#include <QPixmap>
#include <QPointer>

#include <algorithm>

// Ordered list of emoji names for grid display — resolved via Emoji::fromName.
static const QStringList kBaseEmojiNames{
    // Faces & emotions
    "smile",
    "grin",
    "laughing",
    "joy",
    "rofl",
    "sweat_smile",
    "wink",
    "heart_eyes",
    "kissing_heart",
    "stuck_out_tongue",
    "thinking_face",
    "raised_eyebrow",
    "neutral_face",
    "expressionless",
    "zipper_mouth",
    "grimacing",
    "sob",
    "tired_face",
    "sleepy",
    "mask",
    "sunglasses",
    "nerd_face",
    "monocle_face",
    "confused",
    "worried",
    "angry",
    "rage",
    "skull",
    "ghost",
    "alien",
    "poop",
    "clown_face",
    "partying_face",
    // Hands & people
    "wave",
    "raised_hand",
    "ok_hand",
    "thumbsup",
    "thumbsdown",
    "clap",
    "pray",
    "point_right",
    "point_left",
    "point_up",
    "point_down",
    "muscle",
    "handshake",
    "writing_hand",
    "selfie",
    // Hearts & symbols
    "heart",
    "orange_heart",
    "yellow_heart",
    "green_heart",
    "blue_heart",
    "purple_heart",
    "broken_heart",
    "sparkling_heart",
    "two_hearts",
    "100",
    "tada",
    "fire",
    "star",
    "star2",
    "sparkles",
    "zap",
    "boom",
    "eyes",
    "warning",
    // Animals
    "dog",
    "cat",
    "mouse",
    "hamster",
    "rabbit",
    "fox_face",
    "bear",
    "panda_face",
    "koala",
    "tiger",
    "lion",
    "cow",
    "pig",
    "frog",
    "monkey_face",
    "chicken",
    "penguin",
    "bird",
    "hatching_chick",
    "eagle",
    "owl",
    "snake",
    "turtle",
    "lizard",
    "whale",
    "dolphin",
    "shark",
    "octopus",
    "bee",
    "butterfly",
    "palm_tree",
    "deciduous_tree",
    "evergreen_tree",
    "cactus",
    "sunflower",
    "rose",
    // Food
    "apple",
    "banana",
    "watermelon",
    "grapes",
    "strawberry",
    "pizza",
    "hamburger",
    "fries",
    "hot_dog",
    "taco",
    "burrito",
    "sushi",
    "ramen",
    "spaghetti",
    "rice",
    "bread",
    "croissant",
    "cake",
    "cupcake",
    "cookie",
    "chocolate_bar",
    "candy",
    "lollipop",
    "ice_cream",
    "coffee",
    "tea",
    "beer",
    // Travel & places
    "rocket",
    "airplane",
    "car",
    "bus",
    "train",
    "bicycle",
    "boat",
    "house",
    "office",
    "school",
    "hospital",
    "bank",
    "sunrise",
    "city_sunset",
    "night_with_stars",
    "earth_americas",
    "earth_africa",
    "earth_asia",
    // Objects & misc
    "computer",
    "desktop_computer",
    "keyboard",
    "phone",
    "telephone",
    "email",
    "memo",
    "pencil",
    "paperclip",
    "scissors",
    "lock",
    "key",
    "hammer",
    "wrench",
    "gear",
    "bulb",
    "flashlight",
    "books",
    "moneybag",
    "credit_card",
    "chart",
    "trophy",
    "medal",
    "gift",
    "balloon",
    "confetti_ball",
    "musical_note",
    "headphones",
    "microphone",
    "camera",
    "hourglass_flowing_sand",
    "clock1",
    "calendar",
    "x",
    "question",
    "exclamation",
    "bell",
    "information_source",
    "white_check_mark",
    "warning",
};

// Full popup size; height shrinks below kFullHeight to fit a short filtered list.
static constexpr int kFullWidth  = 300;
static constexpr int kFullHeight = 340;
static constexpr int kCols       = 8; // emoji per grid row

EmojiPickerPopup::EmojiPickerPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName("emojiPicker");
    setFixedWidth(kFullWidth);
    setFixedHeight(kFullHeight);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    _search = new QLineEdit(this);
    _search->setPlaceholderText(tr("Search emoji…"));
    // Fixed height so the popup's chrome is deterministic when we auto-size it
    // (QLineEdit::sizeHint doesn't reliably account for the stylesheet padding).
    {
        QFont sf = _search->font();
        sf.setPixelSize(Th::c().fonts.md);
        _search->setFont(sf);
        // 4px top/bottom padding + 1px top/bottom border from the stylesheet.
        _search->setFixedHeight(QFontMetrics(sf).height() + 8 + 2);
    }
    lay->addWidget(_search);

    _scroll = new QScrollArea(this);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scroll->setWidgetResizable(true);
    lay->addWidget(_scroll, 1);

    _grid = new QWidget;
    _grid->setStyleSheet("background: transparent;");
    _scroll->setWidget(_grid);

    // Drive grid navigation from keys typed in the search field.
    _search->installEventFilter(this);

    buildGrid();

    connect(_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        buildGrid(text.trimmed());
    });

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &EmojiPickerPopup::applyTheme
    );
}

void EmojiPickerPopup::applyTheme() {
    setStyleSheet(QString(
                      "QFrame#emojiPicker {"
                      "  background: %1;"
                      "  border: 1px solid %2;"
                      "  border-radius: 8px;"
                      "}"
                      "QLineEdit {"
                      "  border: 1px solid %2;"
                      "  border-radius: 4px;"
                      "  padding: 4px 8px;"
                      "  font-size: %5px;"
                      "  color: %3;"
                      "  background: %1;"
                      "}"
                      "QLineEdit:focus { border-color: %4; }"
                      "QScrollArea { border: none; background: transparent; }"
                      "QScrollArea > QWidget > QWidget { background: transparent; }"
                      // Thin rounded thumb matching the chat list (paintScrollThumb):
                      // 8px track, 2px margins → 4px visible thumb, on the light surface.
                      "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
                      "QScrollBar::handle:vertical {"
                      "  background: %6; border-radius: 2px; min-height: 20px; margin: 2px;"
                      "}"
                      "QScrollBar::handle:vertical:hover { background: %7; }"
                      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
                      "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
                      "  background: transparent;"
                      "}"
    )
                      .arg(
                          Th::qss(Th::c().surface.raised),
                          Th::qss(Th::c().divider.strong),
                          Th::qss(Th::c().text.primary),
                          Th::qss(Th::c().accent.def)
                      )
                      .arg(Th::c().fonts.md)
                      .arg(Th::qss(QColor(0, 0, 0, 80)), Th::qss(QColor(0, 0, 0, 140))));
}

void EmojiPickerPopup::setSession(Session *session) {
    _session = session;
}

void EmojiPickerPopup::open(const QPoint &globalPos) {
    _search->clear();
    buildGrid();
    move(globalPos);
    show();
    raise();
    _search->setFocus();
}

void EmojiPickerPopup::buildGrid(const QString &filter) {
    // Drop references to the about-to-be-deleted buttons before destroying them
    // so the selection helpers never touch dangling pointers.
    _btns.clear();
    _sel = -1;

    delete _grid->layout();
    const auto children = _grid->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *c : children)
        delete c;

    auto *gridLay = new QGridLayout(_grid);
    gridLay->setContentsMargins(2, 2, 2, 2);
    gridLay->setSpacing(2);

    int col = 0, row = 0;

    _btnBaseStyle = QString(
                        "QToolButton { border: none; border-radius: 4px; background: transparent; }"
                        "QToolButton:hover { background: %1; }"
    )
                        .arg(Th::qss(Th::c().surface.highlight));
    _btnSelStyle = QString("QToolButton { border: none; border-radius: 4px; background: %1; }")
                       .arg(Th::qss(Th::c().surface.highlight));

    auto makeBtn = [&](const QString &name) -> QToolButton * {
        auto *btn = new QToolButton(_grid);
        btn->setFixedSize(32, 32);
        btn->setToolTip(name);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(_btnBaseStyle);
        connect(btn, &QToolButton::clicked, this, [this, name] {
            hide();
            emit emojiSelected(name);
        });
        gridLay->addWidget(btn, row, col);
        _btns.push_back(btn);
        if (++col >= kCols) {
            col = 0;
            ++row;
        }
        return btn;
    };

    // Custom emoji from session — shown as downloaded images.
    if (_session) {
        const auto &emap = _session->emojiMap();
        for (auto it = emap.begin(); it != emap.end(); ++it) {
            const QString name = it.key();
            const QString url  = it.value();
            if (url.startsWith("alias:"))
                continue;
            if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
                continue;

            auto *btn = makeBtn(name);
            btn->setIconSize(QSize(22, 22));

            const QByteArray cached = _session->cachedImage(url);
            if (!cached.isEmpty()) {
                QPixmap px;
                if (px.loadFromData(cached) && !px.isNull())
                    btn->setIcon(QIcon(px));
            } else {
                QPointer<QToolButton> weak = btn;
                _session->downloadFile(url, [this, url, weak](QByteArray data) {
                    if (_session)
                        _session->cacheImage(url, data);
                    if (!weak)
                        return;
                    QPixmap px;
                    if (px.loadFromData(data) && !px.isNull())
                        weak->setIcon(QIcon(px));
                });
            }
        }
    }

    // Base Unicode emoji — rendered via platform color emoji font.
    static const QFont kEmojiFont = emojiFont(20);
    for (const QString &name : std::as_const(kBaseEmojiNames)) {
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive))
            continue;
        const QString ch = Emoji::fromName(name);
        if (ch.startsWith(':'))
            continue; // skip unknown names
        auto *btn = makeBtn(name);
        btn->setFont(kEmojiFont);
        btn->setText(ch);
    }

    _grid->adjustSize();

    // Auto-size the popup height to the rendered grid (capped at the full
    // height, where the scroll bar takes over) so a short filtered list
    // doesn't leave empty space below the last row.
    const int rows  = row + (col > 0 ? 1 : 0);
    const int gridH = rows > 0 ? rows * 32 + (rows - 1) * 2 + 4 /*grid margins*/ : 4;

    // Chrome around the scroll area: layout top/bottom margins (8+8), the
    // fixed-height search field, and the 6px spacing below it.
    const int chrome   = 8 + _search->height() + 6 + 8;
    const int maxGridH = kFullHeight - chrome;
    setFixedHeight(chrome + std::min(gridH, maxGridH));

    // Pre-select the first match so Enter picks the top hit while typing.
    setSelected(_btns.isEmpty() ? -1 : 0);
}

void EmojiPickerPopup::setSelected(int idx) {
    if (_sel >= 0 && _sel < _btns.size() && _btns[_sel])
        _btns[_sel]->setStyleSheet(_btnBaseStyle);

    if (idx < 0 || idx >= _btns.size()) {
        _sel = -1;
        return;
    }
    _sel = idx;
    if (_btns[_sel]) {
        _btns[_sel]->setStyleSheet(_btnSelStyle);
        _scroll->ensureWidgetVisible(_btns[_sel], 0, 8);
    }
}

void EmojiPickerPopup::moveSelection(int delta) {
    if (_btns.isEmpty())
        return;
    const int base = _sel < 0 ? 0 : _sel;
    setSelected(std::clamp(base + delta, 0, static_cast<int>(_btns.size()) - 1));
}

bool EmojiPickerPopup::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _search && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (_sel >= 0 && _sel < _btns.size() && _btns[_sel])
                _btns[_sel]->click();
            return true;
        case Qt::Key_Up:
            moveSelection(-kCols);
            return true;
        case Qt::Key_Down:
            moveSelection(kCols);
            return true;
        case Qt::Key_Left:
            moveSelection(-1);
            return true;
        case Qt::Key_Right:
            moveSelection(1);
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}
