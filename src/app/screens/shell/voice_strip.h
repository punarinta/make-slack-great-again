// The composer's voice input pieces (the recording strip and the context
// sent along for vocabulary).
//
// VoiceStrip — the status row between the editor and the bottom bar while a
// dictation started from that composer is in flight:
//   Recording     a pulsing red dot, a scrolling level meter, the elapsed
//                 time (m:ss) and "Esc to cancel"
//   Transcribing / Cleaning  a small spinner and "Transcribing…" /
//                 "Cleaning up…"
//   Error         the failure in the danger colour; hides itself after 6 s
// plus a cancel (×) button on the right in every mode. Hidden otherwise.
//
// buildVoiceContext — what a dictation should know about where its text
// goes (llm::VoiceContext): the conversation's name, the people in it and
// the newest messages on hand ("Name: text"). Never a network call: an
// unknown user is left out rather than fetched.
#pragma once

#include "app/llm/voice_prompt.h"
#include "screens/common/context.h"
#include "screens/common/loading_indicator.h"
#include "ui/ui.h"

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace shell {

class GlyphButton;

class VoiceStrip : public ui::View {
public:
    enum class Mode : uint8_t { Hidden, Recording, Transcribing, Cleaning, Error };
    static constexpr int kErrorMs = 6000;

    explicit VoiceStrip(plat::App &app);
    ~VoiceStrip() override;

    // Entering Recording (from any other mode) restarts the elapsed time and
    // clears the meter. Error shows `errorText` for kErrorMs.
    void               setMode(Mode mode, std::string errorText = {});
    Mode               mode() const { return _mode; }
    const std::string &error() const { return _error; }
    // Peak 0..1 of the latest chunk; ignored outside Recording.
    void               pushLevel(float peak);
    // "m:ss" of the recording so far (tests).
    std::string        elapsedText() const;

    // The × button: cancel the dictation (or dismiss the error).
    std::function<void()> onCancel;

    void layout() override;
    void paint(gfx::Painter &p) override;
    void styleChanged() override;

private:
    static constexpr int kBars = 28; // meter history, newest on the right
    // The texts as last shaped, by their place in the strip (at most two
    // show at once): reused while text, colour and room stay the same, as the
    // strip repaints at 20 Hz while recording.
    struct ShapedText {
        std::string                   text;
        gfx::Color                    color    = 0;
        float                         maxWidth = 0, scale = 0;
        std::unique_ptr<text::Layout> layout;
    };
    ShapedText _texts[2];

    plat::App                &_app;
    Mode                      _mode = Mode::Hidden;
    std::string               _error;
    GlyphButton              *_cancel    = nullptr;
    int64_t                   _startMs   = 0;             // Recording began (base::monotonicMs)
    plat::TimerId             _errorHide = 0, _pulse = 0; // pulse: repaints while recording
    std::array<float, kBars>  _levels{};
    int                       _head = 0; // next slot in the _levels ring
    screens::LoadingIndicator _spinner;
};

// `thread` 0: the conversation's top-level messages; else that thread's.
llm::VoiceContext buildVoiceContext(
    const model::Store &store, model::ConvRef conv, model::Ts thread, int maxMessages = 30
);

} // namespace shell
