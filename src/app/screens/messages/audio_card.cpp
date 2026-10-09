#include "app/screens/messages/audio_card.h"

#include "app/llm/audio_transcriber.h"
#include "app/llm/service.h"
#include "app/media/audio_player.h"
#include "app/model/jobs.h"
#include "app/screens/common/downloads.h"
#include "app/screens/common/message_rules.h"
#include "app/screens/common/remote_images.h"
#include "app/screens/messages/message_list.h"
#include "app/screens/messages/rich.h"
#include "app/screens/messages/rows.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace screens {

using i18n::arg;
using i18n::tr;
using media::AudioPlayer;
using ui::C;

namespace {

// The audio card: round play button + title block on top, slider row
// underneath.
constexpr float kPad      = 12;
constexpr float kBtn      = 36; // play/pause circle
constexpr float kKnob     = 12;
constexpr float kBarH     = 4;
constexpr float kRadius   = 8;
constexpr float kAction   = 28; // "Transcribe" button (square hit area, round hover)
constexpr float kLabelGap = 6;  // time label → action button
constexpr float kGlyph    = 16;
constexpr float kBodyMaxH = 360; // the transcript dialog's text

// A line box's height for a face this size, as a multiple of the size.
constexpr float kLine = 1.2f;

text::Style nameFont() {
    return ui::pxFont(15, text::Weight::Bold, ui::color(C::FormText));
}
text::Style subFont(C c = C::FormTextMuted) {
    return ui::pxFont(15 * 0.82f, text::Weight::Regular, ui::color(c));
}
text::Style lineFont(C c) {
    return ui::pxFont(15, text::Weight::Regular, ui::color(c));
}

size_t gLayoutBuilds = 0; // audioCardLayoutBuilds()

std::unique_ptr<text::Layout>
oneLine(std::string_view s, const text::Style &st, float maxW, float scale) {
    ++gLayoutBuilds;
    text::AttributedText t;
    t.append(s, st);
    text::LayoutOptions o;
    o.maxLines   = 1;
    o.ellipsis   = true;
    o.maxWidth   = std::max(1.f, maxW);
    o.lineHeight = kLine;
    return text::Layout::build(std::move(t), o, scale);
}

// One line kept between paints: shaped again only when its text, width,
// scale or colour (`tag`) changes — the card repaints 5×/s while it plays.
class LineCache {
public:
    const text::Layout &
    get(std::string_view s, const text::Style &style, float maxW, float scale, int tag = 0) {
        maxW = std::max(1.f, maxW);
        if (!_l || s != _s || maxW != _w || scale != _scale || tag != _tag) {
            _l = oneLine(s, style, maxW, scale);
            _s.assign(s);
            _w     = maxW;
            _scale = scale;
            _tag   = tag;
        }
        return *_l;
    }
    void reset() { _l.reset(); }

private:
    std::unique_ptr<text::Layout> _l;
    std::string                   _s;
    float                         _w = 0, _scale = 0;
    int                           _tag = 0;
};

bool live(AudioPlayer::State s) {
    return s == AudioPlayer::State::Playing || s == AudioPlayer::State::Paused ||
           s == AudioPlayer::State::Ended;
}

// "Mira at 2:34 PM" (the transcript subtitle), "· transcribed by X" added
// for an AI transcript.
std::string subtitleFor(Context &ctx, const model::Message *m, const std::string &by) {
    std::string s;
    if (m) {
        const std::string who(authorName(ctx.store(), *m));
        const std::string when = base::formatTime(model::tsSecs(m->ts));
        s                      = who.empty() ? when : arg(tr("%1 at %2"), who, when);
    }
    if (!by.empty())
        s = arg(tr("%1 \xC2\xB7 transcribed by %2"), s, by);
    return s;
}

// ── The transcript dialog ───────────────────────────────────────────────────
// "Transcript (auto-generated)", the subtitle, the
// timestamped lines (scrolling past 360 px), Copy. Opens in the Loading
// shape; setCues / setText / setFailed fill it.
class TranscriptDialog {
public:
    TranscriptDialog(Context &ctx, const std::string &subtitle) : _ctx(ctx) {
        auto d = std::make_unique<ui::Dialog>(
            tr("Transcript (auto-generated)"), 0, ui::Dialog::Scroll::Disabled
        );
        _dialog = d.get();
        ui::styledLabel(d->content(), subtitle, lineFont(C::FormTextMuted))
            ->style()
            .margins(0, 0, 0, ui::metric(ui::M::SpaceM));
        _scroll               = d->content()->add<ui::ScrollView>();
        _scroll->style().maxH = kBodyMaxH;
        _scroll->content()->style().spacing(6);
        _copy = ui::Dialog::makeButton(tr("Copy"), ui::Button::Kind::Primary);
        _copy->setEnabled(false);
        auto  alive    = _alive;
        auto *copy     = _copy;
        _copy->onClick = [this, copy, alive] {
            _ctx.app.platform().setClipboardText(_plain);
            flashCopied(_ctx, copy, alive);
        };
        d->addButtonRow(_copy, nullptr);
        setBody({}, tr("Loading\xE2\x80\xA6"));
        if (ctx.window)
            ctx.window->showPopup(std::move(d));
        else {
            // _scroll/_copy point into `d`'s tree and stay in use after
            // this returns -- keep `d` alive instead of letting it free
            // out from under them.
            _dialog     = nullptr;
            _standalone = std::move(d);
        }
    }

