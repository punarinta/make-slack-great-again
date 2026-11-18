#include "base/crypto.h"
#include "base/str.h"
#include "prim/bytes.h"

#include <algorithm>
#include <cstring>

#if defined(MSGA_PORT)
#include "port/port.h"
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <unistd.h> // getentropy
#endif

namespace crypto {

namespace {

inline uint32_t rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}
inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

// SHA-1's Merkle–Damgård padding (big-endian, 64-byte blocks; Sha256 pads
// the same way): feeds every block of data + 0x80 + zeros + bit length.
template <typename F>
void eachBlock(std::string_view data, F &&block) {
    const uint64_t bits = uint64_t(data.size()) * 8;
    size_t         i    = 0;
    for (; i + 64 <= data.size(); i += 64)
        block(reinterpret_cast<const uint8_t *>(data.data() + i));
    uint8_t      tail[128] = {};
    const size_t rest      = data.size() - i;
    std::memcpy(tail, data.data() + i, rest);
    tail[rest]       = 0x80;
    const size_t len = rest + 9 <= 64 ? 64 : 128;
    for (int k = 0; k < 8; ++k)
        tail[len - 1 - k] = uint8_t(bits >> (8 * k));
    block(tail);
    if (len == 128)
        block(tail + 64);
}

template <size_t N>
void store(std::array<uint8_t, N> &out, const uint32_t *h) {
    for (size_t i = 0; i < N / 4; ++i)
        for (int k = 0; k < 4; ++k)
            out[i * 4 + k] = uint8_t(h[i] >> (24 - 8 * k));
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode64(std::string_view data, bool url) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    const auto *p   = reinterpret_cast<const uint8_t *>(data.data());
    size_t      i   = 0;
    auto        put = [&](uint32_t v) {
        const char c = kB64[v & 63];
        out += url && c == '+' ? '-' : url && c == '/' ? '_' : c;
    };
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t v = uint32_t(p[i]) << 16 | uint32_t(p[i + 1]) << 8 | p[i + 2];
        put(v >> 18), put(v >> 12), put(v >> 6), put(v);
    }
    const size_t rest = data.size() - i;
    if (rest) {
        const uint32_t v = uint32_t(p[i]) << 16 | (rest > 1 ? uint32_t(p[i + 1]) << 8 : 0);
        put(v >> 18), put(v >> 12);
        if (rest > 1)
            put(v >> 6);
        if (!url)
            out.append(rest == 1 ? "==" : "=");
    }
    return out;
}

} // namespace

namespace {

// One 64-byte block into SHA-256's state.
void sha256Block(uint32_t *h, const uint8_t *b) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = prim::be32(b + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    std::memcpy(v, h, sizeof v);
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
        const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
        const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
        const uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        std::memmove(v + 1, v, 7 * sizeof(uint32_t));
        v[4] += t1;
        v[0] = t1 + s0 + mj;
    }
    for (int i = 0; i < 8; ++i)
        h[i] += v[i];
}

} // namespace

Sha256::Sha256()
    : _h{0x6a09e667,
         0xbb67ae85,
         0x3c6ef372,
         0xa54ff53a,
         0x510e527f,
         0x9b05688c,
         0x1f83d9ab,
         0x5be0cd19} {}

void Sha256::update(std::string_view data) {
    const auto *p    = reinterpret_cast<const uint8_t *>(data.data());
    size_t      n    = data.size();
    size_t      used = size_t(_len % 64);
    _len += n;
    if (used) { // top up the partial block first
        const size_t take = std::min(n, 64 - used);
        std::memcpy(_buf + used, p, take);
        p += take, n -= take, used += take;
        if (used < 64)
            return;
        sha256Block(_h, _buf);
    }
    for (; n >= 64; p += 64, n -= 64)
        sha256Block(_h, p);
    std::memcpy(_buf, p, n);
}

std::array<uint8_t, 32> Sha256::finish() {
    // Merkle–Damgård padding: 0x80, zeros, then the bit length.
    const uint64_t bits      = _len * 8;
    const size_t   used      = size_t(_len % 64);
    uint8_t        tail[128] = {};
    std::memcpy(tail, _buf, used);
    tail[used]       = 0x80;
    const size_t len = used + 9 <= 64 ? 64 : 128;
    for (int k = 0; k < 8; ++k)
        tail[len - 1 - k] = uint8_t(bits >> (8 * k));
    sha256Block(_h, tail);
    if (len == 128)
        sha256Block(_h, tail + 64);
    std::array<uint8_t, 32> out;
    store(out, _h);
    return out;
}

std::array<uint8_t, 32> sha256(std::string_view data) {
    Sha256 s;
    s.update(data);
    return s.finish();
}

