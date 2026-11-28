// ui_gallery — every ui widget in a fake chat screen modelled on msga's
// look: workspace rail, sidebar with sections/badges/selection, a
// header with tabs, a 10,000-message VirtualList, and the composer with its
// formatting toolbar. Also the toolkit's perf probe (--bench).
//
//   ui_gallery [--dark] [--messages N] [--menu|--context|--emoji] [--type TEXT]
//              [--jump INDEX] [--hover X Y] [--bench STEPS] [--exit-after MS]
#include "base/str.h"
#include "gfx/icons_generated.h"
#include "plat/testing.h"
#include "ui/ui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ui;
using gfx::Icon;

namespace {

// ── Fake data ───────────────────────────────────────────────────────────────

struct Rng {
    uint32_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int n) { return int(next() % uint32_t(n)); }
};

const char *kNames[] = {
    "Priya Natarajan",
    "Mira Okafor",
    "Jonas Weber",
    "Lena Sørensen",
    "Sam Rivera",
    "Yuki Tanaka",
    "Alex Chen",
    "Ravi Patel",
    "Tomas Novak",
    "Deploy Bot"
};
const char *kAvatars[] = {
    "priya", "mira", "jonas", "lena", "sam", "yuki", "alex", "ravi", "tomas", "deploybot"
};
constexpr int kPeople = 10;

const char *kPhrases[] = {
    "Started on the new empty states.",
    "Crit notes from yesterday are in the channel canvas.",
    "Can someone export the hero illustration at 2x for the blog post?",
    "PNG is fine.",
    "Action items are tagged with owners — tick yours off as you go.",
    "I think the onboarding flow is one step too long.",
    "Pushed a fix for the flaky login test, CI is green again.",
    "Does anyone know why the staging build takes twelve minutes now?",
    "Looks great to me, ship it.",
    "Let's pair on this after lunch.",
    "The palette v3 contrast numbers are in the doc.",
    "We should keep the old import tool around for one more release.",
    "Too chatty?",
    "Moved the retro to Thursday so Lena can join.",
    "Reminder: design review at 3 PM in the big room.",
    "I left a few comments on the pull request.",
    "Numbers from the A/B test look promising, but the sample is small.",
    "Who owns the release notes this week?",
    "Updated the Figma file with the new spacing scale.",
    "Heads up: the API rate limits change on Monday.",
};
constexpr int kPhraseCount = int(sizeof(kPhrases) / sizeof(kPhrases[0]));
const char   *kWords[]     = {"deploy", "canvas", "tokens", "sidebar", "release", "export"};

enum Kind : uint8_t { Full, Continued, Day };

struct Msg {
    uint32_t seed;
    uint16_t minute; // minutes since the day started
    uint8_t  author;
    uint8_t  kind;
    uint8_t  reactions; // 0..3 pills
    uint8_t  attach;    // 0 none, 1 image, 2 animated gif
    int      sent = -1; // index into Chat::sent for messages typed in the composer
};