    void setCues(const std::vector<VttCue> &cues) {
        _plain.clear();
        for (const VttCue &c : cues) {
            const std::string t = formatDuration(c.startMs);
            _plain += (_plain.empty() ? "" : "\n") + t + "  " + c.text;
        }
        setBody(cues, {});
        _copy->setEnabled(!cues.empty());
    }
    void setText(const std::string &text) { setCues({VttCue{0, text}}); }
    void setFailed(const std::string &error) {
        setBody({}, error);
        _copy->setEnabled(false);
    }
    std::weak_ptr<char> alive() const { return _alive; }

    // The dialog owns this: it lives exactly as long as the card is open.
    static TranscriptDialog *open(Context &ctx, const std::string &subtitle);

private:
    void setBody(const std::vector<VttCue> &cues, const std::string &plain) {
        ui::View *body = _scroll->content();
        body->clearChildren();
        if (cues.empty()) {
            ui::styledLabel(body, plain, lineFont(C::FormText));
            return;
        }
        for (const VttCue &c : cues) {
            text::AttributedText t;
            t.append(formatDuration(c.startMs), lineFont(C::FormTextMuted));
            t.append("\xC2\xA0\xC2\xA0" + c.text, lineFont(C::FormText));
            body->add<ui::Label>()->setRichText(std::move(t));
        }
    }

