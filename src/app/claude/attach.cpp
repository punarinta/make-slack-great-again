#include "app/claude/attach.h"

#include "app/claude/pty.h"
#include "base/file.h"
#include "base/str.h"
#include "base/utf8.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace claude {

namespace {

// The terminal msga attaches with: wide, so a long first line of the message
// still starts on the prompt box's first row.
constexpr int kRows = 50;
constexpr int kCols = 200;

// Pasted a few hundred characters at a time, a paste stays plain typing (see
// the header); 400 is well under where it was seen turn into <pasted_content>.
// Counted in UTF-16 units.
constexpr size_t kChunk = 400;

constexpr int kLookMs     = 300;    // how often the screen is looked at while it changes
int           attachMs    = 15'000; // attaching took ~2 s when tried
constexpr int kEchoMs     = 5'000;
int           submitMs    = 10'000;
constexpr int kWriteGapMs = 15; // between pastes, so each is one of its own
constexpr int kLingerMs   = 5'000;

constexpr std::string_view kPasteStart = "\x1b[200~";
constexpr std::string_view kPasteEnd   = "\x1b[201~";

// A one-shot timer kept in `slot` (0 = none), cleared as it fires.
void disarm(plat::App &app, uint64_t &slot) {
    if (slot)
        app.cancelTimer(slot);
    slot = 0;
}

void arm(plat::App &app, uint64_t &slot, int ms, std::function<void()> fn) {
    disarm(app, slot);
    uint64_t *s = &slot; // the owner cancels its timers before it goes
    slot        = app.addTimer(ms, false, [s, fn = std::move(fn)] {
        *s = 0;
        fn();
    });
}

std::string plainLines(std::string_view in) {
    std::string text;
    text.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\r') {
            text.push_back('\n');
            if (i + 1 < in.size() && in[i + 1] == '\n')
                ++i; // CRLF: one newline
            continue;
        }
        // Control characters would be keys (Esc, Ctrl+C…): only tabs stay.
        const auto b = static_cast<unsigned char>(c);
        if ((b >= 0x20 && b != 0x7F) || c == '\n' || c == '\t')
            text.push_back(c);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    size_t lead = 0;
    while (lead < text.size() && text[lead] == '\n')
        ++lead;
    text.erase(0, lead);
    // A command meant to run that way starts the line; any other "!…" (a
    // shell command) or "/…" (a slash command) is kept text by a space.
    if (AttachInput::isLiveCommand(text))
        return std::string(str::trimSpace(text));
    if (!text.empty() && (text[0] == '!' || text[0] == '/'))
        text.insert(text.begin(), ' ');
    return text;
}

// The byte length of `s`'s longest start of at most `units` UTF-16 units —
// never a character cut in two.
size_t prefixUnits(std::string_view s, size_t units) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        size_t       j  = i;
        const size_t cu = utf8::decode(s, j) >= 0x10000 ? 2 : 1;
        if (n + cu > units)
            break;
        n += cu;
        i = j;
    }
    return i;
}

} // namespace

bool AttachInput::isLiveCommand(std::string_view text) {
    text = str::trimSpace(text);
    // One line: a second one would be typed after the command's name, into
    // whatever the command leaves on screen.
    if (text.find_first_of("\r\n") != std::string_view::npos)
        return false;
    const size_t           end  = text.find_first_of(" \t");
    const std::string_view name = text.substr(0, end);
    return name == "/compact";
}

std::vector<std::string> AttachInput::keystrokes(std::string_view text) {
    std::vector<std::string> out;
    const std::string        plain = plainLines(text);
    const auto               lines = str::split(plain, '\n');
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        while (!line.empty()) {
            size_t n = prefixUnits(line, kChunk);
            if (n == 0)
                n = utf8::nextBoundary(line, 0);
            std::string key(kPasteStart);
            key.append(line.substr(0, n)).append(kPasteEnd);
            out.push_back(std::move(key));
            line.remove_prefix(n);
        }
        if (i + 1 < lines.size())
            out.emplace_back("\n"); // a new line in the prompt, not Enter
    }
    return out;
}

void AttachInput::setAttachTimeoutMs(int ms) {
    attachMs = ms;
}

void AttachInput::setSubmitTimeoutMs(int ms) {
    submitMs = ms;
}

// ── AttachSession ───────────────────────────────────────────────────────────

