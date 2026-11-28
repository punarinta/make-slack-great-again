// Precompiled header of the test trees (src/tools/pch.cmake force-includes it
// into every C++ file of msga's own targets and plat when MSGA_BUILD_TESTS is
// on; release builds never see it).
//
// Standard headers only, the ones nearly every file reaches through plat.h,
// base/str.h and the model: of 292 C++ files, 280+ parse <string>/<vector>,
// 210+ <functional>/<memory>/<unordered_map>, 186 <optional>, 131 <algorithm>.
// No project headers: they change often (a PCH rebuild recompiles everything)
// and a file must still build without the PCH (every release build does).
//
// Measured 2026-10 on clean Linux test trees (24 threads, paired runs, the
// machine shared): total compile CPU down 22% (±5) with GCC 15 at Debug and
// MinSizeRel, 40% (±2) with Clang 21; rebuilding after a touch of base/str.h
// (111 files) -18% GCC, -44% Clang; a one-.cpp rebuild unchanged. Wall time
// gains less on GCC (every file waits for the .gch, 76 MB at -g, first). A PCH
// per target instead of the shared one was slower than none (+5% CPU). The
// smaller set without <algorithm>, <cmath>, <cstdio>, <cstdlib>, <cstring>,
// <limits>, <utility> saved 2 points less; adding <map>, <mutex>, <atomic>,
// <deque>, <set>, <unordered_set> no more. Keep <chrono> (and <iomanip>) out:
// in libstdc++'s C++20 <chrono> pulls in <iomanip>, and the std::quoted that
// ADL then finds breaks str::concat({.., quoted(x)}) calls in browser_login.cpp.
// Re-measure before growing this list.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