    Context              &_ctx;
    ui::Dialog           *_dialog = nullptr;
    ui::ScrollView       *_scroll = nullptr;
    ui::Button           *_copy   = nullptr;
    std::unique_ptr<ui::Dialog> _standalone; // set only without a window
    std::string           _plain; // what Copy puts on the clipboard
    std::shared_ptr<char> _alive = std::make_shared<char>(0);

public:
    // Owned by the dialog view (dies with it).
    struct Holder : ui::View {
        std::unique_ptr<TranscriptDialog> d;
        Holder() { setVisible(false); }
    };
};

TranscriptDialog *TranscriptDialog::open(Context &ctx, const std::string &subtitle) {
    auto *td = new TranscriptDialog(ctx, subtitle);
    if (!td->_dialog) { // no window (tests without one)
        static std::unique_ptr<TranscriptDialog> orphan;
        orphan.reset(td);
        return td;
    }
    auto *h = td->_dialog->content()->add<Holder>();
    h->d.reset(td);
    return td;
}

// ── The card ────────────────────────────────────────────────────────────────

class AudioCard;

// The round play/pause button (a child, so the tour and accessibility find it).
class PlayButton final : public ui::Clickable {
public:
    explicit PlayButton(AudioCard &card) : _card(card) {
        setLook({C::None, C::None, C::None, C::None, 0});
    }
    void        paint(gfx::Painter &p) override;
    std::string accessibleName() const override;
    void        activate() override;

private:
    AudioCard &_card;
};

// "Transcribe with AI": muted glyph, accent on hover / while the AI works.
class TranscribeButton final : public ui::Clickable {
public:
    explicit TranscribeButton(AudioCard &card) : _card(card) {
        setLook({C::None, C::None, C::None, C::None, 0});
        setTooltip(tr("Transcribe with AI"));
        setHoverRepaint(true);
    }
    bool tooltipImmediate() const override { return true; }
    void paint(gfx::Painter &p) override;
    void activate() override;

private:
    AudioCard &_card;
};

// "View transcript" after the quoted line.
class TranscriptLink final : public ui::Clickable {
public:
    explicit TranscriptLink(AudioCard &card) : _card(card) {
        setLook({C::None, C::None, C::None, C::None, 0});
    }
    ui::SizeF measureContent(float, float) override {
        const float k = windowScale();
        if (k != _measuredAt) {
            _measuredAt = k;
            _labelW     = std::ceil(text::measure(label(), lineFont(C::Link), k));
        }
        return {_labelW, 15 * kLine};
    }
    void paint(gfx::Painter &p) override {
        const text::Layout &l = _line.get(label(), lineFont(C::Link), 1e9f, windowScale());
        l.paint(p, snapPx({0, std::floor((height() - l.height()) / 2)}));
    }
    void styleChanged() override {
        _line.reset();
        _measuredAt = 0;
        Clickable::styleChanged();
    }
    std::string accessibleName() const override { return label(); }
    void        activate() override;

private:
    static std::string label() { return tr("View transcript"); }
    AudioCard         &_card;
    LineCache          _line;
    float              _measuredAt = 0, _labelW = 0;
};

class AudioCard final : public ui::Clickable {
public:
    AudioCard(Context &ctx, MessageList *list, Ts ts, const model::File &f)
        : _ctx(ctx), _list(list), _ts(ts), _file(f) {
        setLook({C::None, C::None, C::None, C::None, kRadius});
        // A transcript the user's AI produced replaces Slack's line (and its
        // cues, which describe Slack's words).
        if (const auto *ai = ctx.store().aiTranscript(f.id)) {
            _file.transcript = ai->text;
            _file.transcriptVtt.clear();
            _by = ai->by;
        }
        _simpleTranscript = str::simplified(_file.transcript);
        _play             = add<PlayButton>(*this);
        _transcribe       = add<TranscribeButton>(*this);
        if (hasTranscript())
            _link = add<TranscriptLink>(*this);
        if (ctx.audio)
            _audioObs = ctx.audio->observe([this](const std::string &key) {
                if (key == _file.id)
                    update();
            });
        if (ctx.ai)
            _aiObs = ctx.ai->transcriber().observe([this](const std::string &key) {
                if (key == _file.id)
                    update();
            });
    }
    ~AudioCard() override {
        if (_ctx.audio)
            _ctx.audio->unobserve(_audioObs);
        if (_ctx.ai)
            _ctx.ai->transcriber().unobserve(_aiObs);
    }

    bool                  hasTranscript() const { return !_file.transcript.empty(); }
    const model::File    &file() const { return _file; }
    const std::string    &by() const { return _by; }
    Context              &ctx() { return _ctx; }
    const model::Message *message() const { return _list && _ts ? _list->message(_ts) : nullptr; }
    bool                  playing() const {
        return _ctx.audio && _ctx.audio->isCurrent(_file.id) &&
               _ctx.audio->status().state == AudioPlayer::State::Playing;
    }
    bool transcribing() const { return _ctx.ai && _ctx.ai->transcriber().inFlight(_file.id); }
    std::string accessibleName() const override { return _file.name; }

    ui::SizeF measureContent(float aw, float) override {
        float w = kAudioCardMaxW;
        if (aw > 0 && aw < w)
            w = aw;
        return {w, kAudioCardH + (hasTranscript() ? kAudioTranscriptH : 0)};
    }
    void layout() override {
        _play->setFrame({kPad, kPad, kBtn, kBtn});
        _transcribe->setFrame(actionRect());
        if (_link) {
            const ui::RectF tl = transcriptText();
            const ui::SizeF ls = _link->measure(ui::kInf, ui::kInf);
            _link->setFrame({tl.x + tl.w + 6, tl.y, ls.w, tl.h});
        }
    }

    void activate() override { toggleAudio(_ctx, _file); }

