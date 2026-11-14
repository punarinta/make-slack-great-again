#include "screens/shell/demo/tour_script.h"

#include "base/file.h"
#include "base/json.h"
#include "base/str.h"

#include <cstring>

namespace demo {

namespace {

using K = TourStep::Kind;

struct Verb {
    const char *name;
    K           kind;
    enum Arg : uint8_t { None, Text, Number, Pair, Special, Post } arg;
};

// The verb is whichever known key the step object carries ({"type": …,
// "cps": …} has two keys; the table, not the key order, decides).
constexpr Verb kVerbs[] = {
    {"wait", K::Wait, Verb::Number},
    {"open", K::Open, Verb::Text},
    {"scroll", K::Scroll, Verb::Number},
    {"hover", K::Hover, Verb::Text},
    {"thread", K::Thread, Verb::Text},
    {"closeThread", K::CloseThread, Verb::None},
    {"type", K::Type, Verb::Text},
    {"key", K::Key, Verb::Text},
    {"send", K::Send, Verb::None},
    {"react", K::React, Verb::Pair},
    {"search", K::Search, Verb::Text},
    {"closeSearch", K::CloseSearch, Verb::None},
    {"quickSwitch", K::QuickSwitch, Verb::Text},
    {"theme", K::Theme, Verb::Text},
    {"settings", K::Settings, Verb::Special},
    {"messageMenu", K::MessageMenu, Verb::Text},
    {"channelMenu", K::ChannelMenu, Verb::Text},
    {"menuHover", K::MenuHover, Verb::Text},
    {"menuPick", K::MenuPick, Verb::Text},
    {"closeMenu", K::CloseMenu, Verb::None},
    {"moveToThread", K::MoveToThread, Verb::Pair},
    {"dialogButton", K::DialogButton, Verb::Text},
    {"closeDialog", K::CloseDialog, Verb::None},
    {"gif", K::Gif, Verb::Text},
    {"play", K::Play, Verb::Text},
    {"openImage", K::OpenImage, Verb::Text},
    {"closeImage", K::CloseImage, Verb::None},
    {"openThreads", K::OpenThreads, Verb::None},
    {"openSaved", K::OpenSaved, Verb::None},
    {"canvas", K::Canvas, Verb::None},
    {"messagesTab", K::MessagesTab, Verb::None},
    {"post", K::Post, Verb::Post},
    {"click", K::Click, Verb::Text},
    {"point", K::Point, Verb::Text},
    {"field", K::Field, Verb::Number},
    {"jobs", K::Jobs, Verb::None},
    {"quit", K::Quit, Verb::None},
};

constexpr const char *kSettingsPages[] = {
    "appearance", "notifications", "ai", "storage", "system", "about"
};
constexpr const char *kKeys[] = {"Return", "Tab", "Escape", "Down", "Up", "Backspace"};

template <size_t N>
bool oneOf(const std::string &s, const char *const (&list)[N]) {
    for (const char *k : list)
        if (s == k)
            return true;
    return false;
}

} // namespace

std::string tourPathFromArgs(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--demo-tour") == 0 && i + 1 < argc)
            return argv[i + 1];
        if (std::strncmp(argv[i], "--demo-tour=", 12) == 0)
            return argv[i] + 12;
    }
    return {};
}

bool loadTour(const std::string &path, TourScript *out, std::string *error) {
    std::string text;
    if (!file::readAll(path, &text)) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    return parseTour(text, out, error);
}

bool parseTour(std::string_view text, TourScript *out, std::string *error) {
    auto fail = [error](std::string why) {
        if (error)
            *error = "tour: " + why;
        return false;
    };
    json::Document doc;
    if (!doc.parse(std::string(text), nullptr) || !doc.root().isObject())
        return fail("not a JSON object");
    const json::Value root = doc.root();
    TourScript        script;
    if (const json::Value w = root["window"]; w.size() == 2) {
        script.width  = int(w[0].integer());
        script.height = int(w[1].integer());
    }
    if (script.width < 800 || script.height < 600)
        return fail("window must be at least 800x600");
    script.pauseMs = int(root["pause"].integer(script.pauseMs));

    int n = 0;
    for (const json::Value o : root["steps"]) {
        ++n;
        const std::string at   = "step " + str::number(int64_t(n)) + ": ";
        const Verb       *verb = nullptr;
        for (const Verb &v : kVerbs)
            if (o.has(v.name)) {
                verb = &v;
                break;
            }
        if (!verb) {
            std::string first;
            for (const json::Value m : o) {
                first = std::string(m.key());
                break;
            }
            return fail(at + "unknown verb \"" + first + "\"");
        }
        TourStep st;
        st.kind               = verb->kind;
        const json::Value val = o[verb->name];
        switch (verb->arg) {
        case Verb::None:
            break;
        case Verb::Text:
            st.arg = std::string(val.str());
            if (st.arg.empty())
                return fail(at + "\"" + verb->name + "\" needs a value");
            break;
        case Verb::Number:
            st.num = val.number();
            st.ms  = int(val.integer(int64_t(st.num)));
            break;
        case Verb::Pair:
            if (val.size() != 2 || val[0].str().empty() || val[1].str().empty())
                return fail(at + verb->name + " needs [text, text]");
            st.arg  = std::string(val[0].str());
            st.arg2 = std::string(val[1].str());
            break;
        case Verb::Special: // settings: N (hold on Appearance) | {"pages": [...], "each": ms}
            if (val.isObject()) {
                for (const json::Value p : val["pages"])
                    st.list.emplace_back(p.str());
                st.ms = int(val["each"].integer(1600));
            } else {
                st.ms = int(val.integer(2500));
            }
            if (st.list.empty())
                st.list.emplace_back("appearance");
            for (const std::string &p : st.list)
                if (!oneOf(p, kSettingsPages))
                    return fail(at + "unknown settings page \"" + p + "\"");
            break;
        case Verb::Post: // {"conv": id, "user": id, "text": mrkdwn, "thread": fragment}
            st.conv = std::string(val["conv"].str());
            st.user = std::string(val["user"].str());
            st.arg  = std::string(val["text"].str());
            st.arg2 = std::string(val["thread"].str());
            if (st.conv.empty() || st.user.empty() || st.arg.empty())
                return fail(at + "post needs conv, user and text");
            break;
        }
        if (st.kind == K::Type)
            st.num = o["cps"].number(16);
        if (st.kind == K::Click || st.kind == K::Point) {
            st.arg2 = std::string(o["in"].str());
            if (!st.arg2.empty() && st.arg2 != "sidebar" && st.arg2 != "dialog")
                return fail(at + verb->name + " \"in\" is sidebar|dialog");
        }
        if (st.kind == K::Theme && st.arg != "light" && st.arg != "dark")
            return fail(at + "theme is light|dark");
        if (st.kind == K::Key && !oneOf(st.arg, kKeys))
            return fail(at + "key is one of Return, Tab, Escape, Down, Up, Backspace");
        script.steps.push_back(std::move(st));
    }
    if (script.steps.empty())
        return fail("no steps");
    *out = std::move(script);
    return true;
}

} // namespace demo
