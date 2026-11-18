// The relay protocol (docs/claude-remote-relay.md): what msga and `msga
// --cc-relay` on another machine say to each other, one JSON object per line
// over the relay's stdin and stdout (an ssh session's). Every message has a
// type, "t"; a request has an "id" its reply ("re") carries back.
//
//   relay → msga  hello, roster, data/reset/gone/file (the mirror), synced,
//                 questions, tail, re, ping
//   msga → relay  hello, follow, tails, ping, cancel, and the requests
//
// Here: the version handshake, the shapes both sides share (sessions,
// questions, commands…) and the mirror's path rules. The two ends are
// relay_server.* and host_remote.cpp.
#pragma once

#include "app/claude/approvals.h"
#include "app/claude/catalog.h"
#include "app/claude/common.h"
#include "app/claude/launcher.h"
#include "app/claude/roster.h"
#include "app/claude/vt.h"
#include "base/json.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace claude::relay {

// `msga --cc-relay`: the relay mode of the msga executable.
inline constexpr std::string_view kFlag = "--cc-relay";

// The protocol's version: raised for any change an older peer would get
// wrong. Each side says the range it speaks; they use the highest both do.
inline constexpr int kProtocol    = 1;
inline constexpr int kProtocolMin = 1;

// Limits. A line longer than kMaxLine ends the link (a peer gone wrong).
inline constexpr size_t  kMaxLine     = size_t(80) << 20;
inline constexpr size_t  kChunk       = size_t(256) << 10; // mirror bytes per data message
inline constexpr int64_t kMaxUpload   = int64_t(64) << 20; // one attached file
inline constexpr size_t  kUploadChunk = size_t(192) << 10; // its bytes per message (raw)
inline constexpr int     kPingMs      = 15'000;            // each side, when it has said nothing
inline constexpr int     kSilentMs    = 50'000;            // msga: a relay this quiet is gone
inline constexpr int     kRelayIdleMs = 120'000;           // the relay: msga this quiet is gone

// What a relay can do beyond the basics, in its hello.
inline constexpr std::string_view kCapAttach  = "attach";  // live typing, reading questions
inline constexpr std::string_view kCapHook    = "hook";    // its sessions ask through the hook
inline constexpr std::string_view kCapStopCmd = "stopcmd"; // background commands stoppable

struct Hello {
    int                      protocol = 0, protocolMin = 0;
    int64_t                  msgaVersion = 0;
    std::vector<std::string> caps;
    // The relay's: Claude Code's folder there, whether its CLI is there,
    // the OS it runs on.
    std::string              home, os;
    bool                     claude = false;
    // An error the relay says instead of serving ("" = none).
    std::string              error;
};
std::string helloLine(const Hello &h, std::string_view extraJson = {});
bool        readHello(const json::Value &v, Hello *out);
// The version both speak, 0 when there's none.
int         agreedProtocol(const Hello &a, const Hello &b);
int64_t     msgaVersion(); // this build's MSGA_VERSION
std::string osName();

// ── Shapes ──────────────────────────────────────────────────────────────────
void                     writeSession(json::Writer &w, const SessionInfo &s);
void                     readSession(const json::Value &v, SessionInfo *s);
void                     writeQuestion(json::Writer &w, const HookQuestion &q);
void                     readQuestion(const json::Value &v, HookQuestion *q);
void                     writePermissionQuestion(json::Writer &w, const PermissionQuestion &q);
void                     readPermissionQuestion(const json::Value &v, PermissionQuestion *q);
void                     writeCommand(json::Writer &w, const SlashCommand &c);
void                     readCommand(const json::Value &v, SlashCommand *c);
void                     writeAccount(json::Writer &w, const Account &a);
void                     readAccount(const json::Value &v, Account *a);
void                     writeCatalogEntry(json::Writer &w, const CatalogEntry &e);
void                     readCatalogEntry(const json::Value &v, CatalogEntry *e);
void                     writeStrings(json::Writer &w, const std::vector<std::string> &v);
std::vector<std::string> readStrings(const json::Value &v);

std::string_view      loginWord(Login l); // "in" / "out" / "unknown"
Login                 parseLogin(std::string_view s);
// AttachInput / AttachAnswer outcomes as words, and back (unknown = NotReady).
std::string_view      inputOutcomeWord(AttachInput::Outcome o);
AttachInput::Outcome  parseInputOutcome(std::string_view s);
std::string_view      answerOutcomeWord(AttachAnswer::Outcome o);
AttachAnswer::Outcome parseAnswerOutcome(std::string_view s);

// ── The mirror ──────────────────────────────────────────────────────────────
// Paths the relay names are relative to Claude Code's folder there, and only
// these: the projects' transcripts (and their subagents'), the jobs' small
// files, the prompt history. Anything else — absolute, "..", another folder
// — is refused, so a relay gone wrong can't write elsewhere on this machine.
bool                    mirrorPathOk(std::string_view rel);
// What both ends compare to tell that a mirror file's start is the
// original's: a hash of its last (up to) 4 KB.
uint64_t                tailHash(std::string_view bytes);
inline constexpr size_t kTailHashBytes = 4096;

} // namespace claude::relay
