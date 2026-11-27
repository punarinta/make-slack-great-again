// A fixture workspace (fixture.json: the tests' tests/assets/fixture.json,
// or demo/fixture.json, format in demo/README.md) loaded into a Store: the
// fake workspace the tests and `msga --demo demo` show, with its
// validation messages and derived read cursors. Times in the file are relative ("-2d 09:14", "+7m")
// so a recording made on any day looks like this week.
#pragma once

#include "app/model/store.h"

#include <string>
#include <string_view>
#include <vector>

namespace fake {

// A canned reply posted after the user sends into `conv`: `typingMs` of
// "is typing…" (0 = none), then the message. Consumed in file order per
// conversation; inThread ones answer a thread reply (and land in that thread).
struct AutoReply {
    model::ConvRef conv = model::kNoConv;
    model::UserRef user = model::kNoUser;
    std::string    text; // mrkdwn
    int            afterMs  = 1200;
    int            typingMs = 1500;
    bool           inThread = false;
};

// A link preview attached ~1 s after a sent message containing `url`
// (prefix match), like Slack's unfurl.
struct Unfurl {
    std::string       url;
    model::Attachment attachment;
};

struct AiReply {
    std::string match; // case-insensitive substring of the request
    std::string text;
};

// A conversation's bookmarks ("bookmarks": [{title, link, emoji}]).
struct Bookmarks {
    model::ConvRef               conv = model::kNoConv;
    std::vector<model::Bookmark> items;
};

struct Canvas {
    model::ConvRef conv = model::kNoConv;
    std::string    htmlPath; // absolute; the inner HTML of Slack's quip-canvas-content
};

// A workflow ("workflows": [{trigger, title, description, author, form}]),
// featured by a conversation ("featuredWorkflows": [trigger]) or bookmarked
// ({"workflow": trigger} in its "bookmarks"). Its form, in the model's terms:
// {title, submit, close, fields: [{kind, label, hint, placeholder,
// optional, multi, options: [text]}]}, kind one of text, paragraph,
// number, people, channels, checkboxes, radio, select, date, time,
// datetime, note. Submitted, `author` posts the answers where it started.
// No form: it just posts that it ran.
struct Workflow {
    model::WorkflowLink link;
    std::string         author; // a user id
    model::Form         form;   // id "" = none
};

// What the fixture holds beyond the Store's data.
struct Fixture {
    std::string            dir;    // absolute; asset paths resolve against it
    std::string            gifDir; // absolute; "gifs" (default assets/gifs): GIF search
    model::ConvRef         startConversation = model::kNoConv;
    std::vector<AutoReply> autoReplies;
    std::vector<Unfurl>    unfurls;
    std::vector<AiReply>   aiReplies;
    std::string            aiDefault;
    std::vector<Canvas>    canvases;
    std::vector<Bookmarks> bookmarks;
    std::vector<Workflow>  workflows;
    const Workflow        *workflow(const std::string &trigger) const;
};

// Loads `path` (a directory holding fixture.json, or a .json file) into
// `store` (cleared first) and *fx. `now` (epoch seconds) anchors the
// relative times; tests pass a fixed one. On failure returns false, sets
// *error to a readable reason and leaves the store cleared.
bool loadFixture(
    std::string_view path, model::Store &store, Fixture *fx, std::string *error, int64_t now
);

// Resolves a fixture time spec to epoch seconds:
//   "-2d 09:14"  two days before today at 09:14 (local)   "14:32"  today at 14:32
//   "-45m"       45 minutes before now  (s/m/h/d)          "+7m"    7 minutes after `prev`
// prev < 0 means "no previous message" ("+7m" is then relative to now).
// False when the spec is malformed.
bool parseTimeSpec(std::string_view spec, int64_t now, int64_t prev, int64_t *out);

// A File record for a local file: mime from the name and magic bytes,
// size from disk, image dimensions from the header (PNG, GIF, JPEG, WebP) —
// no decoder needed. Also used for demo uploads.
model::File fileFromLocalPath(const std::string &absPath, int index);

} // namespace fake