    bool onEvent(ui::Event &e) override {
        using ui::EventType;
        if (_list && _ts &&
            (e.type == EventType::PointerEnter || e.type == EventType::PointerLeave))
            _list->fileHovered(this, _ts, _file.path, e.type == EventType::PointerEnter);
        if (e.type == EventType::PointerDown && e.button == plat::Button::Left) {
            // The transcript line: only its link acts.
            if (e.pos.y > kAudioCardH)
                return true;
            // The seek bar, once the player holds this file with a known
            // length: a press starts a scrub, the release seeks.
            const AudioPlayer::Status *st = status();
            if (st && st->durationMs > 0 && live(st->state)) {
                const ui::RectF bar = barRect(st->durationMs);
                if (e.pos.x >= bar.x - 2 && e.pos.x <= bar.x + bar.w + 2 && e.pos.y >= bar.y - 8 &&
                    e.pos.y <= bar.y + bar.h + 8) {
                    _scrubbing = true;
                    scrubTo(e.pos.x, *st);
                    return true;
                }
            }
        }
        if (_scrubbing && e.type == EventType::PointerMove) {
            if (const AudioPlayer::Status *st = status())
                scrubTo(e.pos.x, *st);
            return true;
        }
        if (_scrubbing && (e.type == EventType::PointerUp || e.type == EventType::PointerCancel)) {
            _scrubbing = false;
            if (_ctx.audio && _ctx.audio->isCurrent(_file.id) && _scrubMs >= 0 &&
                e.type == EventType::PointerUp)
                _ctx.audio->seek(_scrubMs);
            _scrubMs = -1;
            update();
            return true;
        }
        return Clickable::onEvent(e);
    }

    void styleChanged() override {
        // Theme colours and text size are baked into the cached lines.
        _name.reset();
        _sub.reset();
        _time.reset();
        _transcriptLine.reset();
        _barAt = _textAt = 0;
        Clickable::styleChanged();
    }

    void paint(gfx::Painter &p) override {
        const float                k  = windowScale();
        const AudioPlayer::Status *st = status();
        const AudioPlayer::State   ph = st ? st->state : AudioPlayer::State::Idle;
        const int64_t   dur = st && st->durationMs > 0 ? st->durationMs : _file.durationMs;
        const ui::RectF chip{0, 0, width(), kAudioCardH};

        // Card
        paintCardFrame(p, chip, kRadius);

        // Title block: name, then "0:05 (79 KB)" / Loading… / the error.
        const float         textX = kPad + kBtn + kPad;
        const float         textW = chip.w - textX - kPad;
        const text::Layout &name  = _name.get(_file.name, nameFont(), textW, k);
        std::string         sub;
        C                   subColor = C::FormTextMuted;
        if (ph == AudioPlayer::State::Error) {
            sub      = st->error;
            subColor = C::FormError;
        } else if (ph == AudioPlayer::State::Loading) {
            sub = tr("Loading\xE2\x80\xA6");
        } else {
            const std::string sz = str::byteSize(_file.size, str::ByteSize::File);
            if (dur > 0)
                sub = formatDuration(dur, true) + (sz.empty() ? "" : " (" + sz + ")");
            else
                sub = sz.empty() ? _file.prettyType : sz;
        }
        name.paint(p, snapPx({textX, kPad - 1}));
        if (!sub.empty())
            _sub.get(sub, subFont(subColor), textW, k, int(subColor))
                .paint(p, snapPx({textX, kPad - 1 + std::ceil(name.height()) + 2}));

        // Slider: track, played part, knob.
        const bool isLive = live(ph) || _scrubMs >= 0;
        int64_t    pos    = _scrubMs >= 0 ? _scrubMs : (st ? st->positionMs : 0);
        if (ph == AudioPlayer::State::Ended && _scrubMs < 0)
            pos = dur;
        if (!isLive)
            pos = 0;
        const float     frac = dur > 0 ? std::clamp(float(pos) / float(dur), 0.f, 1.f) : 0.f;
        const ui::RectF bar  = barRect(dur);
        p.fillRoundRect(bar, kBarH / 2, ui::color(C::FileChipBorder));
        const float knobX = bar.x + bar.w * frac;
        if (frac > 0)
            p.fillRoundRect({bar.x, bar.y, knobX - bar.x, bar.h}, kBarH / 2, ui::color(C::Accent));
        p.fillCircle(
            {knobX, bar.y + bar.h / 2 + 0.5f},
            kKnob / 2,
            ui::color(isLive ? C::Accent : C::FormTextMuted)
        );

        // Time: elapsed while live, the clip's length otherwise.
        std::string label;
        if (ph == AudioPlayer::State::Ended && _scrubMs < 0)
            label = dur > 0 ? formatDuration(dur, true) : std::string();
        else if (isLive)
            label = formatDuration(pos);
        else if (dur > 0)
            label = formatDuration(dur, true);
        if (!label.empty()) {
            const ui::RectF     action = actionRect();
            const text::Layout &l      = _time.get(label, subFont(), 1e9f, k);
            const float         right  = action.x - kLabelGap;
            p.save();
            p.clipRect({bar.x + bar.w + 1, 0, std::max(0.f, right - bar.x - bar.w - 1), chip.h});
            l.paint(
                p,
                snapPx(
                    {right - std::ceil(l.width()), bar.y + bar.h / 2 - std::floor(l.height() / 2)}
                )
            );
            p.restore();
        }

        // The transcript line under the card.
        if (hasTranscript()) {
            const ui::RectF tl = transcriptText();
            p.fillRoundRect({0, tl.y, 3, tl.h}, 1.5f, ui::color(C::FormDivider));
            if (tl.w > 0)
                _transcriptLine.get(_simpleTranscript, lineFont(C::FormTextMuted), tl.w, k)
                    .paint(p, snapPx({tl.x, tl.y}));
        }
    }

