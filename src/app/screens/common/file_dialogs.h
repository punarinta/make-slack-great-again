// File and folder choosers for every screen: the OS dialog (plat's
// showFileDialogEx, parented to Context::window), else — on Linux, when plat
// reports none (no FileChooser portal) or $MSGA_FILE_DIALOG=builtin — the
// in-app ui::FileBrowser over that window. Elsewhere "none" is a cancel. Callbacks run later on the
// UI thread, once; an empty answer means cancelled. Callers whose view may be gone by then guard
// their captures (a weak token), as with backend calls.
#pragma once

#include "screens/common/context.h"

#include <functional>
#include <string>
#include <vector>

namespace screens {

using PathsDone = std::function<void(std::vector<std::string> paths)>;
using PathDone  = std::function<void(std::string path)>; // "" = cancelled

// Any mode; d.parent is filled in from ctx.window.
void fileDialog(Context &ctx, plat::FileDialogDesc d, PathsDone done);

// Files to attach (OpenMultiple unless multiple = false), from startDir
// when it is a folder, else the last folder used this session.
void pickFiles(
    Context                      &ctx,
    PathsDone                     done,
    bool                          multiple = true,
    std::vector<plat::FileFilter> filters  = {},
    std::string                   startDir = {}
);
// Where to save `suggestedName`; starts in startDir, else Downloads.
void saveFile(Context &ctx, std::string suggestedName, PathDone done, std::string startDir = {});
// A folder (the Settings downloads folder); starts in startDir, else Downloads.
void pickFolder(Context &ctx, PathDone done, std::string startDir = {});

// Drag and drop of files: whether a DropEnter/DropMove offers any (a URI
// list), and a Drop's local paths (file:// URIs decoded; others skipped).
bool                     dragOffersFiles(const plat::Event *raw);
std::vector<std::string> droppedFiles(const plat::Event *raw);
// The local paths of file:// URIs (others skipped), and of a text/uri-list
// (RFC 2483: CRLF or LF lines, '#' comments) — copied files on a clipboard.
std::vector<std::string> localPaths(const std::vector<std::string> &uris);
std::vector<std::string> uriListPaths(std::string_view list);

} // namespace screens