AttachSession::AttachSession(plat::App &app)
    : _app(app), _pty(std::make_unique<Pty>(app)), _screen(kRows, kCols) {
    _pty->onOutput   = [this](std::string_view bytes) { onOutput(bytes); };
    _pty->onFinished = [this] { exited(); };
}

AttachSession::~AttachSession() {
    disarm(_app, _quiet);
    disarm(_app, _limit);
    disarm(_app, _linger);
}

bool AttachSession::start(
    const std::string &program, const std::vector<std::string> &args, const std::string &cwd
) {
    if (!_pty->start(program, args, cwd, kRows, kCols))
        return false;
    armLimit(attachMs);
    return true;
}

std::string AttachSession::ptyError() const {
    return _pty->errorString();
}

void AttachSession::onOutput(std::string_view bytes) {
    _gotOutput = true;
    _screen.feed(bytes);
    // Looked at a moment after output starts, not once it stops: a turn's
    // spinner redraws for as long as the turn runs.
    if (!_quiet)
        look();
}

void AttachSession::look() {
    arm(_app, _quiet, kLookMs, [this] { settle(); });
}

void AttachSession::armLimit(int ms) {
    arm(_app, _limit, ms, [this] { onLimit(); });
}

void AttachSession::submit() {
    armLimit(submitMs);
    _pty->write("\r");
}

void AttachSession::end(const std::function<void()> &report) {
    disarm(_app, _quiet);
    disarm(_app, _limit);
    // The terminal goes once `attach` has exited (or been made to); the
    // session goes on.
    _pty->onOutput   = nullptr;
    _pty->onFinished = [this] { release(); };
    _pty->terminate();
    report();
    // Released now if `attach` is already gone, else when it exits or after
    // kLingerMs.
    if (!_pty->isRunning())
        release();
    else
        arm(_app, _linger, kLingerMs, [this] { release(); });
}

// The last hold goes on the loop's next turn, not in mid-call.
void AttachSession::release() {
    disarm(_app, _linger);
    if (_self)
        _app.post([self = std::move(_self)] {});
}

// ── AttachInput ─────────────────────────────────────────────────────────────

std::shared_ptr<AttachInput> AttachInput::send(
    plat::App                      &app,
    const std::string              &program,
    const std::vector<std::string> &args,
    const std::string              &cwd,
    const std::string              &text,
    Done                            done
) {
    std::shared_ptr<AttachInput> self(new AttachInput(app, text, std::move(done)));
    self->_self = self;
    if (plainLines(text).empty()) {
        self->finish(Outcome::NotReady, "nothing to type");
        return self;
    }
    if (!self->start(program, args, cwd)) {
        self->finish(Outcome::NotReady, self->ptyError());
        return self;
    }
    AttachInput *p = self.get();
    // `attach` draws within a moment ("Attaching…"); a terminal that shows
    // nothing at all isn't going to (a pseudo-terminal not wired up).
    arm(app, p->_nothing, std::min(attachMs, 5000), [p] {
        if (!p->_gotOutput && p->_phase == Phase::Attaching)
            p->finish(Outcome::NotReady, "claude attach showed nothing");
    });
    return self;
}

bool AttachInput::cancel() {
    if (_phase == Phase::Done)
        return true;
    if (_phase != Phase::Attaching)
        return false;
    finish(Outcome::NotReady, "cancelled");
    return true;
}

AttachInput::AttachInput(plat::App &app, std::string text, Done done)
    : AttachSession(app), _text(std::move(text)), _done(std::move(done)) {
    const std::string plain = plainLines(_text);
    const auto        first = str::split(plain, '\n').front();
    _echo                   = std::string(str::trimSpace(first.substr(0, prefixUnits(first, 24))));
    _writes                 = keystrokes(_text);
}

AttachInput::~AttachInput() {
    disarm(_app, _nothing);
    disarm(_app, _gap);
}

void AttachInput::exited() {
    // `attach` ended on its own: no such session, or it went away.
    finish(
        _phase == Phase::Attaching ? Outcome::NotReady : Outcome::Failed, "claude attach exited"
    );
}

