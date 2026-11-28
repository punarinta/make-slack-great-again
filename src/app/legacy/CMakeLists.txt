# msga_app_legacy — the upgrade from earlier versions' data (legacy.h), linked into
# the msga executable only. Removable: see legacy.h.
add_library(msga_app_legacy STATIC
    legacy.cpp
    old_settings_import.cpp
    old_cache_import.cpp
)
target_link_libraries(msga_app_legacy PUBLIC msga_screens_shell msga_flags)
target_link_libraries(msga PRIVATE msga_app_legacy)
target_compile_definitions(msga PRIVATE MSGA_LEGACY_IMPORT)
