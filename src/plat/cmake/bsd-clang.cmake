# Cross-compile for FreeBSD or OpenBSD (amd64) from Linux with the host's
# clang and lld against a sysroot (scripts/bsd-sysroot.py fetches one;
# scripts/build-bsd-cross.sh drives the whole build):
#   cmake -S . -B build-freebsd -G Ninja -DCMAKE_TOOLCHAIN_FILE=src/plat/cmake/bsd-clang.cmake \
#         -DMSGA_BSD=freebsd -DMSGA_BSD_SYSROOT=build-bsd-sysroot/freebsd/root
# MSGA_BSD is freebsd or openbsd; MSGA_BSD_VERSION the release the triple names
# (14.4 / 7.9 by default). Nothing from the sysroot is ever run: wayland-scanner
# and the other build tools are the host's.
set(MSGA_BSD "" CACHE STRING "Target BSD: freebsd or openbsd")
set(MSGA_BSD_SYSROOT "" CACHE PATH "Sysroot with the target's base system and packages")
set(MSGA_BSD_VERSION "" CACHE STRING "Release in the target triple (default: 14.4 / 7.9)")
# try_compile() projects re-read this file: hand them the same choices.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSGA_BSD MSGA_BSD_SYSROOT MSGA_BSD_VERSION)

if(MSGA_BSD STREQUAL "freebsd")
    set(CMAKE_SYSTEM_NAME FreeBSD)
    if(NOT MSGA_BSD_VERSION)
        set(MSGA_BSD_VERSION 14.4)
    endif()
    # Ports install under /usr/local with their .pc files in libdata.
    set(_bsd_pc "/usr/libdata/pkgconfig:/usr/local/libdata/pkgconfig:/usr/local/share/pkgconfig")
    set(_bsd_libs /usr/local/lib)
elseif(MSGA_BSD STREQUAL "openbsd")
    set(CMAKE_SYSTEM_NAME OpenBSD)
    if(NOT MSGA_BSD_VERSION)
        set(MSGA_BSD_VERSION 7.9)
    endif()
    # Xenocara (X11, xcb, freetype) lives in /usr/X11R6, packages in /usr/local.
    set(_bsd_pc "/usr/lib/pkgconfig:/usr/X11R6/lib/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig")
    set(_bsd_libs /usr/X11R6/lib /usr/local/lib)
else()
    message(FATAL_ERROR "bsd-clang.cmake: set MSGA_BSD to freebsd or openbsd")
endif()
if(NOT IS_DIRECTORY "${MSGA_BSD_SYSROOT}/usr/include")
    message(FATAL_ERROR "bsd-clang.cmake: no sysroot at '${MSGA_BSD_SYSROOT}' (scripts/bsd-sysroot.py)")
endif()
get_filename_component(MSGA_BSD_SYSROOT "${MSGA_BSD_SYSROOT}" ABSOLUTE)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT "${MSGA_BSD_SYSROOT}")

set(_triple x86_64-unknown-${MSGA_BSD}${MSGA_BSD_VERSION})
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_triple})

# lld for every link, try_compile's included: the host's GNU ld knows neither
# OS's conventions (OpenBSD's libfoo.so.M.N names, for one). The libraries'
# own dependencies (libwayland-client → libepoll-shim, libffi) are found in
# the sysroot, never on the host.
set(_bsd_link "-fuse-ld=lld")
foreach(dir ${_bsd_libs})
    string(APPEND _bsd_link " -Wl,-rpath-link,${MSGA_BSD_SYSROOT}${dir}")
endforeach()
if(MSGA_BSD STREQUAL "openbsd")
    # OpenBSD names its libraries libfoo.so.MAJOR.MINOR with no libfoo.so,
    # and only OpenBSD's own lld looks for that on -lfoo: upstream lld would
    # quietly take libc.a, libc++.a, libpthread.a for the driver's -lc,
    # -lc++, -lpthread (a static libc under dynamic libraries). Unversioned
    # links to the newest versions, searched before the sysroot; these
    # libraries carry a soname, so the binary still records libc.so.103.0.
    set(_shim "${CMAKE_BINARY_DIR}/openbsd-libs")
    file(MAKE_DIRECTORY "${_shim}")
    foreach(lib c c++ c++abi pthread m)
        file(GLOB _versions "${MSGA_BSD_SYSROOT}/usr/lib/lib${lib}.so.*")
        if(NOT _versions)
            message(FATAL_ERROR "bsd-clang.cmake: no lib${lib}.so.* in the sysroot")
        endif()
        list(SORT _versions COMPARE NATURAL)
        list(POP_BACK _versions _newest)
        file(CREATE_LINK "${_newest}" "${_shim}/lib${lib}.so" SYMBOLIC)
    endforeach()
    string(PREPEND _bsd_link "-L${_shim} ")
endif()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_bsd_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_bsd_link}")

set(CMAKE_FIND_ROOT_PATH "${MSGA_BSD_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The host's pkg-config, reading only the sysroot's .pc files and prefixing
# their paths with it.
set(_bsd_pc_dirs "")
string(REPLACE ":" ";" _bsd_pc_list "${_bsd_pc}")
foreach(dir ${_bsd_pc_list})
    list(APPEND _bsd_pc_dirs "${MSGA_BSD_SYSROOT}${dir}")
endforeach()
string(REPLACE ";" ":" _bsd_pc_dirs "${_bsd_pc_dirs}")
set(ENV{PKG_CONFIG_LIBDIR} "${_bsd_pc_dirs}")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${MSGA_BSD_SYSROOT}")
