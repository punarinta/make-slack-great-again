# Win32 backend (included by src/plat/CMakeLists.txt on WIN32). Pure Win32 + DWM +
# IMM32 + OLE (drag and drop) + shell (tray, taskbar); newer entry points
# (per-monitor DPI v2, GetDpiForWindow, WinRT toasts in combase, …) are
# resolved at runtime, so the binary still starts on early Windows 10 builds
# and under Wine. WIC (PNG) and the shell helpers are reached through
# CoCreateInstance, so they add no import libraries.
target_sources(plat PRIVATE
    src/win32/win32_app.cpp
    src/win32/win32_data.cpp
    src/win32/win32_dialog.cpp
    src/win32/win32_dnd.cpp
    src/win32/win32_image.cpp
    src/win32/win32_keys.cpp
    src/win32/win32_shell.cpp
    src/win32/win32_system.cpp
    src/win32/win32_window.cpp
)
target_compile_definitions(plat
    PUBLIC PLAT_HAS_WIN32
    PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
)
# PUBLIC: plat is a static library, so its consumers' link lines need these.
target_link_libraries(plat PUBLIC user32 gdi32 dwmapi imm32 shell32 ole32 oleaut32 uuid advapi32 windowscodecs)
