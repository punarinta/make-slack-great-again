# Audio (include/plat/audio.h): playback, microphone capture and notification
# sounds, with what each OS ships — no new shared libraries anywhere.
#   Linux, BSDs: the sound server's CLI helpers (pw-cat/pw-record/…, sndio's
#            aucat) over pipes,
#            plus the vendored miniaudio decoders (src/third_party/miniaudio,
#            public domain / MIT-0; decoding only, device I/O compiled out).
#   Windows: MFPlay (Media Foundation) + waveIn/PlaySound (winmm).
#   macOS:   AVAudioPlayer / AVAudioRecorder (AVFoundation) + NSSound.
target_sources(plat PRIVATE src/audio/audio_common.cpp)
if(WIN32)
    target_sources(plat PRIVATE src/audio/audio_win32.cpp)
    target_link_libraries(plat PUBLIC mfplay mfplat winmm ole32)
elseif(APPLE)
    target_sources(plat PRIVATE src/audio/audio_cocoa.mm)
    set_source_files_properties(src/audio/audio_cocoa.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(plat PUBLIC "-framework AVFoundation")
else()
    enable_language(C)
    add_library(plat_miniaudio STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio/miniaudio_impl.c)
    target_include_directories(plat_miniaudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio)
    target_compile_options(plat_miniaudio PRIVATE -w)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        # As msga_flags does for our own code: sections for --gc-sections,
        # and no unwind tables (nothing unwinds through C).
        target_compile_options(plat_miniaudio PRIVATE -ffunction-sections -fdata-sections
            -fno-asynchronous-unwind-tables -fno-unwind-tables)
    endif()
    target_link_libraries(plat_miniaudio PUBLIC m)
    target_sources(plat PRIVATE src/audio/audio_linux.cpp)
    target_link_libraries(plat PRIVATE plat_miniaudio)
endif()
