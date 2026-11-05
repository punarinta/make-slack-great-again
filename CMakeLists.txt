# msga — see src/README.md. Size-first build: every flag here exists to make
# the binary smaller; opt a hot file into -O2 locally, never globally.
cmake_minimum_required(VERSION 3.21)
project(msga LANGUAGES C CXX)
# Each static library once on a link line where the linker resolves them in
# any order (Apple's ld warns about the repeats).
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

# -Oz where the compiler has it (Clang, GCC 12+): Clang's -Oz build is ~20%
# smaller than its -Os one, GCC's ~1%. MSGA_SIZE_OPT is for the third-party
# targets that pin their own optimisation level.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Oz MSGA_HAVE_OZ)
if(MSGA_HAVE_OZ)
    set(MSGA_SIZE_OPT -Oz)
    # After CMake's own -Os on the command line, so it wins (every language,
    # Objective-C++ included, and every target added below).
    add_compile_options($<$<CONFIG:MinSizeRel>:-Oz>)
else()
    set(MSGA_SIZE_OPT -Os)
endif()

# Flags for every msga module (not for third-party code, which gets its own).
add_library(msga_flags INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(msga_flags INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
        -ffunction-sections -fdata-sections -fvisibility=hidden
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(NOT APPLE)
        target_link_options(msga_flags INTERFACE -Wl,--gc-sections)
    else()
        target_link_options(msga_flags INTERFACE -Wl,-dead_strip)
    endif()
endif()
include(CheckLinkerFlag)
if(MINGW)
    # mingw gcc links with ld.bfd; lld's COFF ICF needs its own recipe
    # (see memory lld-coff-icf notes) — off until the Windows release script.
    set(MSGA_HAVE_LLD OFF)

    # MSYS2 GCC 16: C++20 + -Os/-Oz leaves std::string's move ctor undefined
    # (MINGW-packages#31015, GCC PR 125359). -Os/-Oz turn on -fdeclone-ctor-dtor.
    check_cxx_compiler_flag(-fno-declone-ctor-dtor MSGA_HAVE_NO_DECLONE)
    if(MSGA_HAVE_NO_DECLONE)
        add_compile_options(
            $<$<AND:$<CONFIG:MinSizeRel>,$<COMPILE_LANGUAGE:CXX>>:-fno-declone-ctor-dtor>)
    endif()
else()
    check_linker_flag(CXX "-fuse-ld=lld" MSGA_HAVE_LLD)
endif()
if(MSGA_HAVE_LLD AND NOT APPLE)
    target_link_options(msga_flags INTERFACE -fuse-ld=lld)
endif()
# Packed relative relocations (DT_RELR): a PIE's vtable and function-pointer
# fixups shrink from 24 bytes each (~180 KB of .rela.dyn) to a bitmap of ~5 KB.
# Needs glibc 2.36 / musl 1.2.4 at run time; a static non-PIE has none anyway.
if(NOT APPLE AND NOT MINGW)
    if(MSGA_HAVE_LLD)
        set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=lld)
    endif()
    check_linker_flag(CXX "-Wl,-z,pack-relative-relocs" MSGA_HAVE_RELR)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(MSGA_HAVE_RELR)
        target_link_options(msga_flags INTERFACE -Wl,-z,pack-relative-relocs)
    endif()
endif()
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # No exceptions → nothing needs .eh_frame; it can be ~9% of a binary. A
    # future crash reporter walks frames itself (or we ship symbolised dumps).
    target_compile_options(msga_flags INTERFACE -fno-asynchronous-unwind-tables -fno-unwind-tables)
endif()
# Coverage (scripts/coverage.sh): gcov instrumentation of our own code and the
# tests: everything on msga_flags, plus plat (below). Not the vendored code
# (mbedTLS, FreeType, HarfBuzz, stb, Hunspell), which never links msga_flags:
# its .gcno/.gcda would only be noise in the report. -O0 comes after the
# build type's -Os/-Oz, so line counts match the source. Atomic counter
# updates: the net/loop threads otherwise race them into negative hit counts,
# which gcov prints and gcovr refuses.
option(MSGA_COVERAGE "Instrument msga's own code and the tests for coverage (gcov)" OFF)
if(MSGA_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "MSGA_COVERAGE needs GCC or Clang")
    endif()
    set(MSGA_COVERAGE_FLAGS --coverage -fprofile-update=atomic -O0 -g)
    target_compile_options(msga_flags INTERFACE ${MSGA_COVERAGE_FLAGS})
    target_link_options(msga_flags INTERFACE --coverage)
endif()
# AddressSanitizer + LeakSanitizer (scripts/run-asan.sh, build.sh --asan →
# build-asan/): heap errors (use-after-free, overflows) as they happen, leaks
# reported at a clean exit. Our code and plat are instrumented; the vendored
# libraries are not, but their mallocs still go through ASan, so their leaks
# show up too. -O1 comes after Debug's -O0: ASan at -O0 makes the UI crawl.
option(MSGA_ASAN "Build with AddressSanitizer + LeakSanitizer" OFF)
if(MSGA_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "MSGA_ASAN needs GCC or Clang")
    endif()
    set(MSGA_ASAN_FLAGS -fsanitize=address -fno-omit-frame-pointer -O1 -g)
    target_compile_options(msga_flags INTERFACE ${MSGA_ASAN_FLAGS})
    target_link_options(msga_flags INTERFACE -fsanitize=address)
endif()
# The release scripts turn MSGA_LINK_STRIP off: they build with -g, split the
# symbols into a .debug / .dSYM for crash reports, then strip themselves.
option(MSGA_LINK_STRIP "Strip MinSizeRel executables at link time (lld)" ON)
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" AND NOT MSGA_COVERAGE AND NOT MSGA_ASAN)
    # lld folds identical functions (template copies, trivial thunks). Not
    # under coverage: folded functions would share one function's counts.
    if(MSGA_HAVE_LLD AND NOT APPLE)
        target_link_options(msga_flags INTERFACE -Wl,--icf=all)
        if(MSGA_LINK_STRIP)
            target_link_options(msga_flags INTERFACE -s)
        endif()
    endif()
endif()

# Fully static executables, the release binaries: on Linux one that runs on
# any distribution (Alpine/musl, scripts/release-linux-static.sh; every library
# from its .a, pkg-config asked for the static link lines), on Windows an .exe
# with libstdc++/libgcc/winpthread linked in (scripts/release-windows.sh).
option(MSGA_STATIC "Link fully static (the Linux/musl and Windows releases)" OFF)
if(MSGA_STATIC AND MINGW)
    target_link_options(msga_flags INTERFACE -static)
elseif(MSGA_STATIC)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a) # after project(): the platform files reset it
    set(PKG_CONFIG_ARGN --static)
    target_link_options(msga_flags INTERFACE -static-pie
        # musl's default thread stack is 128 KiB (glibc: 8 MiB) and the only
        # knob for threads we don't create ourselves is PT_GNU_STACK's size,
        # which musl adopts at startup. Too small a stack segfaults (issue
        # #70); release-linux-static.sh checks the segment.
        -Wl,-z,stack-size=8388608)
