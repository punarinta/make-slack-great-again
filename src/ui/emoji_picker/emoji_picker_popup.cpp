// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "emoji_picker_popup.h"
#include "session/session.h"
#include "util/emoji.h"
#include "util/emoji_font.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QScrollArea>
#include <QToolButton>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QPointer>

// Ordered list of emoji names for grid display — resolved via Emoji::fromName.
static const QStringList kBaseEmojiNames {
    // Faces & emotions
    "smile","grin","laughing","joy","rofl","sweat_smile",
    "wink","heart_eyes","kissing_heart","stuck_out_tongue","thinking_face","raised_eyebrow",
    "neutral_face","expressionless","zipper_mouth","grimacing","sob","tired_face",
    "sleepy","mask","sunglasses","nerd_face","monocle_face","confused",
    "worried","angry","rage","skull","ghost","alien","poop","clown_face","partying_face",
    // Hands & people
    "wave","raised_hand","ok_hand","thumbsup","thumbsdown","clap",
    "pray","point_right","point_left","point_up","point_down","muscle",
    "handshake","writing_hand","selfie",
    // Hearts & symbols
    "heart","orange_heart","yellow_heart","green_heart","blue_heart","purple_heart",
    "broken_heart","sparkling_heart","two_hearts",
    "100","tada","fire","star","star2","sparkles","zap","boom","eyes","warning",
    // Animals
    "dog","cat","mouse","hamster","rabbit","fox_face","bear","panda_face","koala",
    "tiger","lion","cow","pig","frog","monkey_face","chicken","penguin","bird",
    "hatching_chick","eagle","owl","snake","turtle","lizard",
    "whale","dolphin","shark","octopus","bee","butterfly",
    "palm_tree","deciduous_tree","evergreen_tree","cactus","sunflower","rose",
    // Food
    "apple","banana","watermelon","grapes","strawberry","pizza","hamburger","fries",
    "hot_dog","taco","burrito","sushi","ramen","spaghetti","rice","bread","croissant",
    "cake","cupcake","cookie","chocolate_bar","candy","lollipop","ice_cream",
    "coffee","tea","beer",
    // Travel & places
    "rocket","airplane","car","bus","train","bicycle","boat","house","office",
    "school","hospital","bank","sunrise","city_sunset","night_with_stars",
    "earth_americas","earth_africa","earth_asia",
    // Objects & misc
    "computer","desktop_computer","keyboard","phone","telephone","email",
    "memo","pencil","paperclip","scissors","lock","key",
    "hammer","wrench","gear","bulb","flashlight","books",
    "moneybag","credit_card","chart","trophy","medal","gift",
    "balloon","confetti_ball","musical_note","headphones","microphone","camera",
    "hourglass_flowing_sand","clock1","calendar","x","question","exclamation",
    "bell","information_source","white_check_mark","warning",
};

EmojiPickerPopup::EmojiPickerPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setObjectName("emojiPicker");
    setFixedSize(300, 340);
    setStyleSheet(
        "QFrame#emojiPicker {"
        "  background: #FFFFFF;"
        "  border: 1px solid #D1D1D1;"
        "  border-radius: 8px;"
        "}"
        "QLineEdit {"
        "  border: 1px solid #D1D1D1;"
        "  border-radius: 4px;"
        "  padding: 4px 8px;"
        "  font-size: 13px;"
        "  color: #1D1C1D;"
        "  background: #FFFFFF;"
        "}"
        "QLineEdit:focus { border-color: #007A5A; }"
        "QScrollArea { border: none; background: transparent; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"
    );

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    _search = new QLineEdit(this);
    _search->setPlaceholderText(tr("Search emoji…"));
    lay->addWidget(_search);

    _scroll = new QScrollArea(this);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scroll->setWidgetResizable(true);
    lay->addWidget(_scroll, 1);

    _grid = new QWidget;
    _grid->setStyleSheet("background: transparent;");
    _scroll->setWidget(_grid);

    buildGrid();

    connect(_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        buildGrid(text.trimmed());
    });
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
    delete _grid->layout();
    const auto children = _grid->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *c : children) delete c;

    auto *gridLay = new QGridLayout(_grid);
    gridLay->setContentsMargins(2, 2, 2, 2);
    gridLay->setSpacing(2);

    const int cols = 8;
    int col = 0, row = 0;

    static const QString kBtnStyle =
        "QToolButton { border: none; border-radius: 4px; background: transparent; }"
        "QToolButton:hover { background: #F0F0F0; }";

    auto makeBtn = [&](const QString &name) -> QToolButton * {
        auto *btn = new QToolButton(_grid);
        btn->setFixedSize(32, 32);
        btn->setToolTip(name);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(kBtnStyle);
        connect(btn, &QToolButton::clicked, this, [this, name] {
            hide();
            emit emojiSelected(name);
        });
        gridLay->addWidget(btn, row, col);
        if (++col >= cols) { col = 0; ++row; }
        return btn;
    };

    // Custom emoji from session — shown as downloaded images.
    if (_session) {
        const auto &emap = _session->emojiMap();
        for (auto it = emap.begin(); it != emap.end(); ++it) {
            const QString name = it.key();
            const QString url  = it.value();
            if (url.startsWith("alias:")) continue;
            if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)) continue;

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
                    if (_session) _session->cacheImage(url, data);
                    if (!weak) return;
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
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)) continue;
        const QString ch = Emoji::fromName(name);
        if (ch.startsWith(':')) continue; // skip unknown names
        auto *btn = makeBtn(name);
        btn->setFont(kEmojiFont);
        btn->setText(ch);
    }

    _grid->adjustSize();
}
