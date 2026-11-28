# Cocoa (macOS) backend: Objective-C++ with ARC. Included by plat's top-level
# CMakeLists.txt on APPLE. OBJCXX is enabled here so the top level can stay
# C/C++ only (the Linux/Windows builds never need an Objective-C toolchain).
enable_language(OBJCXX)

set(PLAT_COCOA_SOURCES
    src/cocoa/cocoa_app.mm
    src/cocoa/cocoa_window.mm
    src/cocoa/cocoa_keys.mm
    src/cocoa/cocoa_data.mm
    src/cocoa/cocoa_tray.mm
    src/cocoa/cocoa_notify.mm
    src/cocoa/cocoa_services.mm
)
target_sources(plat PRIVATE ${PLAT_COCOA_SOURCES})
set_source_files_properties(${PLAT_COCOA_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# UNUserNotificationCenter raises an Objective-C exception for a process it
# cannot identify; the one @try that guards it must survive PLAT_NO_EXCEPTIONS.
set_source_files_properties(src/cocoa/cocoa_notify.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc;-fexceptions;-fobjc-exceptions")
target_compile_definitions(plat PRIVATE PLAT_HAS_COCOA)
# Carbon: TIS input sources + UCKeyTranslate (layout-aware key mapping).
target_link_libraries(plat PUBLIC
    "-framework AppKit"
    "-framework QuartzCore"
    "-framework IOSurface"
    "-framework CoreGraphics"
    "-framework Carbon"
    "-framework UniformTypeIdentifiers"
    "-framework UserNotifications"
    "-framework ImageIO"
    "-framework Network"
    "-framework CoreServices"
)