void AttachInput::onLimit() {
    switch (_phase) {
    case Phase::Attaching:
        finish(Outcome::NotReady, "the prompt box never showed up ready");
        break;
    case Phase::Echoing:
        finish(Outcome::Failed, "the message didn't show up in the prompt box");
        break;
    case Phase::Submitting:
        // Not a failure: Enter went to the box with the whole message in
        // it. A prompt typed mid-turn (Claude Code 2.1.283) was queued and
        // answered though this deadline passed — the transcript is what
        // tells whether it came.
        finish(Outcome::Unconfirmed, "the prompt box kept the message after Enter");
        break;
    default:
        break;
    }
}

void AttachInput::settle() {
    switch (_phase) {
    case Phase::Attaching:
        // Ready twice running, a look apart — never on a frame half drawn.
        if (!readyForInput(_screen)) {
            _readySeen = false;
            return; // not yet (still drawing, or a question on screen): the deadline decides
        }
        if (!std::exchange(_readySeen, true)) {
            look(); // the second look, output or not
            return;
        }
        _phase = Phase::Typing;
        disarm(_app, _limit);
        typeNext();
        break;
    case Phase::Echoing: {
        const auto box = findPromptBox(_screen);
        if (!box || box->lines.empty() ||
            !str::startsWith(str::trimSpace(box->lines.front()), _echo))
            return;
        _phase = Phase::Submitting;
        submit();
        break;
    }
    case Phase::Submitting: {
        // Taken once the box no longer shows it: empty again, gone, or holding
        // something else — the Enter went to the box (the message was in it),
        // and what replaced it came after: say a permission question for the
        // turn it started, or whatever the box shows while the prompt is queued.
        const auto box = findPromptBox(_screen);
        if (!box || box->empty || box->lines.empty() ||
            !str::startsWith(str::trimSpace(box->lines.front()), _echo))
            finish(Outcome::Sent, {});
        break;
    }
    default:
        break;
    }
}

void AttachInput::typeNext() {
    if (_phase != Phase::Typing)
        return;
    if (_nextWrite >= _writes.size()) {
        _phase = Phase::Echoing;
        armLimit(kEchoMs);
        look(); // looked at even if typing drew nothing more
        return;
    }
    _pty->write(_writes[_nextWrite++]);
    arm(_app, _gap, kWriteGapMs, [this] { typeNext(); });
}

void AttachInput::finish(Outcome outcome, std::string_view detail) {
    if (_phase == Phase::Done)
        return;
    _phase = Phase::Done;
    disarm(_app, _nothing);
    disarm(_app, _gap);
    end([&] {
        if (auto done = std::exchange(_done, {}))
            done(outcome, std::string(detail));
    });
}

// ── AttachAnswer ────────────────────────────────────────────────────────────

namespace {

constexpr int kMoveMs = 5'000;
constexpr int kStepMs = 50; // between arrow keys

bool sameOptions(const PermissionQuestion &a, const PermissionQuestion &b) {
    if (a.selected != b.selected || a.options.size() != b.options.size())
        return false;
    for (size_t i = 0; i < a.options.size(); ++i)
        if (a.options[i].label != b.options[i].label)
            return false;
    return true;
}

} // namespace

std::shared_ptr<AttachAnswer> AttachAnswer::choose(
    plat::App                      &app,
    const std::string              &program,
    const std::vector<std::string> &args,
    const std::string              &cwd,
    Match                           match,
    int                             number,
    const std::string              &label,
    Result                          done
) {
    std::shared_ptr<AttachAnswer> self(
        new AttachAnswer(app, std::move(match), number, label, std::move(done))
    );
    self->_self = self;
    if (!self->start(program, args, cwd))
        self->finish(Outcome::NotReady, self->ptyError());
    return self;
}

AttachAnswer::AttachAnswer(plat::App &app, Match match, int number, std::string label, Result done)
    : AttachSession(app), _match(std::move(match)), _number(number), _label(std::move(label)),
      _done(std::move(done)) {}

AttachAnswer::~AttachAnswer() {
    disarm(_app, _step);
}

void AttachAnswer::exited() {
    finish(
        _phase == Phase::Submitting ? Outcome::Failed : Outcome::NotReady, "claude attach exited"
    );
}

void AttachAnswer::onLimit() {
    switch (_phase) {
    case Phase::Reading:
        finish(Outcome::NotReady, "the question isn't on Claude Code's screen");
        break;
    case Phase::Moving:
        finish(Outcome::NotReady, "the choice didn't move to the option");
        break;
    case Phase::Submitting:
        finish(Outcome::Failed, "the question stayed after Enter");
        break;
    default:
        break;
    }
}