std::array<uint8_t, 20> sha1(std::string_view data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    eachBlock(data, [&](const uint8_t *b) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = prim::be32(b + 4 * i);
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, kk;
            if (i < 20)
                f = (bb & c) | (~bb & d), kk = 0x5A827999;
            else if (i < 40)
                f = bb ^ c ^ d, kk = 0x6ED9EBA1;
            else if (i < 60)
                f = (bb & c) | (bb & d) | (c & d), kk = 0x8F1BBCDC;
            else
                f = bb ^ c ^ d, kk = 0xCA62C1D6;
            const uint32_t t = rotl(a, 5) + f + e + kk + w[i];
            e = d, d = c, c = rotl(bb, 30), bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    });
    std::array<uint8_t, 20> out;
    store(out, h);
    return out;
}

std::array<uint8_t, 20> hmacSha1(std::string_view key, std::string_view data) {
    uint8_t block[64] = {};
    if (key.size() > 64) {
        const auto h = sha1(key);
        std::memcpy(block, h.data(), h.size());
    } else {
        std::memcpy(block, key.data(), key.size());
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5c;
    }
    std::string inner;
    inner.reserve(64 + data.size());
    inner.append(reinterpret_cast<const char *>(ipad), 64);
    inner.append(data);
    const auto  innerHash = sha1(inner);
    std::string outer;
    outer.reserve(64 + innerHash.size());
    outer.append(reinterpret_cast<const char *>(opad), 64);
    outer.append(reinterpret_cast<const char *>(innerHash.data()), innerHash.size());
    return sha1(outer);
}

std::string
pbkdf2Sha1(std::string_view password, std::string_view salt, int iterations, size_t len) {
    std::string out;
    out.reserve(len);
    uint32_t block = 1;
    while (out.size() < len) {
        // U1 = HMAC(password, salt || INT_BE32(block)); Ui = HMAC(password, Ui-1).
        std::string seed(salt);
        for (int k = 3; k >= 0; --k)
            seed += char(block >> (8 * k));
        std::array<uint8_t, 20> u = hmacSha1(password, seed);
        std::array<uint8_t, 20> t = u;
        for (int it = 1; it < iterations; ++it) {
            u = hmacSha1(
                password, std::string_view(reinterpret_cast<const char *>(u.data()), u.size())
            );
            for (size_t i = 0; i < t.size(); ++i)
                t[i] ^= u[i];
        }
        const size_t take = std::min(t.size(), len - out.size());
        out.append(reinterpret_cast<const char *>(t.data()), take);
        ++block;
    }
    return out;
}

bool randomBytes(void *out, size_t n) {
#if defined(MSGA_PORT)
    return port_random(out, n) == PORT_OK;
#elif defined(_WIN32)
    return BCryptGenRandom(
               nullptr, static_cast<PUCHAR>(out), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG
           ) == 0;
#elif defined(__APPLE__)
    arc4random_buf(out, n);
    return true;
#else
    // getentropy (POSIX.1-2024; glibc 2.25, musl, the BSDs) rather than
    // Linux's getrandom: the same kernel source, at most 256 bytes a call.
    auto *p = static_cast<uint8_t *>(out);
    while (n > 0) {
        const size_t chunk = std::min<size_t>(n, 256);
        if (getentropy(p, chunk) != 0)
            return false;
        p += chunk;
        n -= chunk;
    }
    return true;
#endif
}

std::string base64(std::string_view data) {
    return encode64(data, false);
}

std::string base64url(std::string_view data) {
    return encode64(data, true);
}

bool base64Decode(std::string_view in, std::string *out) {
    out->clear();
    out->reserve(in.size() / 4 * 3 + 3);
    uint32_t acc = 0;
    int      n   = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+' || c == '-')
            v = 62;
        else if (c == '/' || c == '_')
            v = 63;
        else if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        else
            return false;
        acc = acc << 6 | uint32_t(v);
        if (++n == 4) {
            *out += char(acc >> 16);
            *out += char(acc >> 8);
            *out += char(acc);
            acc = 0, n = 0;
        }
    }
    if (n == 1)
        return false;
    if (n == 2)
        *out += char(acc >> 4);
    else if (n == 3)
        *out += char(acc >> 10), *out += char(acc >> 2);
    return true;
}

std::string hex(std::string_view data) {
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out += str::kHexLower[c >> 4];
        out += str::kHexLower[c & 15];
    }
    return out;
}

std::string randomHex(size_t n) {
    uint8_t b[64];
    if (n > sizeof b || !randomBytes(b, n))
        return {};
    return hex({reinterpret_cast<const char *>(b), n});
}

std::string uuid4() {
    uint8_t b[16];
    if (!randomBytes(b, sizeof b))
        return {};
    b[6]          = uint8_t((b[6] & 0x0F) | 0x40); // version 4
    b[8]          = uint8_t((b[8] & 0x3F) | 0x80); // variant 10
    std::string s = hex({reinterpret_cast<const char *>(b), sizeof b});
    for (size_t at : {8, 13, 18, 23})
        s.insert(at, 1, '-');
    return s;
}

} // namespace crypto
