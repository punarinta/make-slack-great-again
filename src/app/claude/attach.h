// Types a message into a live background session through its own terminal UI:
// `claude attach <short>` run in a hidden pseudo-terminal, the way a person at
// a terminal would. The message reaches the session without stopping it — so
// a subagent it runs, a background command or a scheduled prompt carries on —
// and arrives as the user's own prompt: answered at once when Claude is idle,
// queued by Claude Code until the next step when it's mid-turn. Stopping and
// resuming (Launcher::resume) would end all of that, and a second process
// can't hand the running one a prompt any other way (verified with Claude Code
// 2.1.282: the cross-session socket delivers a peer's message, not the user's,
// and the daemon's terminal socket is private).
//
// Verified live with 2.1.282, 2026-09-25:
//   • Attaching mirrors the session's screen; several attachers can be on at
//     once (nobody is pushed off), and ending `attach` leaves the session be.
//   • A bracketed paste of more than one line, or of a long line, arrives
//     wrapped in <pasted_content> — which Claude reads as material the user
//     pasted, not what they said. Line by line, a few hundred characters at a
//     time, with a newline (LF) between lines, it arrives exactly as written.
//   • A message starting with "!" switches the prompt to shell mode and runs
//     as a command; one starting with "/" runs a slash command, and some open
//     a panel that keeps the keyboard afterwards. A leading space keeps either
//     plain text — messages meant as slash commands aren't sent this way,
//     but for the few that just run and give the prompt box back (isLiveCommand).
//   • A permission question (the session's own or a subagent's) or a panel
//     takes the prompt box's place and the keyboard: an Enter there answers
//     it. A subagent's question isn't in any state file, so the screen is the
//     only way to know — nothing is typed unless it shows the empty prompt box
//     with the cursor in it (readyForInput).
//
// Lifetime: each runs on its own once started (it holds itself) until it is
// over and `attach` has exited, and the returned handle may be dropped — keep
// a std::weak_ptr to cancel() it later. `done` runs once, on the plat loop;
// a caller that may be gone by then guards it (a shared alive flag). It runs
// at once, from inside the call, when nothing
// could be started (nothing to type, `attach` didn't start) and from cancel().
#pragma once

#include "app/claude/vt.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace claude {

class Pty;

// What AttachInput and AttachAnswer share: `attach` run in a hidden terminal
// whose screen is looked at a moment after output (settle), a deadline per
// phase (onLimit), and the way it ends (end). Not used on its own.
class AttachSession {
public:
    virtual ~AttachSession();
    AttachSession(const AttachSession &)            = delete;
    AttachSession &operator=(const AttachSession &) = delete;

protected:
    explicit AttachSession(plat::App &app);
    virtual void settle()  = 0; // look at the screen
    virtual void onLimit() = 0; // the current phase's deadline passed
    virtual void exited()  = 0; // `attach` ended on its own
    // Starts `attach` and its first deadline; false (nothing armed) if it didn't start.
    bool
    start(const std::string &program, const std::vector<std::string> &args, const std::string &cwd);
    void        look();           // settle() soon, output or not
    void        armLimit(int ms); // onLimit() then
    void        submit();         // Enter, with the submit deadline
    // How it ends: the deadline and the pending look stop, the terminal goes
    // (once `attach` has exited, or been made to; the session goes on),
    // `report` tells the caller, and the hold on itself goes once `attach`
    // is gone, or after a while.
    void        end(const std::function<void()> &report);
    std::string ptyError() const;

    plat::App                     &_app;
    std::shared_ptr<AttachSession> _self; // alive until over
    std::unique_ptr<Pty>           _pty;
    VtScreen                       _screen;
    uint64_t                       _quiet     = 0; // a look at the screen, soon after output
    uint64_t                       _limit     = 0; // the current phase's deadline
    bool                           _gotOutput = false;

private:
    void onOutput(std::string_view bytes);
    void release(); // drops the hold it has on itself (deferred)

    uint64_t _linger = 0; // `attach` given time to exit
};

class AttachInput : public AttachSession {
public:
    enum class Outcome : uint8_t {
        Sent,     // typed and taken: the prompt box let go of it after Enter
        NotReady, // nothing typed: the prompt box never showed up ready
        Failed,   // typing began but never got as far as Enter; it may be half there
        // Typed in full and Enter pressed, but the prompt box still seemed to
        // hold it when the time was up. Mid-turn Claude Code queues a prompt
        // and redraws the box as it likes: only the transcript can tell.
        Unconfirmed,
    };
    using Done = std::function<void(Outcome, std::string detail)>;

