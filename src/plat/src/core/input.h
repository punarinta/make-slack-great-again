// Input helpers every backend shares: the multi-click counter and the
// character-to-Key table the layout-aware key mappings use.
#pragma once

#include "plat/plat.h"

#include <cstdint>

namespace plat::core {

// Counts presses in a run: 1 for a fresh press, one more than the last when
// it is the same button, at most intervalMs after the previous press (32-bit
// millisecond clocks: wrap-safe) and within slop of where it was. Each
// backend passes its OS's double-click time and distance.
class ClickCounter {
public:
    int press(
        int      button,
        double   x,
        double   y,
        uint32_t timeMs,
        uint32_t intervalMs,
        double   slopX,
        double   slopY
    );
    int  clicks() const { return _clicks; }
    void reset() { _clicks = 0; } // the next press starts a new run

private:
    int      _clicks = 0, _button = -1;
    uint32_t _time = 0;
    double   _x = 0, _y = 0;
};

// The Key that types the ASCII punctuation c ('-', '=', '[', ']', '\\', ';',
// '\'', '`', ',', '.', '/'), else Key::Unknown.
Key keyFromPunctuation(uint32_t c);
// Also letters (either case) and digits.
Key keyFromAscii(uint32_t c);

// A wrapping millisecond clock (steady) for ClickCounter on backends whose
// events carry no time of their own.
uint32_t monotonicMs();

} // namespace plat::core