endif()

# What a distribution's package turns off, its package manager doing both:
# msga replacing its own binary with the next release (app/update), and, on
# Linux, writing its own ~/.local/share/applications/msga.desktop on every
# start (screens/shell/desktop_entry.cpp), which names this build's path.
option(MSGA_SELF_UPDATE "Check msga.app for new releases and install them" ON)
option(MSGA_INSTALL_LAUNCHER "Linux: write the user's msga.desktop launcher entry at startup" ON)
# On msga_flags: the updater's UI (the update bar, Settings' update checks)
# and its tests compile out with it, not only app/update.
foreach(opt MSGA_SELF_UPDATE MSGA_INSTALL_LAUNCHER)
    if(${opt})
        target_compile_definitions(msga_flags INTERFACE ${opt})
    endif()
endforeach()

# Linker maps for tools/size_report.py (per-module size + budgets).
option(MSGA_SIZE_MAP "Write <exe>.map linker maps for the size report" ON)
function(msga_size_map target)
    if(MSGA_SIZE_MAP AND MSGA_HAVE_LLD AND NOT APPLE)
        target_link_options(${target} PRIVATE "-Wl,-Map=$<TARGET_FILE:${target}>.map")
    endif()
endfunction()

# The fake backend (app/fake: a fixture.json as a workspace; the tests use
# tests/assets/fixture.json) is OPT-IN ONLY:
# nothing of it is compiled unless one of these is on, and only MSGA_DEMO puts
# it into the msga executable (`msga --demo <dir>`, messages_demo). Release
# builds pass neither.
option(MSGA_DEMO "Compile the fixture-driven demo workspace into msga (--demo)" OFF)
option(MSGA_BUILD_TESTS "Build the test suites (and the fake backend they run on)" OFF)
if(MSGA_BUILD_TESTS)
    enable_testing()