    // The round play button's centre glyph.
    void paintPlay(gfx::Painter &p, ui::RectF r) const {
        const bool on = playing();
        p.fillCircle({r.x + r.w / 2, r.y + r.h / 2}, r.w / 2, ui::color(C::AccentSubtle));
        const float gx = std::floor(r.x + (r.w - kGlyph) / 2) + (on ? 0 : 1);
        const float gy = std::floor(r.y + (r.h - kGlyph) / 2);
        gfx::drawIcon(
            p,
            on ? gfx::Icon::Pause : gfx::Icon::Play,
            {gx, gy, kGlyph, kGlyph},
            ui::color(C::Accent)
        );
    }

private:
    const AudioPlayer::Status *status() const {
        if (!_ctx.audio || !_ctx.audio->isCurrent(_file.id) ||
            _ctx.audio->status().state == AudioPlayer::State::Idle)
            return nullptr;
        return &_ctx.audio->status();
    }
    void scrubTo(float x, const AudioPlayer::Status &st) {
        const ui::RectF bar  = barRect(st.durationMs);
        const float     frac = std::clamp((x - bar.x) / std::max(1.f, bar.w), 0.f, 1.f);
        _scrubMs             = std::llround(double(frac) * double(st.durationMs));
        update();
    }
    // Vertical centre of the slider row: centred in the band under the title.
    static float rowMid() { return kAudioCardH - kPad - kBtn / 2 + 4; }
    ui::RectF    actionRect() const {
        return {width() - kPad + 4 - kAction, rowMid() - kAction / 2, kAction, kAction};
    }
    // Sized from the duration so its right edge doesn't move as the time
    // ticks: the clip's length, or the widest "m:ss" when unknown.
    ui::RectF barRect(int64_t durationMs) const {
        const float k = windowScale();
        if (k != _barAt || durationMs != _barDur) { // measured once per length and scale
            _barAt  = k;
            _barDur = durationMs;
            _barLw  = std::max(
                text::measure(
                    durationMs > 0 ? formatDuration(durationMs, true) : "0:00", subFont(), k
                ),
                text::measure("0:00", subFont(), k)
            );
        }
        const float x     = kPad + kKnob / 2;
        const float right = actionRect().x - kLabelGap - std::ceil(_barLw) - 12;
        return {x, std::round(rowMid() - kBarH / 2), std::max(20.f, right - x), kBarH};
    }
    // The quoted preview's box: after the 3-px quote bar, as wide as the text
    // up to what leaves room for "View transcript".
    ui::RectF transcriptText() const {
        const float k     = windowScale();
        const float h     = 15 * kLine;
        const float top   = kAudioCardH + std::floor((kAudioTranscriptH - h) / 2);
        const float textX = 3 + kPad;
        if (k != _textAt) { // both widths depend only on the scale
            _textAt  = k;
            _linkW   = std::ceil(text::measure(tr("View transcript"), lineFont(C::Link), k));
            _natural = std::ceil(text::measure(_simpleTranscript, lineFont(C::FormTextMuted), k));
        }
        const float avail = width() - textX - _linkW - 6;
        return {textX, top, std::max(0.f, std::min(avail, _natural)), h};
    }