void AttachAnswer::step() {
    if (_phase != Phase::Moving || _stepsLeft <= 0)
        return;
    --_stepsLeft;
    _pty->write(_key);
    if (_stepsLeft > 0)
        arm(_app, _step, kStepMs, [this] { step(); });
}

void AttachAnswer::settle() {
    auto q = findPermissionQuestion(_screen);
    if (q && !_match(*q))
        q.reset(); // a question, but another one (a subagent's, say)
    switch (_phase) {
    case Phase::Reading: {
        // Seen twice alike, a look apart — never a frame half drawn.
        const bool twice = q && _seen && sameOptions(*q, *_seen);
        _seen            = q;
        if (!q)
            return; // not (yet): the deadline decides
        if (!twice) {
            look();
            return;
        }
        if (!_number) {
            finish(Outcome::Done, {});
            return;
        }
        if (_number > int(q->options.size()) || q->options[size_t(_number - 1)].label != _label) {
            finish(Outcome::NotReady, "the question has other options now");
            return;
        }
        _phase = Phase::Moving;
        armLimit(kMoveMs);
        const int steps = _number - q->selected;
        _key            = steps > 0 ? "\x1b[B" : "\x1b[A"; // ↓ / ↑
        _stepsLeft      = std::abs(steps);
        if (_stepsLeft > 0)
            arm(_app, _step, 0, [this] { step(); });
        _seen.reset();
        look(); // looked at even when nothing needed moving
        break;
    }
    case Phase::Moving: {
        // Enter only with "❯" seen on the option twice running.
        const bool on    = q && q->selected == _number;
        const bool twice = on && _seen;
        _seen            = on ? q : std::nullopt;
        if (!on)
            return;
        if (!twice) {
            look();
            return;
        }
        _phase = Phase::Submitting;
        submit();
        break;
    }
    case Phase::Submitting:
        if (!q)
            finish(Outcome::Done, {});
        break;
    default:
        break;
    }
}

void AttachAnswer::finish(Outcome outcome, std::string_view detail) {
    if (_phase == Phase::Done)
        return;
    const bool read = _phase == Phase::Reading;
    _phase          = Phase::Done;
    disarm(_app, _step);
    end([&] {
        if (auto done = std::exchange(_done, {}))
            done(
                outcome,
                read && outcome == Outcome::Done ? _seen : std::nullopt,
                std::string(detail)
            );
    });
}

// ── Matching a question to a job's needs ────────────────────────────────────

namespace {

// Without white space and the frame Claude Code draws around a command (│).
std::string squash(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const size_t   at = i;
        const uint32_t c  = utf8::decode(s, i);
        if (!utf8::isSpace(c) && c != 0x2502)
            out.append(s.substr(at, i - at));
    }
    return out;
}

bool contains(std::string_view s, std::string_view part) {
    return s.find(part) != std::string_view::npos;
}

} // namespace

bool questionIsFor(std::string_view needs, const PermissionQuestion &q) {
    constexpr std::string_view kApprove = "approve ";
    const std::string_view     detail   = needs.substr(std::min(needs.size(), kApprove.size()));
    const size_t               colon    = detail.find(": ");
    const std::string_view     tool     = detail.substr(0, colon);
    const std::string_view     arg      = colon == std::string_view::npos
                                              ? std::string_view()
                                              : str::trimSpace(detail.substr(colon + 2));
    const std::string          text     = squash(q.text);
    if (arg.empty()) {
        // Only the tool's label, which the question needn't repeat: "Entering
        // worktree" asks "Enter the worktree at …?" (verified 2.1.283). A word
        // of it will do.
        if (tool.empty())
            return false;
        if (contains(text, squash(tool)))
            return true;
        for (std::string_view rest = tool; !rest.empty();) {
            const size_t           sp   = rest.find(' ');
            const std::string_view word = rest.substr(0, sp);
            rest = sp == std::string_view::npos ? std::string_view() : rest.substr(sp + 1);
            if (utf8::countCodePoints(word) >= 4 && utf8::containsFolded(text, word))
                return true;
        }
        return false;
    }
    const std::string sq = squash(arg); // its first 40 characters
    if (contains(text, std::string_view(sq).substr(0, utf8::prefixBytes(sq, 40))))
        return true;
    const std::string_view name = file::baseName(arg);
    return file::isAbsolute(arg) && !name.empty() && contains(text, squash(name));
}

} // namespace claude