endif()

# plat, built with the same no-exceptions/no-RTTI flags. Its TestHooks (OS-level
# input injection and screen readback) and headless backend are only compiled
# into test and demo builds; release binaries get testHooks() == nullptr.
set(PLAT_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(PLAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(MSGA_BUILD_TESTS OR MSGA_DEMO)
    set(PLAT_TEST_HOOKS ON CACHE BOOL "" FORCE)
else()
    set(PLAT_TEST_HOOKS OFF CACHE BOOL "" FORCE)
endif()
# prim: the primitives plat and base share (plat can't link base). Added
# here, before plat, so it builds with msga's flags.
add_subdirectory(src/prim prim EXCLUDE_FROM_ALL)
target_link_libraries(prim PRIVATE msga_flags)
add_subdirectory(src/plat plat EXCLUDE_FROM_ALL)
if(MSGA_COVERAGE)
    # plat has its own flags, not msga_flags. Its generated Wayland protocol
    # code is instrumented too; it lives in the build dir, outside the report.
    target_compile_options(plat PRIVATE ${MSGA_COVERAGE_FLAGS})
    target_link_options(plat INTERFACE --coverage)
endif()
if(MSGA_ASAN)
    target_compile_options(plat PRIVATE ${MSGA_ASAN_FLAGS})
    target_link_options(plat INTERFACE -fsanitize=address)
endif()

# MSGA_VERSION: the release number (the update check, the bundle and .exe
# version resources).
include("${CMAKE_CURRENT_SOURCE_DIR}/version.cmake")

# The build's Slack app keys (gitignored; see credentials.cmake.example).
include("${CMAKE_CURRENT_SOURCE_DIR}/credentials.cmake" OPTIONAL)

# Modules add themselves; dependencies only go downwards (see README).
# MSGA_MODULES narrows the build to a prefix of the stack (e.g. "base;net"
# for a cross build of the network layer before the UI ports exist).
set(MSGA_MODULES "base;net;gfx;text;ui;app" CACHE STRING "Modules to build, bottom-up")
# The test media (the fake workspace's fixture, avatars, GIFs, SVG fixtures):
# the test suites read it, and so do the gfx/text/ui gallery dev binaries.
set(MSGA_TEST_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets")
foreach(mod ${MSGA_MODULES})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${mod}/CMakeLists.txt")
        add_subdirectory(src/${mod} ${mod})
    endif()
endforeach()

# The test suites (tests/CMakeLists.txt), one directory per module.
if(MSGA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Precompiled standard headers for the test trees (see src/tools/pch.cmake).
include(src/tools/pch.cmake)
