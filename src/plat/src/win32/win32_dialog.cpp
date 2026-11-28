// Win32 backend, file dialogs: the Common Item Dialog (IFileOpenDialog /
// IFileSaveDialog, Vista+; Wine implements it too).
//
// IFileDialog::Show() is modal and runs its own message loop. plat's
// contract is asynchronous, so showFileDialog() queues the request and a
// message to the loop's message window runs it — outside runPosted() and
// runDueTimers(), so the dialog never stalls the rest of a batch — with
// enterModal() keeping plat timers, posted work and frames going meanwhile.
// The result is posted, so the callback never runs inside the dialog.
#include "win32/win32.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

namespace plat::win32 {

namespace {

std::string itemPath(IShellItem *item) {
    PWSTR       p = nullptr;
    std::string out;
    if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = portablePath(p);
        CoTaskMemFree(p);
    }
    return out;
}

#ifdef PLAT_TEST_HOOKS
std::wstring parentOf(const std::wstring &p) {
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

bool samePath(const std::wstring &a, const std::wstring &b) {
    // NTFS names compare case-insensitively (ordinal, like the file system).
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// What fileDialogRespond() does to a shown dialog: type the answer into
// the file-name box and press OK, or close it as cancelled — through the
// dialog's own window, so its parsing and validation run as for a user.
bool driveDialog(IFileDialog *dlg, const std::vector<std::string> &answer, bool *acted) {
    IOleWindow *ow = nullptr;
    HWND        h  = nullptr;
    if (SUCCEEDED(dlg->QueryInterface(IID_IOleWindow, reinterpret_cast<void **>(&ow)))) {
        ow->GetWindow(&h);
        ow->Release();
    }
    if (!h || !IsWindowVisible(h))
        return false; // not up yet: the timer tries again
    if (*acted) {
        // Still open long after we pressed OK (a validation message, a
        // name it would not take): cancel rather than hang the test in a
        // modal loop nobody will ever end. A message box the dialog put up
        // blocks it, so that goes first.
        if (HWND box = GetWindow(h, GW_ENABLEDPOPUP); box && box != h)
            PostMessageW(box, WM_CLOSE, 0, 0);
        dlg->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        return true;
    }
    *acted = true;
    if (answer.empty()) {
        dlg->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        return true;
    }
    std::wstring text;
    if (answer.size() == 1) {
        text = nativePath(answer[0]);
    } else {
        // Several files: the name box takes "a" "b" relative to the folder
        // shown, full paths otherwise.
        std::wstring folder;
        IShellItem  *cur = nullptr;
        if (SUCCEEDED(dlg->GetFolder(&cur)) && cur) {
            folder = nativePath(itemPath(cur));
            cur->Release();
        }
        bool relative = !folder.empty();
        for (const auto &a : answer)
            relative = relative && samePath(parentOf(nativePath(a)), folder);
        for (const auto &a : answer) {
            const std::wstring p = nativePath(a);
            text += (text.empty() ? L"\"" : L" \"") + (relative ? p.substr(folder.size() + 1) : p) +
                    L"\"";
        }
    }
    dlg->SetFileName(text.c_str());
    PostMessageW(h, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), LPARAM(GetDlgItem(h, IDOK)));
    return false; // keep watching until Show() returns (see above)
}
#endif // PLAT_TEST_HOOKS

} // namespace

void Win32App::showFileDialog(
    const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
) {
    showFileDialogEx(d, [cb = std::move(cb)](FileDialogResult r) { cb(std::move(r.paths)); });
}

void Win32App::showFileDialogEx(const FileDialogDesc &d, std::function<void(FileDialogResult)> cb) {
    DialogRequest r{
        d, d.parent ? static_cast<HWND>(d.parent->nativeHandle()) : nullptr, std::move(cb)
    };
    r.desc.parent =
        nullptr; // the Window may be gone by the time the request runs; the HWND is checked
    _dialogQueue.push_back(std::move(r));
    PostMessageW(_msgHwnd, kDialogMsg, 0, 0);
}

#ifdef PLAT_TEST_HOOKS
bool Win32App::fileDialogRespond(std::vector<std::string> paths) {
    _dialogAnswer = std::move(paths);
    return true;
}
#endif

void Win32App::runNextDialog() {
    // One at a time: a request made while a dialog is up (from a timer
    // inside its modal loop) waits for it.
    if (_dialogActive || _dialogQueue.empty())
        return;
    DialogRequest r = std::move(_dialogQueue.front());
    _dialogQueue.erase(_dialogQueue.begin());
    _dialogActive          = true;
    const HWND       owner = r.owner && IsWindow(r.owner) ? r.owner : nullptr;
    FileDialogResult res   = runFileDialog(r.desc, owner);
    _dialogActive          = false;
    post([cb = std::move(r.cb), res = std::move(res)]() mutable { cb(std::move(res)); });
    if (!_dialogQueue.empty())
        PostMessageW(_msgHwnd, kDialogMsg, 0, 0);
}

FileDialogResult Win32App::runFileDialog(const FileDialogDesc &d, HWND owner) {
    using Mode      = FileDialogDesc::Mode;
    const bool save = d.mode == Mode::Save;
#ifdef PLAT_TEST_HOOKS
    // The answer is taken now, so a dialog that cannot be created does not
    // leave it lying around for an unrelated later one.
    std::optional<std::vector<std::string>> answer = std::move(_dialogAnswer);
    _dialogAnswer.reset();
#endif

    IFileDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(
            save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            save ? IID_IFileSaveDialog : IID_IFileOpenDialog,
            reinterpret_cast<void **>(&dlg)
        )) ||
        !dlg)
        return {.status = FileDialogResult::Status::Unavailable}; // no shell (a stripped Wine)

    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    // FORCEFILESYSTEM: only real paths (no Libraries / phones / zip views);
    // NOCHANGEDIR: never move the process's working directory.
    o |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
    switch (d.mode) {
    case Mode::Open:
        o |= FOS_FILEMUSTEXIST;
        break;
    case Mode::OpenMultiple:
        o |= FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT;
        break;
    case Mode::Save:
        o |= FOS_OVERWRITEPROMPT;
        break;
    case Mode::PickFolder:
        o |= FOS_PICKFOLDERS;
        break;
    }
    dlg->SetOptions(o);
    if (!d.title.empty())
        dlg->SetTitle(toWide(d.title).c_str());

    // Filters: "Images" + {"*.png", "*.jpg"} → L"Images", L"*.png;*.jpg".
    std::vector<std::wstring>      names, specs;
    std::vector<COMDLG_FILTERSPEC> filters;
    if (d.mode != Mode::PickFolder) {
        for (const auto &f : d.filters) {
            std::wstring spec;
            for (const auto &p : f.patterns)
                spec += (spec.empty() ? L"" : L";") + toWide(p);
            if (spec.empty())
                continue;
            names.push_back(toWide(f.name));
            specs.push_back(spec);
        }
        for (size_t i = 0; i < names.size(); ++i)
            filters.push_back({names[i].c_str(), specs[i].c_str()});
        if (!filters.empty()) {
            dlg->SetFileTypes(UINT(filters.size()), filters.data());
            dlg->SetFileTypeIndex(1);
            // Save: a name typed without extension gets the first filter's.
            const std::wstring &first = specs.front();
            if (save && first.size() > 2 && first.rfind(L"*.", 0) == 0 &&
                first.find_first_of(L";*?", 2) == std::wstring::npos)
                dlg->SetDefaultExtension(first.c_str() + 2);
        }
    }
    if (!d.initialDir.empty()) {
        IShellItem *folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                nativePath(d.initialDir).c_str(),
                nullptr,
                IID_IShellItem,
                reinterpret_cast<void **>(&folder)
            )) &&
            folder) {
            dlg->SetFolder(folder); // not SetDefaultFolder: the app asked for this one
            folder->Release();
        }
    }
    if (save && !d.suggestedName.empty())
        dlg->SetFileName(toWide(d.suggestedName).c_str());