std::string readFile(const std::string &path) {
    std::string out;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char   buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

std::shared_ptr<const gfx::Bitmap> loadImage(const std::string &path, int maxPx) {
    gfx::Bitmap b;
    if (!gfx::decodeImage(readFile(path), &b))
        return nullptr;
    if (b.width() > maxPx || b.height() > maxPx) {
        const float s = float(maxPx) / float(std::max(b.width(), b.height()));
        b             = gfx::resize(
            b.view(), std::max(1, int(b.width() * s)), std::max(1, int(b.height() * s))
        );
    }
    return std::make_shared<const gfx::Bitmap>(std::move(b));
}

std::string clock(int minute) {
    const int h = (minute / 60) % 24, m = minute % 60;
    char      buf[16];
    std::snprintf(
        buf, sizeof buf, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM"
    );
    return buf;
}

text::Style bodyStyle() {
    text::Style s = font(Font::Body);
    s.color       = themed(C::Text); // resolved per build: follows theme switches
    return s;
}

// A generated message body with the occasional bold word, code span or link.
text::AttributedText messageText(uint32_t seed) {
    Rng                  r{seed | 1};
    text::AttributedText t;
    const text::Style    base      = bodyStyle();
    const int            sentences = 1 + r.range(3);
    for (int i = 0; i < sentences; ++i) {
        if (i)
            t.append(" ", base);
        t.append(kPhrases[r.range(kPhraseCount)], base);
        const int extra = r.range(8);
        if (extra == 0) {
            t.append(" See ", base);
            text::Style link = base;
            link.color       = themed(C::Link);
            link.linkId      = 1;
            t.append("the design doc", link);
            t.append(".", base);
        } else if (extra == 1) {
            t.append(" Run ", base);
            text::Style code = base;
            code.mono        = true;
            code.size        = base.size - 2;
            code.color       = themed(C::CodeText);
            code.background  = themed(C::CodeBg);
            t.append(std::string("make ") + kWords[r.range(6)], code);
            t.append(" first.", base);
        } else if (extra == 2) {
            t.append(" This is ", base);
            text::Style b = base;
            b.weight      = text::Weight::Bold;
            t.append("important", b);
            t.append(".", base);
        }
    }
    return t;
}

// ── Rows ────────────────────────────────────────────────────────────────────

class Gallery;

class MessageRow final : public Clickable {
public:
    explicit MessageRow(Gallery *g, bool compact);
    bool onEvent(Event &e) override;

    Gallery *gallery;
    int      index  = -1;
    Image   *avatar = nullptr, *attach = nullptr;
    Label   *name = nullptr, *time = nullptr, *body = nullptr;
    View    *reactions = nullptr;
};

class DayRow final : public View {
public:
    DayRow() {
        style().row().padding(20, 10).spacing(12).items(Align::Center);
        add<Separator>()->style().flex(1);
        label = add<Label>("", Font::SmallBold, C::TextMuted);
        label->setBorder(C::Border);
        label->setBackground(C::Surface, 12);
        label->style().padding(12, 3);
        add<Separator>()->style().flex(1);
    }
    Label *label;
};

class Chat final : public VirtualList::Adapter {
public:
    Chat(Gallery *g) : gallery(g) {}
    void generate(uint32_t seed, int n) {
        msgs.clear();
        sent.clear();
        Rng r{seed};
        int minute = 8 * 60, last = -1, day = 0;
        for (int i = 0; i < n; ++i) {
            if (i % 180 == 0) {
                msgs.push_back({r.next(), 0, 0, Day, 0, 0});
                msgs.back().minute = uint16_t(day++);
                last               = -1;
            }
            Msg m{r.next(), uint16_t(minute), uint8_t(r.range(kPeople)), Full, 0, 0};
            if (r.range(3) == 0 && last >= 0)
                m.author = uint8_t(last); // runs of messages by the same person
            m.kind = m.author == last ? Continued : Full;
            if (r.range(6) == 0)
                m.reactions = uint8_t(1 + r.range(3));
            if (r.range(40) == 0)
                m.attach = uint8_t(1 + r.range(2));
            msgs.push_back(m);
            last = m.author;
            minute += r.range(9);
        }
    }

    int                   count() const override { return int(msgs.size()); }
    int                   kind(int i) const override { return msgs[size_t(i)].kind; }
    std::unique_ptr<View> create(int k) override {
        if (k == Day)
            return std::make_unique<DayRow>();
        return std::make_unique<MessageRow>(gallery, k == Continued);
    }
    void  bind(View &row, int i) override;
    float estimateHeight(int i) const override {
        const Msg &m = msgs[size_t(i)];
        return m.kind == Day ? 44 : m.kind == Continued ? 26 : 56 + (m.attach ? 180 : 0);
    }

    Gallery                          *gallery;
    std::vector<Msg>                  msgs;
    std::vector<text::AttributedText> sent;
};

// ── Sidebar ─────────────────────────────────────────────────────────────────

class SideRow final : public Clickable {
public:
    SideRow(
        std::string                        text,
        Icon                               icon,
        std::shared_ptr<const gfx::Bitmap> avatar = nullptr,
        int                                unread = 0,
        bool                               bold   = false
    ) {
        Look l;
        l.hover   = C::SidebarHover;
        l.pressed = C::SidebarHover;
        l.checked = C::SidebarSelected;
        l.radius  = 6;
        setLook(l);
        style().row().height(30).padding(10, 0).spacing(8).items(Align::Center);
        if (avatar) {
            auto *img = add<Image>();
            img->style().size(20, 20);
            img->setRadius(4);
            img->setBitmap(std::move(avatar));
        } else {
            glyph = add<IconView>(icon, 16, C::SidebarTextMuted);
        }
        label = add<Label>(std::move(text), bold ? Font::BodyBold : Font::Body, C::SidebarText);
        label->style().flex(1);
        label->setMaxLines(1);
        if (unread > 0)
            add<Badge>(unread);
    }
    void select(bool on) {
        setChecked(on);
        label->setColor(on ? C::SidebarSelectedText : C::SidebarText);
        if (glyph)
            glyph->setTint(on ? C::SidebarSelectedText : C::SidebarTextMuted);
    }
    Label    *label;
    IconView *glyph = nullptr;
};

// ── The screen ──────────────────────────────────────────────────────────────

class Gallery {
public:
    explicit Gallery(Window &w, const std::string &assets, int messages);

    void openMessageMenu(int index, PointF windowPos);
    void openMoreMenu();
    void openEmoji();
    void send();
    void refreshToolbar();
    void selectChannel(SideRow *row, const char *name, uint32_t seed);

    Window                                         &win;
    Chat                                            chat{this};
    std::vector<std::shared_ptr<const gfx::Bitmap>> avatars;
    std::shared_ptr<const gfx::Bitmap>              picture;
    std::shared_ptr<const Image::Frames>            gif;
    VirtualList                                    *list  = nullptr;
    TextEdit                                       *edit  = nullptr;
    Label                                          *title = nullptr;
    Button  *themeBtn = nullptr, *moreBtn = nullptr, *emojiBtn = nullptr;
    Button  *fmtBold = nullptr, *fmtItalic = nullptr, *fmtStrike = nullptr, *fmtCode = nullptr;
    SideRow *selected = nullptr;
    int      messages;
};

MessageRow::MessageRow(Gallery *g, bool compact) : gallery(g) {
    setLook({C::None, C::SurfaceHover, C::SurfaceHover, C::None, 0});
    setFocusable(false);
    setRole(Role::ListItem);
    style().row().padding(20, compact ? 2 : 8, 20, compact ? 2 : 4).spacing(10).items(Align::Start);
    if (compact) {
        add<View>()->style().width(36).height(1);
    } else {
        avatar = add<Image>();
        avatar->style().size(36, 36).margins(0, 2, 0, 0);
        avatar->setRadius(6);
        avatar->setPlaceholder(C::Border);
    }
    auto *col = add<View>();
    col->style().flex(1).spacing(2);
    if (!compact) {
        auto *header = col->add<View>();
        header->style().row().spacing(8).items(Align::Center);
        name = header->add<Label>("", Font::BodyBold);
        time = header->add<Label>("", Font::Small, C::TextFaint);
    }
    body         = col->add<Label>();
    body->onLink = [](uint32_t) { std::printf("link clicked\n"); };
    attach       = col->add<Image>();
    attach->setRadius(8);
    attach->setFit(Image::Fit::Contain);
    attach->style().margins(0, 4, 0, 4);
    attach->style().alignSelf(Align::Start);
    attach->setVisible(false);
    reactions = col->add<View>();
    reactions->style().row().spacing(4).margins(0, 4, 0, 2).items(Align::Center);
    reactions->style().alignSelf(Align::Start);
    reactions->setVisible(false);
}

bool MessageRow::onEvent(Event &e) {
    if (e.type == EventType::ContextMenu) {
        gallery->openMessageMenu(index, e.windowPos);
        return true;
    }
    return Clickable::onEvent(e);
}

void Chat::bind(View &row, int i) {
    const Msg &m = msgs[size_t(i)];
    if (m.kind == Day) {
        static const char *days[] = {
            "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
        };
        const int   d = m.minute;
        std::string s = d == int(msgs.size() / 181) ? "Today" : std::string(days[d % 7]);
        static_cast<DayRow &>(row).label->setText(std::move(s));
        return;
    }
    auto &r = static_cast<MessageRow &>(row);
    r.index = i;
    if (r.avatar) {
        r.avatar->setBitmap(gallery->avatars[m.author]);
        r.name->setText(kNames[m.author]);
        r.time->setText(clock(m.minute));
    }
    r.body->setRichText(m.sent >= 0 ? sent[size_t(m.sent)] : messageText(m.seed));
    if (m.attach == 1 && gallery->picture) {
        r.attach->setBitmap(gallery->picture);
        const float w = std::min(360.f, float(gallery->picture->width()));
        r.attach->style().size(w, w * gallery->picture->height() / gallery->picture->width());
    } else if (m.attach == 2 && gallery->gif) {
        r.attach->setFrames(gallery->gif);
        const gfx::Bitmap &f0 = gallery->gif->front().frame;
        r.attach->style().size(160, 160.f * f0.height() / std::max(1, f0.width()));
    }
    r.attach->setVisible(m.attach != 0);
    // Reactions: reuse the pill buttons already there.
    static const char *emoji[] = {"👍", "🎉", "🙏", "👀", "✅"};
    Rng                rr{m.seed ^ 0x9e3779b9u};
    for (size_t k = 0; k < r.reactions->childCount(); ++k)
        r.reactions->child(k)->setVisible(int(k) < m.reactions);
    for (int k = 0; k < m.reactions; ++k) {
        Button *b;
        if (size_t(k) < r.reactions->childCount()) {
            b = static_cast<Button *>(r.reactions->child(size_t(k)));
        } else {
            b = r.reactions->add<Button>("", Button::Kind::Secondary);
            b->style().height(24).padding(8, 0);
            b->setLook({C::MentionBg, C::Hover, C::Pressed, C::MentionBg, 12});
            b->setBorder(C::None);
            b->onClick = [b] { b->setChecked(!b->checked()); };
        }
        b->setLabel(std::string(emoji[rr.range(5)]) + " " + str::number(1 + rr.range(9)));
    }
    r.reactions->setVisible(m.reactions > 0);
}

Gallery::Gallery(Window &w, const std::string &assets, int n) : win(w), messages(n) {
    for (const char *a : kAvatars)
        avatars.push_back(loadImage(assets + "/avatars/" + a + ".png", 72));
    picture = loadImage(assets + "/images/onboarding-flow-b.png", 720);
    {
        auto frames = std::make_shared<Image::Frames>();
        if (gfx::decodeAnimation(readFile(assets + "/gifs/party-confetti.gif"), frames.get()) &&
            !frames->empty())
            gif = frames;
    }
    chat.generate(42, n);

    View &root   = w.root();
    auto *screen = root.add<View>();
    screen->style().row();

    // Workspace rail.
    auto *rail = screen->add<View>();
    rail->setBackground(C::Rail);
    rail->style().width(68).padding(14, 14).spacing(12).items(Align::Center);
    auto *tile = rail->add<View>();
    tile->style().size(40, 40).stack().items(Align::Center);
    tile->setBackground(C::Accent, 8);
    tile->add<Label>("A", Font::Title, C::AccentText);
    auto *addWs = rail->add<IconButton>(Icon::Plus, "Add a workspace");
    addWs->setTextColor(C::SidebarText);
    rail->add<View>()->style().flex(1);
    auto *prefs = rail->add<IconButton>(Icon::Settings2, "Preferences");
    prefs->setTextColor(C::SidebarText);

    // Sidebar.
    auto *side = screen->add<View>();
    side->setBackground(C::Sidebar);
    side->style().width(260);
    auto *wsRow = side->add<View>();
    wsRow->style().row().height(52).padding(18, 0).spacing(6).items(Align::Center);
    wsRow->add<Label>("Acme design", Font::BodyBold, C::SidebarText);
    wsRow->add<IconView>(Icon::ChevronDown, 14, C::SidebarText);
    auto *scroll = side->add<ScrollView>();
    scroll->style().flex(1);
    View *items = scroll->content();
    items->style().padding(8, 4).spacing(1);
    auto section = [&](const char *text, Icon icon) {
        auto *h = items->add<Clickable>();
        h->setLook({C::None, C::SidebarHover, C::SidebarHover, C::None, 6});
        h->style()
            .row()
            .height(28)
            .padding(10, 0)
            .spacing(8)
            .items(Align::Center)
            .margins(0, 6, 0, 0);
        h->add<IconView>(icon, 14, C::SidebarTextMuted);
        h->add<Label>(text, Font::SmallBold, C::SidebarTextMuted);
        return h;
    };
    auto channel = [&](const char *name, Icon icon, int unread, bool bold, uint32_t seed) {
        auto *row    = items->add<SideRow>(name, icon, nullptr, unread, bold);
        row->onClick = [this, row, name, seed] { selectChannel(row, name, seed); };
        return row;
    };
    section("Threads", Icon::MessagesSquare);
    section("Starred", Icon::Star);
    channel("general", Icon::Hash, 0, false, 7);
    SideRow            *design = channel("design", Icon::Hash, 0, false, 42);
    auto               *chHead = section("Channels", Icon::ChevronDown);
    std::vector<View *> chans;
    chans.push_back(channel("engineering", Icon::Hash, 1, true, 11));
    auto *rel = channel("releases", Icon::Hash, 0, true, 12);
    rel->add<Badge>()->setDot(true);
    chans.push_back(rel);
    chans.push_back(channel("launch-atlas", Icon::Lock, 0, false, 13));
    chans.push_back(channel("random", Icon::Hash, 0, false, 14));
    auto *addCh = items->add<SideRow>("Add channels", Icon::Plus);
    chans.push_back(addCh);
    chHead->onClick = [chans] { // collapse/expand: a relayout, nothing rebuilt
        for (View *v : chans)
            v->setVisible(!v->visible());
    };
    section("Direct messages", Icon::MessageSquare);
    for (int i = 1; i < 8; ++i) {
        auto *dm =
            items->add<SideRow>(kNames[i], Icon::Hash, avatars[size_t(i)], i == 1 ? 2 : 0, i == 1);
        dm->onClick = [this, dm, i] { selectChannel(dm, kNames[i], 100u + uint32_t(i)); };
    }
    auto *me = side->add<View>();
    me->style().row().height(56).padding(14, 0).spacing(10).items(Align::Center);
    auto *meImg = me->add<Image>();
    meImg->style().size(32, 32);
    meImg->setCircle(true);
    meImg->setBitmap(avatars[5]);
    auto *meName = me->add<Label>(kNames[5], Font::Body, C::SidebarText);
    meName->style().flex(1);
    meName->setMaxLines(1);
    auto *status = me->add<IconButton>(Icon::CircleUserRound, "Set a status");
    status->setTextColor(C::SidebarText);

    // Main pane.
    auto *main = screen->add<View>();
    main->style().flex(1);
    main->setBackground(C::Surface);
    auto *header = main->add<View>();
    header->style().row().height(56).padding(20, 0, 12, 0).spacing(4).items(Align::Center);
    title = header->add<Label>("#design", Font::Title);
    header->add<View>()->style().flex(1);
    header->add<IconButton>(Icon::Users, "9 members");
    header->add<IconButton>(Icon::Star, "Star channel");
    header->add<IconButton>(Icon::Search, "Search");
    auto *jump = header->add<Button>("Jump to…", Button::Kind::Ghost);
    jump->setTooltip("Scroll smoothly to a random older message");
    jump->onClick = [this] {
        static Rng r{12345};
        const int  i = r.range(std::max(1, chat.count()));
        list->scrollToItem(i, VirtualList::ItemAlign::Center, true);
    };
    themeBtn =
        header->add<Button>(app()->dark() ? "Light mode" : "Dark mode", Button::Kind::Secondary);
    themeBtn->onClick = [this] {
        const bool dark = !app()->dark();
        app()->setThemeMode(dark ? ThemeMode::Dark : ThemeMode::Light);
        themeBtn->setLabel(dark ? "Light mode" : "Dark mode");
    };
    moreBtn          = header->add<IconButton>(Icon::MoreHorizontal, "More");
    moreBtn->onClick = [this] { openMoreMenu(); };
    auto *tabs       = main->add<View>();
    tabs->style().row().padding(12, 0).spacing(4);
    auto *t1 = tabs->add<Button>("Messages", Button::Kind::Tab);
    t1->setIcon(Icon::MessageCircle);
    t1->setIconSize(16);
    t1->setChecked(true);
    auto *t2 = tabs->add<Button>("Design crit — week 38", Button::Kind::Tab);
    t2->setIcon(Icon::Canvas);
    t2->setIconSize(16);
    t1->onClick = [t1, t2] {
        t1->setChecked(true);
        t2->setChecked(false);
    };
    t2->onClick = [t1, t2] {
        t2->setChecked(true);
        t1->setChecked(false);
    };
    main->add<Separator>();

    list = main->add<VirtualList>(&chat);
    list->setBackground(C::Surface); // opaque: scrolling can blit
    list->style().flex(1);
    list->setStickToBottom(true);
    list->setBottomAligned(true);

    // Composer.
    auto *wrap = main->add<View>();
    wrap->style().padding(20, 8, 20, 20);
    auto *box = wrap->add<View>();
    box->setBackground(C::InputBg, 8);
    box->setBorder(C::InputBorder);
    auto *toolbar = box->add<View>();
    toolbar->style().row().padding(6, 6, 6, 0).spacing(2).items(Align::Center);
    auto fmt = [&](Icon icon, const char *tip, TextEdit::Format f) {
        auto *b    = toolbar->add<IconButton>(icon, tip);
        b->onClick = [this, f] {
            edit->toggleFormat(f);
            refreshToolbar();
        };
        b->setLook({C::None, C::Hover, C::Pressed, C::Pressed, metric(M::RadiusM)});
        return b;
    };
    fmtBold   = fmt(Icon::Bold, "Bold", TextEdit::Bold);
    fmtItalic = fmt(Icon::Italic, "Italic", TextEdit::Italic);
    toolbar->add<IconButton>(Icon::Underline, "Underline");
    fmtStrike = fmt(Icon::Strikethrough, "Strikethrough", TextEdit::Strike);
    auto sep  = [&] {
        auto *s = toolbar->add<Separator>(true);
        s->style().height(18).margins(4, 0, 4, 0);
    };
    sep();
    toolbar->add<IconButton>(Icon::Link, "Link");
    toolbar->add<IconButton>(Icon::ListOrdered, "Ordered list");
    toolbar->add<IconButton>(Icon::List, "Bulleted list");
    sep();
    toolbar->add<IconButton>(Icon::Quote, "Quote");
    fmtCode = fmt(Icon::Code, "Code", TextEdit::Code);
    toolbar->add<IconButton>(Icon::Braces, "Code block");
    edit = box->add<TextEdit>();
    edit->setPlaceholder("Message #design");
    edit->setMaxLines(10);
    edit->style().padding(14, 10);
    edit->onSubmit = [this] {
        send();
        return true;
    };
    edit->onSelectionChange = [this] { refreshToolbar(); };
    auto *bottom            = box->add<View>();
    bottom->style().row().padding(6, 0, 8, 6).spacing(2).items(Align::Center);
    bottom->add<IconButton>(Icon::Paperclip, "Attach");
    emojiBtn          = bottom->add<IconButton>(Icon::Smile, "Emoji");
    emojiBtn->onClick = [this] { openEmoji(); };
    bottom->add<IconButton>(Icon::Gif, "GIF");
    bottom->add<IconButton>(Icon::AtSign, "Mention someone");
    bottom->add<View>()->style().flex(1);
    auto *sendBtn    = bottom->add<IconButton>(Icon::Send, "Send");
    sendBtn->onClick = [this] { send(); };

    w.addShortcut(plat::Key::K, Window::kPrimary, [this] { edit->focus(); });
    w.addShortcut(plat::Key::D, Window::kPrimary | plat::ModShift, [this] {
        themeBtn->activate();
    });

    design->select(true);
    selected = design;
    edit->focus();
}

void Gallery::selectChannel(SideRow *row, const char *name, uint32_t seed) {
    if (selected)
        selected->select(false);
    row->select(true);
    selected = row;
    title->setText(std::string(seed >= 100 ? "" : "#") + name);
    edit->setPlaceholder(std::string("Message ") + (seed >= 100 ? "" : "#") + name);
    chat.generate(seed, messages / (seed == 42 ? 1 : 4));
    list->reset();
    edit->focus();
}

void Gallery::refreshToolbar() {
    fmtBold->setChecked(edit->formatActive(TextEdit::Bold));
    fmtItalic->setChecked(edit->formatActive(TextEdit::Italic));
    fmtStrike->setChecked(edit->formatActive(TextEdit::Strike));
    fmtCode->setChecked(edit->formatActive(TextEdit::Code));
}

void Gallery::send() {
    if (edit->empty())
        return;
    // The composer's runs become the message's rich text.
    text::AttributedText t;
    const std::string   &s = edit->text();
    for (const TextEdit::Run &r : edit->runs()) {
        text::Style st = bodyStyle();
        if (r.format & TextEdit::Bold)
            st.weight = text::Weight::Bold;
        if (r.format & TextEdit::Italic)
            st.italic = true;
        if (r.format & TextEdit::Strike)
            st.strike = true;
        if (r.format & TextEdit::Code) {
            st.mono       = true;
            st.size       = st.size - 2;
            st.color      = themed(C::CodeText);
            st.background = themed(C::CodeBg);
        }
        if (!r.link.empty()) {
            st.color  = themed(C::Link);
            st.linkId = 1;
        }
        t.append(std::string_view(s).substr(r.start, r.end - r.start), st);
    }
    chat.sent.push_back(std::move(t));
    Msg m{0, uint16_t(chat.msgs.empty() ? 0 : chat.msgs.back().minute + 1), 5, Full, 0, 0};
    if (!chat.msgs.empty() && chat.msgs.back().kind != Day && chat.msgs.back().author == 5)
        m.kind = Continued;
    m.sent = int(chat.sent.size() - 1);
    chat.msgs.push_back(m);
    list->itemsInserted(chat.count() - 1, 1);
    list->scrollToBottom(true);
    edit->clear();
}

void Gallery::openMessageMenu(int index, PointF at) {
    std::vector<MenuItem> items = {
        {1, "Reply in thread", "T", uint16_t(Icon::MessageSquareReply)},
        {2, "Copy link", "L", uint16_t(Icon::Link)},
        {3, "Mark unread", "U", uint16_t(Icon::Eye)},
        {4, "Pin to channel", "P", uint16_t(Icon::Pin)},
        {0, {}, {}, Button::kNoIcon, true, false, true},
        {5, "Edit message", "E", uint16_t(Icon::Edit3), false},
        {6, "Delete message…", "Del", uint16_t(Icon::Trash2), true, false, false, true},
    };
    Menu::show(
        win,
        {at.x, at.y, 0, 0},
        std::move(items),
        [this, index](int id) {
            std::printf("message %d: action %d\n", index, id);
            if (id == 6 && index >= 0 && index < chat.count()) {
                chat.msgs.erase(chat.msgs.begin() + index);
                list->itemsRemoved(index, 1);
            }
        },
        Popup::Place::Over
    );
}

void Gallery::openMoreMenu() {
    std::vector<MenuItem> items = {
        {1, "Open channel details", "", uint16_t(Icon::Users)},
        {2, "Mute channel", "", uint16_t(Icon::BellOff)},
        {3, "Show unread first", "", Button::kNoIcon, true, true},
        {0, {}, {}, Button::kNoIcon, true, false, true},
        {4, "Copy channel link", "", uint16_t(Icon::Copy)},
        {5, "Leave channel", "", uint16_t(Icon::LogOut), true, false, false, true},
    };
    const RectF r = moreBtn->windowRect();
    Menu::show(win, {r.x - 180 + r.w, r.y, 180, r.h}, std::move(items), [](int id) {
        std::printf("more: %d\n", id);
    });
}

void Gallery::openEmoji() {
    static const char *emoji[] = {"😀", "😂", "🥲", "😍", "🤔", "😅", "😎", "🙃", "🎉", "🔥",
                                  "👍", "👀", "🙏", "✅", "❤️",  "🚀", "💡", "☕", "🐛", "📌"};
    auto               p       = std::make_unique<Popup>();
    p->style().padding(10).spacing(4);
    p->add<Label>("Frequently used", Font::SmallBold, C::TextMuted)->style().margins(4, 0, 0, 4);
    View  *rowV = nullptr;
    Popup *raw  = p.get();
    for (int i = 0; i < 20; ++i) {
        if (i % 5 == 0) {
            rowV = p->add<View>();
            rowV->style().row().spacing(2);
        }
        auto *c = rowV->add<Clickable>();
        c->setLook({C::None, C::Hover, C::Pressed, C::None, 6});
        c->style().size(40, 40).stack().items(Align::Center);
        c->add<Label>(emoji[i], Font::Title);
        const char *e = emoji[i];
        c->onClick    = [this, raw, e] {
            raw->close();
            edit->insertText(e);
        };
    }
    p->setAnchor(emojiBtn->windowRect(), Popup::Place::Above);
    win.showPopup(std::move(p));
}

long rssKb() {
    long  kb = -1;
    FILE *f  = std::fopen("/proc/self/status", "r");
    if (!f)
        return kb;
    char line[256];
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "VmRSS:", 6) == 0)
            kb = std::atol(line + 6);
    std::fclose(f);
    return kb;
}

} // namespace

