// The inline audio player in the message list (the audio file card, its
// play / pause, transcription and the transcript dialog).
//
// An audio file (File::isAudio) shows as a 380×88 card: a round play/pause
// button, the name over "0:05 (79 KB)" / "Loading…" / the error, a seek bar
// with the time, and the "Transcribe with AI" button; under it, when the file
// has a transcript (Slack's for a voice clip, or the user's own AI one), a
// quoted line and "View transcript". A click on the card plays or pauses; one
// file plays at a time (Context::audio, media/audio_player.h) and the
// bytes come through screens::fetchFile into the audio cache as a background
// job, from the source this platform can decode.
#pragma once

#include "app/screens/messages/context_fwd.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace screens {

class MessageList;

inline constexpr float kAudioCardH       = 88;
inline constexpr float kAudioTranscriptH = 26; // the transcript line under the card
inline constexpr float kAudioCardMaxW    = 380;

// The card for `f` (its message `ts` in `list`; ts 0 or no list: a quoted
// file, no file bar).
ui::View *
addAudioCard(ui::View *parent, Context &ctx, MessageList *list, Ts ts, const model::File &f);

// Single-line layouts the audio cards have shaped so far (tests: playback
// ticks reshape nothing but a changed time label).
size_t audioCardLayoutBuilds();

// Play or pause `f`: downloads it first when needed.
void toggleAudio(Context &ctx, const model::File &f);
// "Transcribe with AI": the transcript dialog, filled by the speech-to-text
// provider (a cached result at once); the text then replaces the file's
// transcript line (Store::setAiTranscript). `m` (may be null) names the
// author in the subtitle.
void startTranscription(Context &ctx, const model::File &f, const model::Message *m);
// "View transcript": Slack's WebVTT cues (or the preview line), or the AI
// one (`by` its provider, for the subtitle).
void openTranscript(
    Context &ctx, const model::File &f, const model::Message *m, const std::string &by = {}
);

// One WebVTT cue: start time and its text (speaker dashes/tags stripped).
struct VttCue {
    int64_t     startMs = 0;
    std::string text;
    bool        operator==(const VttCue &o) const { return startMs == o.startMs && text == o.text; }
};
std::vector<VttCue> parseVtt(std::string_view vtt);
// "0:05", "12:34", "1:02:03". Position labels floor, duration labels round —
// a 4.98 s clip is "0:05" long but is at "0:04" while it plays.
std::string         formatDuration(int64_t ms, bool round = false);

} // namespace screens