#ifdef PLAT_TEST_HOOKS
    TimerId drive = 0;
    if (answer) {
        // Polled through a plat timer, which enterModal() keeps firing
        // inside Show(): the dialog's window only exists once it is up.
        bool acted = false;
        auto start = core::Clock::now();
        drive      = addTimer(50, true, [&, start] {
            const bool late = core::Clock::now() - start > std::chrono::seconds(5);
            if ((!acted || late) && driveDialog(dlg, *answer, &acted)) {
                cancelTimer(drive);
                drive = 0;
            }
        });
    }
#endif
    enterModal();
    const HRESULT hr = dlg->Show(owner);
    leaveModal();
#ifdef PLAT_TEST_HOOKS
    if (drive)
        cancelTimer(drive);
#endif

    std::vector<std::string> out;
    if (hr == S_OK) {
        if (d.mode == Mode::OpenMultiple) {
            IShellItemArray *items = nullptr;
            if (SUCCEEDED(static_cast<IFileOpenDialog *>(dlg)->GetResults(&items)) && items) {
                DWORD n = 0;
                items->GetCount(&n);
                for (DWORD i = 0; i < n; ++i) {
                    IShellItem *it = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                        if (std::string p = itemPath(it); !p.empty())
                            out.push_back(std::move(p));
                        it->Release();
                    }
                }
                items->Release();
            }
        } else {
            IShellItem *it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)) && it) {
                if (std::string p = itemPath(it); !p.empty())
                    out.push_back(std::move(p));
                it->Release();
            }
        }
    }
    dlg->Release();
    FileDialogResult res;
    res.status =
        out.empty() ? FileDialogResult::Status::Cancelled : FileDialogResult::Status::Chosen;
    // Show() failing for any reason but the user's cancel: it never appeared.
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        res.status = FileDialogResult::Status::Unavailable;
    res.paths = std::move(out);
    return res;
}

} // namespace plat::win32