    Context                &_ctx;
    MessageList            *_list;
    Ts                      _ts;
    model::File             _file;
    std::string             _by; // the provider of an AI transcript
    PlayButton             *_play       = nullptr;
    TranscribeButton       *_transcribe = nullptr;
    TranscriptLink         *_link       = nullptr;
    AudioPlayer::ObserverId _audioObs   = 0;
    uint32_t                _aiObs      = 0;
    int64_t                 _scrubMs    = -1; // ≥ 0 while dragging the slider
    bool                    _scrubbing  = false;
    std::string             _simpleTranscript; // str::simplified(_file.transcript)
    LineCache               _name, _sub, _time, _transcriptLine;
    // barRect / transcriptText widths, per scale (0 = not measured yet).
    mutable float           _barAt = 0, _barLw = 0, _textAt = 0, _linkW = 0, _natural = 0;
    mutable int64_t         _barDur = 0;
};

void PlayButton::paint(gfx::Painter &p) {
    _card.paintPlay(p, bounds());
}
std::string PlayButton::accessibleName() const {
    return _card.playing() ? tr("Pause") : tr("Play");
}
void PlayButton::activate() {
    toggleAudio(_card.ctx(), _card.file());
}

void TranscribeButton::paint(gfx::Painter &p) {
    const bool  lit = hovered() || _card.transcribing();
    const float r   = width() / 2;
    if (lit)
        p.fillCircle({r, height() / 2}, r, ui::color(C::AccentSubtle));
    gfx::drawIcon(
        p,
        gfx::Icon::Captions,
        {std::floor((width() - kGlyph) / 2), std::floor((height() - kGlyph) / 2), kGlyph, kGlyph},
        ui::color(lit ? C::Accent : C::FormTextMuted)
    );
}
void TranscribeButton::activate() {
    startTranscription(_card.ctx(), _card.file(), _card.message());
}

void TranscriptLink::activate() {
    openTranscript(_card.ctx(), _card.file(), _card.message(), _card.by());
}

// The local file to play or transcribe for `url` ("" = it has to come down
// first): a local path as is, a download from the audio cache.
std::string
localCopy(Context &ctx, const model::File &f, const std::string &url, std::string *cache) {
    if (!RemoteImages::isRemote(url))
        return str::startsWith(url, "file://") ? file::fromFileUrl(url) : url;
    *cache = ctx.audio ? ctx.audio->cachePath(f, url) : std::string();
    return !cache->empty() && file::size(*cache) > 0 ? *cache : std::string();
}

// The bytes of `url` in the audio cache, then done(path, ok) — one
// background job ("Downloading …") while it comes down.
void download(
    Context                     &ctx,
    const model::File           &f,
    const std::string           &url,
    const std::string           &path,
    std::function<void(bool ok)> done
) {
    if (path.empty()) {
        done(false);
        return;
    }
    const int job = model::jobs().begin(arg(tr("Downloading %1"), f.name));
    fetchFile(
        ctx.app.platform(),
        ctx.backend,
        url,
        path,
        [job, remote = ctx.remote, path, done = std::move(done)](bool ok, const std::string &err) {
            model::jobs().end(job);
            if (!ok)
                LOG_WARN("audio", "download failed: %s", err.c_str());
            else if (remote) // counts toward the cache limit (lives as long as the app)
                remote->noteWritten(path);
            done(ok);
        }
    );
}

} // namespace

size_t audioCardLayoutBuilds() {
    return gLayoutBuilds;
}

ui::View *
addAudioCard(ui::View *parent, Context &ctx, MessageList *list, Ts ts, const model::File &f) {
    return parent->add<AudioCard>(ctx, list, ts, f);
}

void toggleAudio(Context &ctx, const model::File &f) {
    if (!ctx.audio)
        return;
    AudioPlayer &player = *ctx.audio;
    if (player.isCurrent(f.id)) {
        switch (player.status().state) {
        case AudioPlayer::State::Playing:
        case AudioPlayer::State::Paused:
        case AudioPlayer::State::Ended:
            player.togglePause();
            return;
        case AudioPlayer::State::Loading:
            return;
        default:
            break; // Error: try again
        }
    }
    const std::string url = player.sourceFor(f);
    if (url.empty())
        return;
    std::string       cache;
    const std::string local = localCopy(ctx, f, url, &cache);
    if (!local.empty()) {
        player.play(f.id, local, f.durationMs);
        return;
    }
    player.beginLoading(f.id, f.durationMs);
    AudioPlayer *pl = &player; // main's: outlives every download callback
    download(ctx, f, url, cache, [pl, id = f.id, cache, dur = f.durationMs](bool ok) {
        if (ok)
            pl->play(id, cache, dur);
        else
            pl->loadFailed(id, tr("Download failed"));
    });
}

