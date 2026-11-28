// exportThread — behind the thread panel's
// "Download thread as text": fetches the WHOLE thread from the service
// (Backend::loadThread reads every replies page; the open panel may hold only
// part of a long thread), then writes a plain-text transcript to `path`.
//
//   exportThread(ctx, conv, root, "#general", "/home/me/thread-2026-03-15.txt");
//
// Registered with model::jobs() ("Downloading thread…") until the file is
// written or the export failed; nothing blocks: the names are resolved on
// the UI thread a slice at a time, the transcript is put together and
// written on a worker (model::runInBackground). The job belongs to nobody:
// it survives the thread panel switching threads or closing. A failed page
// aborts it (no silently truncated file), as does the workspace going away
// before the replies arrive; the reason is logged.
#pragma once

#include "screens/common/context.h"

#include <functional>
#include <string>

namespace screens {

// title: the conversation's label for the header ("#general", a DM peer's
// name; empty: no header line). done(ok) on the UI thread, never inside the
// call; ok = the file was written.
void exportThread(
    Context                  &ctx,
    ConvRef                   conv,
    Ts                        root,
    std::string               title,
    std::string               path,
    std::function<void(bool)> done = {}
);

// The header label exportThread takes for conv (the
// people of a DM or group DM, "#name" for a channel; "" when unnamed).
std::string threadExportTitle(const Store &store, ConvRef conv);

} // namespace screens
