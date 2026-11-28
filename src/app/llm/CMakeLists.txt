# msga_app_llm — the AI provider layer: the Anthropic Messages and
# OpenAI-compatible chat wires, the presets, the Service the app asks (over
# net::Client), and the prompts built on it (discussion summaries, voice
# input clean-up), audio transcription and voice input. Triage is meant to
# land here as well.
add_library(msga_app_llm STATIC
    wire.cpp
    provider.cpp
    service.cpp
    audio_transcriber.cpp
    discussion_summary.cpp
    voice_prompt.cpp
    voice_input.cpp
)
target_include_directories(msga_app_llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(msga_app_llm PUBLIC msga_base msga_net plat msga_app_core msga_app_spell msga_flags)