void startTranscription(Context &ctx, const model::File &f, const model::Message *m) {
    llm::Service        *ai   = ctx.ai;
    // The provider that will actually transcribe: an OpenAI-compatible one
    // can while the default chat provider (Anthropic) cannot.
    const llm::Provider *prov = ai ? ai->sttProvider() : nullptr;
    TranscriptDialog    *dlg =
        TranscriptDialog::open(ctx, subtitleFor(ctx, m, prov ? prov->name : ""));
    if (ai)
        if (const std::string *hit = ai->transcriber().cached(f.id)) {
            dlg->setText(*hit);
            return;
        }
    // Fail fast: no download when nothing could consume it.
    if (!prov) {
        const llm::Provider *active = ai ? ai->active() : nullptr;
        if (active && !ai->isStandIn(active))
            dlg->setFailed(
                arg(tr("%1 does not support speech-to-text. Pick an OpenAI-compatible provider in "
                       "Settings \xE2\x86\x92 AI assistance."),
                    active->name)
            );
        else
            dlg->setFailed(
                tr("Transcription needs an AI provider. Connect one in Settings "
                   "\xE2\x86\x92 AI assistance.")
            );
        return;
    }

    std::weak_ptr<char> guard    = dlg->alive();
    const std::string   provider = prov->name;
    // Reads the bytes off the UI thread and hands them over. The dialog is
    // the listener; a closed one still lets the result be adopted.
    auto run = [&ctx, guard, dlg, provider, f](const std::string &path, const std::string &source) {
        auto bytes = std::make_shared<std::string>();
        auto ok    = std::make_shared<bool>(false);
        model::runInBackground(
            ctx.app.platform(),
            [bytes, ok, path] { *ok = file::readAll(path, bytes.get()); },
            [&ctx, guard, dlg, provider, f, bytes, ok, source] {
                if (!*ok) {
                    if (!guard.expired())
                        dlg->setFailed(tr("Could not read the file"));
                    return;
                }
                std::string ext = AudioPlayer::extensionOf(source);
                if (ext.empty())
                    ext = AudioPlayer::extensionOf(f.name);
                llm::TranscriptionInput in;
                in.audio    = std::move(*bytes);
                in.fileName = f.id + (ext.empty() ? "" : "." + ext);
                in.mimeType = llm::audioMimeForExtension(ext);
                if (in.mimeType.empty())
                    in.mimeType = f.mime;
                // One listener per dialog: adopt the text even if the dialog
                // is gone (it replaces the transcript line under the player
                // from now on — for this workspace only), then fill the
                // dialog if it is still open.
                const std::string ws = ctx.store().workspaceId;
                ctx.ai->transcriber().transcribe(
                    f.id,
                    std::move(in),
                    [&ctx, ws, guard, dlg, fileId = f.id, provider](
                        const llm::TranscriptionResult &r
                    ) {
                        if (r.ok && !r.text.empty() && ctx.store().workspaceId == ws)
                            ctx.store().setAiTranscript(fileId, r.text, provider);
                        if (guard.expired())
                            return;
                        if (!r.ok)
                            dlg->setFailed(arg(tr("Couldn't transcribe: %1"), r.error));
                        else if (r.text.empty())
                            dlg->setFailed(tr("No speech was recognised"));
                        else
                            dlg->setText(r.text);
                    }
                );
            }
        );
    };

    AudioPlayer      *pl  = ctx.audio;
    const std::string url = pl ? pl->sourceFor(f) : f.path;
    if (url.empty()) {
        dlg->setFailed(tr("Download failed"));
        return;
    }
    // The same bytes the player uses: a clip already played needs no fetch.
    std::string       cache;
    const std::string local = localCopy(ctx, f, url, &cache);
    if (!local.empty()) {
        run(local, url);
        return;
    }
    download(ctx, f, url, cache, [run, guard, dlg, cache, url](bool ok) {
        if (ok)
            run(cache, url);
        else if (!guard.expired())
            dlg->setFailed(tr("Download failed"));
    });
}

