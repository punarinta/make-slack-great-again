// The shell's dialogs:
//
//   Forward      "Forward this message" / "Forward this file": a
//                channel/person picker, the composer ("Add a message, if
//                you'd like."), the message preview, Copy Link / Cancel /
//                Forward. Re-posts the text (and files) as my own message in
//                the pick.
//   Workspace icon the rail bubble preview, Choose image… / Use default, a
//                hint, Cancel / Save; the icon lives in the data folder and
//                only this app shows it.
//   Rename       "Name conversation" /
//                "Rename session": a Name field (80 characters, the derived
//                name as its placeholder), a hint, Cancel / Save. Sets the
//                conversation's local name ("" = back to the derived one).
//   Section      "Create a section" / "Rename section" / "Delete section":
//                the sidebar's own sections.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shell {

// A workspace a message may be forwarded into: a running one's Store and
// Backend.
struct ForwardWorkspace {
    std::string           key, name;
    model::Store         *store   = nullptr;
    model::Backend       *backend = nullptr;
    std::function<bool()> alive; // still running (the send may come after a download)
};
// An agent workspace's teammate picked as the target: workspace `key` ("" = the open one) shows the
// teammate's page with `text` (mrkdwn) and the files in its composer, left to the user to pick a
// folder and send.
using PrefillTeammate = std::function<void(
    const std::string       &key,
    const std::string       &role,
    std::string              text,
    std::vector<std::string> paths
)>;
// onError(message): a file couldn't be fetched, a session couldn't start (the
// shell's error banner). With two or more `workspaces` (one of them the open
// Store's) a picker above the conversation selector chooses where it goes,
// starting on the open one; else it stays in the open workspace. Without
// prefillTeammate a teammate can't be picked successfully ("Couldn't open the
// chat.").
ui::Popup *showForwardDialog(
    screens::Context                        &ctx,
    ui::Window                              &w,
    model::ConvRef                           conv,
    model::Ts                                ts,
    const std::string                       &onlyFile,
    std::function<void(const std::string &)> onError,
    std::vector<ForwardWorkspace>            workspaces      = {},
    PrefillTeammate                          prefillTeammate = {}
);
// The forwarded text for another workspace (portable mrkdwn): `mrkdwn`
// from `source` with its workspace-local tokens turned into the words they
// read as there — <@U…> "@Name", <#C…> "#name", <!subteam^…> "@handle",
// <!here>/<!channel>/<!everyone> plain "@here"… (nobody is pinged), dates
// their fallback text. Links, emphasis and :emoji: stay as they are.
std::string portableMrkdwn(const model::Store &source, std::string_view mrkdwn);

// The workspace's own icon file (the data folder), "" when none.
std::string customWorkspaceIconPath(plat::App &app, const std::string &workspaceId);
// The workspace's own picture if it has one, else `serverIcon`.
std::string
workspaceIcon(plat::App &app, const std::string &workspaceId, const std::string &serverIcon);
// Its own picture goes (the workspace is signed out of).
void       removeCustomWorkspaceIcon(plat::App &app, const std::string &workspaceId);
// done(path) after Save: the new icon ("" = back to the default). The active
// workspace's, or the one with `workspaceId` (its server icon `defaultIcon`).
ui::Popup *showWorkspaceIconDialog(
    screens::Context                        &ctx,
    ui::Window                              &w,
    Avatars                                 &avatars,
    std::function<void(const std::string &)> done
);
ui::Popup *showWorkspaceIconDialog(
    screens::Context                        &ctx,
    ui::Window                              &w,
    Avatars                                 &avatars,
    const std::string                       &workspaceId,
    const std::string                       &defaultIcon,
    std::function<void(const std::string &)> done
);

ui::Popup *showRenameDialog(screens::Context &ctx, ui::Window &w, model::ConvRef conv);

// "Create a section" (id "") / "Rename section": the emoji button and the
// Name field (80 characters) in one box, a hint, Cancel / Create (Save). A
// new section holds `first` (kNoConv: none).
ui::Popup *showSectionDialog(
    screens::Context &ctx, ui::Window &w, const std::string &id, model::ConvRef first
);
// "Delete section": Cancel / Delete; its conversations go back.
ui::Popup *showDeleteSectionDialog(screens::Context &ctx, ui::Window &w, const std::string &id);

// Settings → Tray icon: the
// tray-like preview, Choose image… / Use default, the hint, "Convert to
// monochrome", Cancel / Save. On Save the picture is copied into the data
// folder: done(path, monochrome), path "" = back to the built-in icon.
using TrayIconDone = std::function<void(const std::string &path, bool monochrome)>;
ui::Popup *showTrayIconDialog(
    screens::Context  &ctx,
    ui::Window        &w,
    const std::string &current, // the copied picture now in use ("" = none)
    bool               monochrome,
    TrayIconDone       done
);
// The tray picture: bytes → image (SVG rendered at
// the stored size), then fitted into a kTrayIconSize square, aspect kept and
// centred, and as a white silhouette with `monochrome`.
constexpr int kTrayIconSize = 128;
bool          decodeTrayPicture(std::string_view bytes, gfx::Bitmap *out);
gfx::Bitmap   trayPicture(const gfx::Bitmap &src, bool monochrome);
void          trayMonochrome(gfx::Bitmap *bmp);

} // namespace shell
