#include "core/input.h"

#include <cmath>
#if defined(PLAT_PORT)
#include "port/port_sys.h"
#else
#include <chrono>
#endif

namespace plat::core {

int ClickCounter::press(
    int button, double x, double y, uint32_t timeMs, uint32_t intervalMs, double slopX, double slopY
) {
    const bool again = _clicks > 0 && button == _button && uint32_t(timeMs - _time) <= intervalMs &&
                       std::abs(x - _x) <= slopX && std::abs(y - _y) <= slopY;
    _clicks          = again ? _clicks + 1 : 1;
    _button          = button;
    _time            = timeMs;
    _x               = x;
    _y               = y;
    return _clicks;
}

Key keyFromPunctuation(uint32_t c) {
    switch (c) {
    case '-':
        return Key::Minus;
    case '=':
        return Key::Equal;
    case '[':
        return Key::BracketLeft;
    case ']':
        return Key::BracketRight;
    case '\\':
        return Key::Backslash;
    case ';':
        return Key::Semicolon;
    case '\'':
        return Key::Apostrophe;
    case '`':
        return Key::Grave;
    case ',':
        return Key::Comma;
    case '.':
        return Key::Period;
    case '/':
        return Key::Slash;
    default:
        return Key::Unknown;
    }
}

Key keyFromAscii(uint32_t c) {
    if (c >= 'a' && c <= 'z')
        return Key(int(Key::A) + int(c - 'a'));
    if (c >= 'A' && c <= 'Z')
        return Key(int(Key::A) + int(c - 'A'));
    if (c >= '0' && c <= '9')
        return Key(int(Key::Num0) + int(c - '0'));
    return keyFromPunctuation(c);
}

uint32_t monotonicMs() {
#if defined(PLAT_PORT) // the port's clock, as every other in a port build
    return uint32_t(port_time_monotonic_ns() / 1000000);
#else
    using namespace std::chrono;
    return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

} // namespace plat::core
