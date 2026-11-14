// The demo tour's script (demo/tour.json) as data:
// the verbs demo::Tour performs and the parser — no UI, so tests cover it.
// Compiled only into demo builds (-DMSGA_DEMO=ON) and the tests.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

struct TourStep {
    enum class Kind : uint8_t {
        Wait,         // ms
        Open,         // arg = conversation id — click its sidebar row
        Scroll,       // num = pixels (negative = up) in the message list
        Hover,        // arg = message text fragment — park the pointer on it
        Thread,       // arg = message text fragment — open its thread panel
        CloseThread,  //
        Type,         // arg = text, num = characters per second (composer, or the focused field)
        Key,          // arg = Return | Tab | Escape | Down | Up | Backspace
        Send,         // press the send key in the focused composer
        React,        // arg = message text fragment, arg2 = emoji name
        Search,       // arg = query — open search, type it, run it
        CloseSearch,  //
        QuickSwitch,  // arg = text typed into the quick switcher, then Enter
        Theme,        // arg = light | dark
        Settings,     // list = pages to walk, ms = per page
        MessageMenu,  // arg = message text fragment — its hover toolbar's "…"
        ChannelMenu,  // arg = conversation id — right-click its sidebar row
        MenuHover,    // arg = item label of the open context menu
        MenuPick,     // arg = item label of the open context menu — click it
        CloseMenu,    //
        MoveToThread, // arg = message text fragment, arg2 = target root text fragment
        DialogButton, // arg = button label in the topmost dialog — click it
        CloseDialog,  //
        Gif,          // arg = GIF search query — open the picker, search, pick the first
        Play,         // arg = message text fragment — press play/pause on its audio clip
        OpenImage,    // arg = message text fragment — open its image in the viewer
        CloseImage,   //
        OpenThreads,  // click the sidebar's "Threads" entry
        OpenSaved,    // click the sidebar's "Saved messages" entry
        Canvas,       // click the open conversation's canvas tab
        MessagesTab,  // click the "Messages" tab (back from the canvas)
        Post,         // conv + user + arg = mrkdwn: another user posts right now;
                      // arg2 = root text fragment → as a reply in that thread
        Click,        // arg = a view's name, tooltip or shown text (fragment) — click it;
                      // arg2 = where to look: sidebar | dialog | "" (popups, then the window)
        Point,        // as Click, but only moves the pointer there (what shows on hover)
        Field,        // num = which text field of the topmost dialog (0 = first) — click it
        Jobs,         // click the header's background jobs (opens their popover)
        Quit,         //
    };
    Kind                     kind = Kind::Wait;
    std::string              arg, arg2;
    std::vector<std::string> list;
    std::string              conv, user; // Post
    int                      ms  = 0;
    double                   num = 0;
};

struct TourScript {
    int                   width = 1280, height = 800;
    int                   pauseMs = 700; // idle between steps
    std::vector<TourStep> steps;
};

// The --demo-tour <path> / --demo-tour=<path> argument; "" when absent.
std::string tourPathFromArgs(int argc, char **argv);
// {"window":[w,h], "pause":700, "steps":[{"open":"C1"}, …]}. False (and
// *error says why) on a broken file.
bool        loadTour(const std::string &path, TourScript *out, std::string *error);
bool        parseTour(std::string_view json, TourScript *out, std::string *error);

} // namespace demo
