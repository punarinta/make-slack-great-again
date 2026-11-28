#include "core/image_util.h"

#include <algorithm>
#include <cmath>

namespace plat::core {

uint32_t unpremultiply(uint32_t p) {
    const uint32_t a = p >> 24;
    if (a == 0)
        return 0;
    if (a == 255)
        return p;
    auto ch = [a](uint32_t v) { return std::min<uint32_t>(255, (v * 255 + a / 2) / a); };
    return a << 24 | ch((p >> 16) & 0xff) << 16 | ch((p >> 8) & 0xff) << 8 | ch(p & 0xff);
}

const Image *pickImage(const std::vector<Image> &sizes, int px, double scale, bool onlyAtScale) {
    auto usable = [](const Image &i) {
        return !i.empty() && i.pixels.size() >= size_t(i.width) * size_t(i.height);
    };
    auto atScale  = [scale](const Image &i) { return std::abs(i.scale - scale) < 0.01; };
    bool restrict = false;
    if (onlyAtScale)
        for (const auto &i : sizes)
            restrict |= usable(i) && atScale(i);
    const Image *best = nullptr;
    int          bm   = 0;
    for (const auto &i : sizes) {
        if (!usable(i) || (restrict && !atScale(i)))
            continue;
        const int  m   = std::max(i.width, i.height);
        const bool big = m >= px, bestBig = best && bm >= px;
        const bool closerScale = best && m == bm && scale > 0 &&
                                 std::abs(i.scale - scale) < std::abs(best->scale - scale);
        if (!best || (big && (!bestBig || m < bm)) || (!big && !bestBig && m > bm) || closerScale) {
            best = &i;
            bm   = m;
        }
    }
    return best;
}

Image scaleImage(const Image &src, int w, int h) {
    Image out{w, h, std::vector<uint32_t>(size_t(std::max(w, 0)) * size_t(std::max(h, 0)), 0)};
    if (src.empty() || src.pixels.size() < size_t(src.width) * size_t(src.height) || w <= 0 ||
        h <= 0)
        return out;
    if (w == src.width && h == src.height) {
        out.pixels = src.pixels;
        return out;
    }
    const double sx = double(src.width) / w, sy = double(src.height) / h;
    auto         at = [&](int x, int y) {
        x = std::clamp(x, 0, src.width - 1);
        y = std::clamp(y, 0, src.height - 1);
        return src.pixels[size_t(y) * size_t(src.width) + size_t(x)];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc[4] = {}, area = 0;
            if (sx > 1 || sy > 1) {
                const double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
                for (int iy = int(y0); iy < std::min(src.height, int(std::ceil(y1))); ++iy) {
                    const double wy = std::min(y1, iy + 1.0) - std::max(y0, double(iy));
                    for (int ix = int(x0); ix < std::min(src.width, int(std::ceil(x1))); ++ix) {
                        const double   a = wy * (std::min(x1, ix + 1.0) - std::max(x0, double(ix)));
                        const uint32_t p = at(ix, iy);
                        for (int c = 0; c < 4; ++c)
                            acc[c] += a * ((p >> (c * 8)) & 0xff);
                        area += a;
                    }
                }
            } else {
                const double   fx = (x + 0.5) * sx - 0.5, fy = (y + 0.5) * sy - 0.5;
                const int      ix = int(std::floor(fx)), iy = int(std::floor(fy));
                const double   ax = fx - ix, ay = fy - iy;
                const uint32_t q[4] = {
                    at(ix, iy), at(ix + 1, iy), at(ix, iy + 1), at(ix + 1, iy + 1)
                };
                const double wq[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
                for (int c = 0; c < 4; ++c)
                    for (int j = 0; j < 4; ++j)
                        acc[c] += wq[j] * ((q[j] >> (c * 8)) & 0xff);
                area = 1;
            }
            uint32_t v = 0;
            for (int c = 0; c < 4; ++c)
                v |= uint32_t(std::clamp(int(std::lround(area > 0 ? acc[c] / area : 0)), 0, 255))
                     << (c * 8);
            out.pixels[size_t(y) * size_t(w) + size_t(x)] = v;
        }
    }
    return out;
}

} // namespace plat::core