int main(int argc, char **argv) {
    std::string typeText, assets = MSGA_TEST_ASSETS;
    bool        dark = false, menu = false, context = false, emoji = false;
    int         bench = 0, jumpTo = -1, exitAfter = 0, messages = 10000;
    float       hoverX = -1, hoverY = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a    = argv[i];
        auto              next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dark")
            dark = true;
        else if (a == "--menu")
            menu = true;
        else if (a == "--context")
            context = true;
        else if (a == "--emoji")
            emoji = true;
        else if (a == "--type")
            typeText = next();
        else if (a == "--bench")
            bench = std::atoi(next());
        else if (a == "--jump")
            jumpTo = std::atoi(next());
        else if (a == "--jump-attach")
            jumpTo = -2;
        else if (a == "--messages")
            messages = std::atoi(next());
        else if (a == "--exit-after")
            exitAfter = std::atoi(next());
        else if (a == "--assets")
            assets = next();
        else if (a == "--hover") {
            hoverX = float(std::atof(next()));
            hoverY = float(std::atof(next()));
        }
    }
    std::string err;
    auto        app = App::create(&err);
    if (!app) {
        std::fprintf(stderr, "ui_gallery: %s\n", err.c_str());
        return 1;
    }
    if (dark)
        app->setThemeMode(ThemeMode::Dark);
    const long       rss0 = rssKb();
    const double     t0   = app->nowMs();
    plat::WindowDesc desc;
    desc.title = "ui gallery";
    desc.appId = "msga-ui-gallery";
    desc.size  = {1200, 800};
    Window  win(desc);
    Gallery g(win, assets, messages);

    // Settle: first frames (layout + full paint).
    for (int i = 0; i < 20; ++i)
        app->pump(5);
    const double startup = app->nowMs() - t0;
    std::fprintf(
        stderr,
        "startup to first frames: %.1f ms, frames=%d, first paint %.2f ms\n",
        startup,
        win.stats().frames,
        win.stats().lastPaintMs
    );

    if (!typeText.empty()) {
        g.edit->insertText(typeText);
        g.edit->setSelection(0, uint32_t(std::min<size_t>(typeText.find(' '), typeText.size())));
        g.edit->toggleFormat(TextEdit::Bold);
        g.edit->setSelection(uint32_t(typeText.size()), uint32_t(typeText.size()));
    }
    if (jumpTo == -2) // the newest message with an animated GIF, and a picture above it
        for (int i = g.chat.count() - 1; i >= 0; --i)
            if (g.chat.msgs[size_t(i)].attach == 2) {
                jumpTo = i;
                break;
            }
    if (jumpTo >= 0)
        g.list->scrollToItem(jumpTo, VirtualList::ItemAlign::End, false);
    if (menu)
        g.openMoreMenu();
    if (emoji)
        g.openEmoji();
    if (context)
        g.openMessageMenu(g.chat.count() - 3, {640, 420});
    if (hoverX >= 0)
        if (auto *h = app->platform().testHooks())
            h->injectPointerMove(win.native(), {hoverX, hoverY});

    if (bench > 0) {
        // Scroll step by step (40 px, like a fast touchpad), one frame each,
        // and record the frame cost the toolkit reports.
        std::vector<double> frame, paint, layout, step;
        std::vector<long>   area;
        for (int i = 0; i < 30; ++i)
            app->pump(5);
        for (int i = 0; i < bench; ++i) {
            const int    f0 = win.stats().frames;
            const double ts = app->nowMs();
            // scrollBy measures the rows scrolling in (text layout), the frame
            // lays out, blits and paints: time both.
            g.list->scrollBy(-40);
            const double tm = app->nowMs() - ts;
            for (int k = 0; k < 100 && win.stats().frames == f0; ++k)
                app->pump(2);
            step.push_back(tm + win.stats().lastFrameMs);
            frame.push_back(win.stats().lastFrameMs);
            paint.push_back(win.stats().lastPaintMs);
            layout.push_back(win.stats().lastLayoutMs);
            long a = 0;
            for (const plat::Rect &r : win.stats().lastDamage)
                a += long(r.w) * r.h;
            if (std::getenv("UI_DEBUG_DAMAGE") && i < 12) {
                std::printf("step %d:", i);
                for (const plat::Rect &r : win.stats().lastDamage)
                    std::printf(" [%d,%d %dx%d]", r.x, r.y, r.w, r.h);
                std::printf("\n");
            }
            area.push_back(a);
        }
        auto report = [&](const char *name, std::vector<double> v) {
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (double x : v)
                sum += x;
            std::printf(
                "%-7s avg %.3f ms  p50 %.3f  p95 %.3f  max %.3f\n",
                name,
                sum / double(v.size()),
                v[v.size() / 2],
                v[v.size() * 95 / 100],
                v.back()
            );
        };
        std::printf(
            "backend %s, scale %.2f, %d scroll steps of 40 px, %d messages\n",
            app->platform().backendName(),
            double(win.scale()),
            bench,
            g.chat.count()
        );
        report("step", step);
        report("frame", frame);
        report("layout", layout);
        report("paint", paint);
        long asum = 0;
        for (long a : area)
            asum += a;
        const SizeF sz = win.size();
        std::printf(
            "damage  avg %.1f%% of the window per step\n",
            100.0 * double(asum) / double(area.size()) /
                (double(sz.w) * sz.h * win.scale() * win.scale())
        );
        // A far animated jump.
        const double tj = app->nowMs();
        g.list->scrollToItem(g.chat.count() / 3, VirtualList::ItemAlign::Center, true);
        for (int k = 0; k < 2000; ++k) {
            app->pump(2);
            if (VirtualList *l = g.list; l->viewFor(g.chat.count() / 3) &&
                                         l->anchor().index <= g.chat.count() / 3 && k > 10 &&
                                         win.stats().lastFrameMs >= 0) {
                View *v = l->viewFor(g.chat.count() / 3);
                if (std::abs(v->frame().y - (l->height() - v->frame().h) / 2) < 1)
                    break;
            }
        }
        std::printf(
            "jump to item %d: %.0f ms, measured rows %d of %d, live rows %d\n",
            g.chat.count() / 3,
            app->nowMs() - tj,
            g.list->measuredCount(),
            g.chat.count(),
            g.list->liveCount()
        );
        std::printf("rss before window %ld KB, after bench %ld KB\n", rss0, rssKb());
        return 0;
    }
    if (exitAfter > 0)
        app->addTimer(exitAfter, false, [&] { app->quit(); });
    std::fprintf(stderr, "rss %ld KB\n", rssKb());
    app->run();
    return 0;
}
