# Cross-compile plat for 64-bit Windows with mingw-w64 from Linux:
#   cmake -S src/plat -B build-plat-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=src/plat/cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Debug
# The -posix compiler variants use winpthreads, which std::thread/std::mutex
# need (the -win32 variants lack them before GCC 13).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(PLAT_MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "mingw-w64 target triple")
set(CMAKE_C_COMPILER ${PLAT_MINGW_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${PLAT_MINGW_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER ${PLAT_MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${PLAT_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Lets ctest run the unit tests through Wine when it is installed (the
# selftest needs a display: scripts/plat-selftest-wine.sh).
find_program(PLAT_WINE wine)
if(PLAT_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${PLAT_WINE})
endif()
