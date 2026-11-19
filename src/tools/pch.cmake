# Precompiled standard headers (src/tools/pch.h) for msga's own C++ targets,
# included at the end of the root CMakeLists.txt once every target exists.
# Measured numbers are in pch.h.
#
# AUTO (the default) turns it on in test trees (MSGA_BUILD_TESTS) only. Every
# release script configures with MSGA_BUILD_TESTS=OFF, so a shipped binary is
# never built through a PCH, and those builds are the ones that catch a file
# that forgot an #include the PCH was supplying. Not with MinGW: GCC's PCH
# there has a history of "had to relocate PCH" failures under ASLR.
set(MSGA_PCH AUTO CACHE STRING "Precompile the common standard headers: AUTO (test trees, not MinGW), ON or OFF")
set_property(CACHE MSGA_PCH PROPERTY STRINGS AUTO ON OFF)
if(MSGA_PCH STREQUAL "AUTO")
    if(MSGA_BUILD_TESTS AND NOT MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(_msga_pch ON)
    else()
        set(_msga_pch OFF)
    endif()
else()
    set(_msga_pch ${MSGA_PCH})
endif()
if(NOT _msga_pch)
    return()
endif()

set(_msga_pch_header "${CMAKE_CURRENT_LIST_DIR}/pch.h")

# One PCH, compiled once with msga_flags and reused by every target built with
# those flags; a PCH per target costs a ~3 s compile (76 MB .gch at -g) for each
# of ~45 targets, which ate the whole gain.
file(WRITE "${CMAKE_BINARY_DIR}/msga_pch.cpp" "")
add_library(msga_pch OBJECT "${CMAKE_BINARY_DIR}/msga_pch.cpp")
target_link_libraries(msga_pch PRIVATE msga_flags)
target_precompile_headers(msga_pch PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_msga_pch_header}>")

function(_msga_pch_targets dir out)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub ${subdirs})
        _msga_pch_targets("${sub}" sub_targets)
        list(APPEND targets ${sub_targets})
    endforeach()
    set(${out} ${targets} PARENT_SCOPE)
endfunction()

# Whether msga_flags reaches the target's compile line (directly or through a
# PUBLIC/INTERFACE dependency): only then do its flags match msga_pch's.
function(_msga_pch_uses_flags target out)
    set(seen "")
    get_target_property(queue ${target} LINK_LIBRARIES)
    while(queue)
        list(POP_FRONT queue dep)
        if(dep STREQUAL "msga_flags")
            set(${out} ON PARENT_SCOPE)
            return()
        endif()
        if(NOT TARGET "${dep}" OR dep IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${dep})
        get_target_property(next ${dep} INTERFACE_LINK_LIBRARIES)
        if(next)
            list(APPEND queue ${next})
        endif()
    endwhile()
    set(${out} OFF PARENT_SCOPE)
endfunction()

_msga_pch_targets("${CMAKE_SOURCE_DIR}" _msga_all_targets)
foreach(t ${_msga_all_targets})
    get_target_property(type ${t} TYPE)
    if(t STREQUAL "msga_pch" OR NOT type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
        continue()
    endif()
    get_target_property(sources ${t} SOURCES)
    set(cxx_sources ${sources})
    list(FILTER cxx_sources INCLUDE REGEX "\\.cpp$")
    list(FILTER cxx_sources EXCLUDE REGEX "third_party")
    if(NOT cxx_sources)
        continue() # third-party code and C-only targets
    endif()
    set(other_sources ${sources})
    list(FILTER other_sources INCLUDE REGEX "\\.(c|m|mm)$")
    _msga_pch_uses_flags(${t} uses_flags)
    if(t STREQUAL "plat" OR other_sources OR t MATCHES "^(msga_text|text_tests)$")
        # Its own PCH: plat has its own flags; REUSE_FROM cannot serve a
        # target that also compiles C or Objective-C(++) (the PCH stays C++
        # only); and msga_text/text_tests get pkg-config's -pthread from the
        # system HarfBuzz, whose _REENTRANT makes GCC reject the shared one.
        if(uses_flags OR t STREQUAL "plat")
            target_precompile_headers(${t} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_msga_pch_header}>")
        endif()
    elseif(uses_flags)
        target_precompile_headers(${t} REUSE_FROM msga_pch)
    endif()
endforeach()

# Files whose compile line differs from their target's: Clang rejects a PCH
# built at another optimisation level (blend.cpp is -O2), and through a PCH
# Clang drops libstdc++'s namespace visibility from the out-of-namespace
# __gnu_cxx::__verbose_terminate_handler definition (it came out HIDDEN).
if(TARGET msga_gfx) # MSGA_MODULES may stop below gfx
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/gfx/blend.cpp"
        TARGET_DIRECTORY msga_gfx PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
if(TARGET msga AND EXISTS "${CMAKE_SOURCE_DIR}/src/app/crash/terminate_handler.cpp")
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/app/crash/terminate_handler.cpp"
        TARGET_DIRECTORY msga PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