    // `program` + `args` run `claude attach <short>` (the caller's CLI path,
    // wrapped as it needs to be on Windows, see Launcher::attachCommand).
    static std::shared_ptr<AttachInput> send(
        plat::App                      &app,
        const std::string              &program,
        const std::vector<std::string> &args,
        const std::string              &cwd,
        const std::string              &text,
        Done                            done
    );
    ~AttachInput() override;

    // Give up before anything is typed: `done` gets NotReady ("cancelled") at
    // once. False when typing has begun — it then goes on to the end.
    bool cancel();

    // Whether `text` is a slash command typed as one: a command that runs and
    // hands the prompt box back, no panel kept on screen — "/compact [focus]".
    // Any other "/…" or "!…" message is typed as plain text.
    static bool                     isLiveCommand(std::string_view text);
    // What goes to the terminal for `text`, write by write (exposed for tests).
    static std::vector<std::string> keystrokes(std::string_view text);
    // How long the prompt box is waited for, and how long it may keep the
    // message after Enter (tests shorten them).
    static void                     setAttachTimeoutMs(int ms);
    static void                     setSubmitTimeoutMs(int ms);

private:
    AttachInput(plat::App &app, std::string text, Done done);
    void settle() override;
    void onLimit() override;
    void exited() override;
    void typeNext();
    void finish(Outcome outcome, std::string_view detail);

    enum class Phase : uint8_t { Attaching, Typing, Echoing, Submitting, Done };

    std::string              _text;
    Done                     _done;
    uint64_t                 _nothing   = 0; // "attach showed nothing"
    uint64_t                 _gap       = 0; // between writes
    bool                     _readySeen = false;
    Phase                    _phase     = Phase::Attaching;
    std::vector<std::string> _writes;
    size_t                   _nextWrite = 0;
    std::string              _echo; // the start of the message, as the prompt box shows it
};

// Answers a background session's permission question the way a person at its
// terminal would: through `claude attach`, picking one of the numbered options
// Claude Code shows (see PermissionQuestion). Which options there are is only
// on the screen — the job's state says "approve Bash: …" and no more — so the
// question is read first (choose with number 0), and an option is picked by
// its number and label. Nothing is pressed unless the screen shows that question
// with that option: "❯" is moved onto it with the arrow keys, and Enter goes
// only once "❯" is seen there — never a key that could pick something else.
// Lifetime as AttachInput's.
class AttachAnswer : public AttachSession {
public:
    enum class Outcome : uint8_t {
        Done,     // number 0: the question is passed on; else answered, the question went
        NotReady, // nothing answered: no such question on screen, or `attach` failed
        Failed,   // Enter was pressed but the question stayed
    };
    // Whether the question on screen is the one meant (the job's `needs`).
    using Match = std::function<bool(const PermissionQuestion &)>;
    using Result =
        std::function<void(Outcome, std::optional<PermissionQuestion>, std::string detail)>;

    // Option `number` ("label" on screen) picked; number 0 = nothing pressed,
    // the question only read.
    static std::shared_ptr<AttachAnswer> choose(
        plat::App                      &app,
        const std::string              &program,
        const std::vector<std::string> &args,
        const std::string              &cwd,
        Match                           match,
        int                             number,
        const std::string              &label,
        Result                          done
    );
    ~AttachAnswer() override;

private:
    AttachAnswer(plat::App &app, Match match, int number, std::string label, Result done);
    void settle() override;
    void onLimit() override;
    void exited() override;
    void step(); // one arrow key towards the option
    void finish(Outcome outcome, std::string_view detail);

    enum class Phase : uint8_t { Reading, Moving, Submitting, Done };

    Match                             _match;
    int                               _number = 0; // 0: read only
    std::string                       _label;
    Result                            _done;
    uint64_t                          _step      = 0;
    int                               _stepsLeft = 0;
    std::string                       _key; // ↓ or ↑
    Phase                             _phase = Phase::Reading;
    std::optional<PermissionQuestion> _seen; // the last look's, to see it twice alike
};

// Whether the permission question on screen is the one a background job's
// `needs` ("approve Bash: rm -rf build", "approve Entering worktree") names.
// The screen shows the command wrapped and framed ("│ rm -rf …"), so both are
// compared without spaces or frames — the command's start, or for a file
// (Edit, Write) its name, which may be shown relative to the folder.
bool questionIsFor(std::string_view needs, const PermissionQuestion &q);

} // namespace claude
