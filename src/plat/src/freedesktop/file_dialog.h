// File dialogs: xdg-desktop-portal's FileChooser (works for Flatpak/Snap and
// gives GNOME/KDE their native dialog), else zenity or kdialog as a child
// process, else an immediate empty answer. Everything is asynchronous: the
// portal answers with a Request object that later emits Response, and the
// fallback child's stdout is read through watchFd.
//
// $PLAT_FILE_DIALOG_FALLBACK picks the fallback: unset = auto (kdialog
// first on KDE, else zenity first), "zenity" / "kdialog" = only that tool,
// "auto" = auto, "0" = none (tests use it so a missing portal never pops up
// a real dialog). A request made with tools = false (showFileDialogEx: the
// app has its own chooser) runs a tool only when the variable names one.
// The answer is Unavailable when nothing could be shown: no portal and no
// tool, a portal that answers "ended some other way" (code 2) before a
// dialog could have been on screen (its backend failed), or a tool that
// exited with an error. A later code 2 is a cancel: GNOME's and GTK's
// backends answer 2 when their dialog is closed with Escape or the window's
// close button, and reading that as Unavailable put the app's own chooser up
// right after the native one.
#pragma once

#include "freedesktop/dbus_conn.h"

#include <sys/types.h>

#include <chrono>
#include <map>
#include <memory>

namespace plat::linux_services {

class FileChooser {
public:
    using Callback = std::function<void(FileDialogResult)>;

    FileChooser(BackendApp &app, Bus &bus);
    ~FileChooser();

    // parent: the portal's parent_window ("x11:…", "wayland:…" or "").
    void show(const FileDialogDesc &d, std::string parent, bool tools, Callback cb);
    // Call from the session bus's onReady: requests made while connecting.
    void busReady();

private:
    struct Request {
        FileDialogDesc                        desc;
        std::string                           parent;
        Callback                              cb;
        std::string                           path;  // the portal Request object we expect
        std::chrono::steady_clock::time_point asked; // when the portal was called
        uint64_t                              sub   = 0;
        TimerId                               timer = 0; // waiting for the bus
        bool tools = true; // zenity/kdialog when the portal is missing
    };
    struct Child {
        uint64_t    request = 0;
        pid_t       pid     = -1;
        int         fd      = -1;
        uint64_t    watch   = 0;
        std::string out;
        bool        kdialog = false;
    };

    void portal(uint64_t id);
    void onResponse(uint64_t id, DBusMessage *m);
    void fallback(uint64_t id);
    bool spawn(uint64_t id, const std::string &tool, bool kdialog);
    void childReadable(uint64_t id);
    void reap(uint64_t id, int attempt);
    void finish(uint64_t id, FileDialogResult r);

    BackendApp                 &_app;
    Bus                        &_bus;
    uint64_t                    _next = 1;
    std::map<uint64_t, Request> _requests;
    std::map<uint64_t, Child>   _children; // by request id
    std::shared_ptr<int>        _alive = std::make_shared<int>(0);
};

} // namespace plat::linux_services