void openTranscript(
    Context &ctx, const model::File &f, const model::Message *m, const std::string &by
) {
    TranscriptDialog *dlg = TranscriptDialog::open(ctx, subtitleFor(ctx, m, by));
    if (f.transcriptVtt.empty()) {
        dlg->setText(f.transcript);
        return;
    }
    // Slack's cues: a download (with the workspace's credentials) to a temp
    // file, read off the UI thread.
    const std::string   path     = tempDownloadPath(ctx.app.platform(), f.id + ".vtt");
    std::weak_ptr<char> guard    = dlg->alive();
    auto                fallback = [guard, dlg, preview = f.transcript] {
        if (!guard.expired())
            dlg->setText(preview);
    };
    if (path.empty()) {
        fallback();
        return;
    }
    fetchFile(
        ctx.app.platform(),
        ctx.backend,
        f.transcriptVtt,
        path,
        [&ctx, guard, dlg, path, fallback](bool ok, const std::string &) {
            if (!ok) {
                fallback();
                return;
            }
            auto cues = std::make_shared<std::vector<VttCue>>();
            model::runInBackground(
                ctx.app.platform(),
                [cues, path] {
                    std::string data;
                    if (file::readAll(path, &data))
                        *cues = parseVtt(data);
                    file::remove(path);
                },
                [guard, dlg, cues, fallback] {
                    if (guard.expired())
                        return;
                    if (cues->empty())
                        fallback();
                    else
                        dlg->setCues(*cues);
                }
            );
        }
    );
}

std::vector<VttCue> parseVtt(std::string_view vtt) {
    // WEBVTT header, blank line, then cues: optional id line,
    // "hh:mm:ss.mmm --> hh:mm:ss.mmm", payload lines until a blank line.
    std::vector<std::string_view> lines;
    if (str::startsWith(vtt, "\xEF\xBB\xBF"))
        vtt.remove_prefix(3);
    for (size_t at = 0; at <= vtt.size();) {
        size_t nl = vtt.find('\n', at);
        if (nl == std::string_view::npos)
            nl = vtt.size();
        std::string_view l = vtt.substr(at, nl - at);
        if (!l.empty() && l.back() == '\r')
            l.remove_suffix(1);
        lines.push_back(l);
        at = nl + 1;
    }
    // ^(?:(\d+):)?(\d{1,2}):(\d{2})\.(\d{3})\s*-->
    const auto timing = [](std::string_view l, int64_t *ms) {
        int64_t parts[3] = {0, 0, 0};
        int     n        = 0;
        size_t  i        = 0;
        for (;;) {
            const size_t s = i;
            int64_t      v = 0;
            while (i < l.size() && l[i] >= '0' && l[i] <= '9')
                v = v * 10 + (l[i++] - '0');
            if (i == s || n == 3)
                return false;
            parts[n++] = v;
            if (i < l.size() && l[i] == ':') {
                ++i;
                continue;
            }
            break;
        }
        if (n < 2 || i + 4 > l.size() || l[i] != '.')
            return false;
        int64_t frac = 0;
        for (int k = 1; k <= 3; ++k) {
            if (l[i + size_t(k)] < '0' || l[i + size_t(k)] > '9')
                return false;
            frac = frac * 10 + (l[i + size_t(k)] - '0');
        }
        i += 4;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
            ++i;
        if (l.substr(i, 3) != "-->")
            return false;
        const int64_t h = n == 3 ? parts[0] : 0, m = parts[n - 2], sec = parts[n - 1];
        *ms = ((h * 60 + m) * 60 + sec) * 1000 + frac;
        return true;
    };
    std::vector<VttCue> cues;
    for (size_t i = 0; i < lines.size(); ++i) {
        VttCue cue;
        if (!timing(lines[i], &cue.startMs))
            continue;
        std::string payload;
        for (++i; i < lines.size() && !str::trim(lines[i]).empty(); ++i) {
            std::string l(str::trim(lines[i]));
            for (size_t lt; (lt = l.find('<')) != std::string::npos;) { // <v Speaker>, <c>, <i>…
                const size_t gt = l.find('>', lt);
                if (gt == std::string::npos)
                    break;
                l.erase(lt, gt - lt + 1);
            }
            if (str::startsWith(l, "- "))
                l.erase(0, 2);
            payload += (payload.empty() ? "" : " ") + l;
        }
        cue.text = str::simplified(payload);
        if (!cue.text.empty())
            cues.push_back(std::move(cue));
    }
    return cues;
}

std::string formatDuration(int64_t ms, bool round) {
    const int64_t s = std::max<int64_t>(0, round ? (ms + 500) / 1000 : ms / 1000);
    const int64_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char          buf[32];
    if (h > 0)
        std::snprintf(
            buf, sizeof buf, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)sec
        );
    else
        std::snprintf(buf, sizeof buf, "%lld:%02lld", (long long)m, (long long)sec);
    return buf;
}

} // namespace screens
