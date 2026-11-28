// Prompt shaping for composer voice input — pure
// functions, no network, so what goes to speech-to-text and to the clean-up
// pass is unit-tested.
//
//   extractKeywords      terms the STT model must spell exactly (gpt-* keywords[])
//   buildSttPrompt       the STT `prompt`: a fixed style sentence, then context
//   buildCleanupRequest  the optional chat pass that strips disfluencies
#pragma once

#include "app/llm/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace llm {

// What the composer knows about where the dictated text will go.
// Drives the speech-to-text prompt and keywords and the
// clean-up pass. Everything optional.
struct VoiceContext {
    std::string              conversationName; // "#backend", or the DM peer's name
    std::vector<std::string> memberNames;      // the conversation's members' names
    std::vector<std::string> recentMessages;   // plain text, oldest → newest (the last ~30)
};

namespace voice {

// Leading sentence of the STT prompt for instruction-following models (the
// gpt-*-transcribe family). Never truncated.
inline constexpr const char *kInstructionPreamble =
    "Clean, edited transcript of a chat message dictated by a software professional: omit "
    "filler words (um, uh, er, like, you know), false starts and repetitions; keep technical "
    "terms, identifiers and names exactly.";
// Leading sentence for Whisper-style models. Whisper reads the prompt as the
// transcript that preceded the audio and imitates its style — so this is a
// cleanly punctuated, filler-free piece of "earlier speech", with no
// instruction wording (instructions leak into the output verbatim).
inline constexpr const char *kTranscriptStylePreamble =
    "Okay, I checked the logs and the fix works. The pull request is ready for review.";

// Upper bound of the whole STT prompt, in characters (Whisper only looks at
// its last ~224 tokens anyway; OpenAI caps it too).
inline constexpr size_t kMaxSttPromptChars = 1000;

// Whether `sttModel` follows prompt instructions and takes keywords[] /
// languages[] (the gpt-*-transcribe family) rather than being Whisper-style.
bool isInstructionFollowingSttModel(std::string_view sttModel);

// Slack mrkdwn → plain words: user/special mentions dropped, <#C…|name> →
// name, <url|label> → label, bare <url> dropped, :emoji: codes dropped,
// backticks / * / ~ removed. Underscores stay (snake_case identifiers);
// _italic_ markers are trimmed per token by extractKeywords.
std::string stripSlackMarkup(std::string_view text);

// Keywords for speech-to-text, most important first: the user's glossary,
// member names, the conversation name (no leading '#'), then "code-looking"
// tokens from the recent messages (newer messages weigh more): CamelCase /
// camelCase, snake_case, kebab-case, ALL-CAPS acronyms, dotted names
// (llm_wire.cpp, v1.2) and letter+digit mixes (gpt-5, k8s). Deduped
// case-insensitively (first spelling wins), tokens over 50 characters
// dropped, sanitised as sanitizeTranscriptionKeywords, at most `cap`.
std::vector<std::string>
extractKeywords(const VoiceContext &ctx, const std::vector<std::string> &glossary, int cap = 80);

// The STT `prompt`: kInstructionPreamble (instruction-following models) or
// kTranscriptStylePreamble (Whisper-style) first, then the conversation name
// and the last few recent messages, each clipped — the whole thing kept under
// kMaxSttPromptChars without ever cutting the leading sentence.
std::string buildSttPrompt(const VoiceContext &ctx, bool instructionFollowingModel);

// Chat request that turns a raw transcript into the message the user meant:
// fillers, false starts and self-corrections resolved, technical terms and
// names spelled as in the context, punctuation added — and nothing else (no
// rephrasing, summarising, translating or answering). The reply is the
// cleaned text only. `outputLanguageHint`: ISO 639-1 code of the user's
// language (Service::language()) — a hint, the transcript's own language
// always wins. `model` is left empty for the caller (the light model).
Request buildCleanupRequest(
    std::string_view transcript, const VoiceContext &ctx, std::string_view outputLanguageHint
);

} // namespace voice
} // namespace llm
