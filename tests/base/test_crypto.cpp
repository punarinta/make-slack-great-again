#include "base/crypto.h"
#include "base/process.h"
#include "support/test.h"

#include <cstring>
#ifndef _WIN32
#include <time.h>
#endif
#include <string>

TEST("crypto: sha256 / sha1 test vectors") {
    using crypto::bytes;
    using crypto::hex;
    CHECK(
        hex(bytes(crypto::sha256(""))) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    CHECK(
        hex(bytes(crypto::sha256("abc"))) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    // Two blocks of padding (56 bytes of input).
    CHECK(
        hex(bytes(crypto::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    );
    CHECK(hex(bytes(crypto::sha1("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(bytes(crypto::sha1(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    std::string million(1000000, 'a');
    CHECK(
        hex(bytes(crypto::sha256(million))) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
    );
}

TEST("crypto: Sha256 in pieces hashes as the whole") {
    using crypto::bytes;
    std::string data;
    for (int i = 0; i < 1000; ++i)
        data += char(i * 7 + 3);
    // Every split, piece sizes across the 64-byte block edge.
    for (size_t step : {size_t(1), size_t(7), size_t(63), size_t(64), size_t(65), size_t(999)}) {
        for (size_t len : {size_t(0), size_t(55), size_t(56), size_t(64), size_t(1000)}) {
            crypto::Sha256         s;
            const std::string_view d = std::string_view(data).substr(0, len);
            for (size_t i = 0; i < d.size(); i += step)
                s.update(d.substr(i, step));
            CHECK(bytes(s.finish()) == bytes(crypto::sha256(d)));
        }
    }
}

TEST("crypto: hmac-sha1 (RFC 2202)") {
    using crypto::bytes;
    using crypto::hex;
    // Case 1: 20-byte 0x0b key, "Hi There".
    CHECK(
        hex(bytes(crypto::hmacSha1(std::string(20, '\x0b'), "Hi There"))) ==
        "b617318655057264e28bc0b6fb378c8ef146be00"
    );
    // Case 2: key "Jefe", "what do ya want for nothing?".
    CHECK(
        hex(bytes(crypto::hmacSha1("Jefe", "what do ya want for nothing?"))) ==
        "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"
    );
    // Case 3: 20-byte 0xaa key, 50 bytes of 0xdd.
    CHECK(
        hex(bytes(crypto::hmacSha1(std::string(20, '\xaa'), std::string(50, '\xdd')))) ==
        "125d7342b9ac11cd91a39af48aa17b4f63f175d3"
    );
    // A key longer than the block (80 bytes of 0xaa), hashed first.
    CHECK(
        hex(bytes(
            crypto::hmacSha1(
                std::string(80, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"
            )
        )) == "aa4ae5e15272d00e95705637ce8a3b55ed402112"
    );
}

TEST("crypto: pbkdf2-hmac-sha1 (RFC 6070)") {
    using crypto::hex;
    CHECK(
        hex(crypto::pbkdf2Sha1("password", "salt", 1, 20)) ==
        "0c60c80f961f0e71f3a9b524af6012062fe037a6"
    );
    CHECK(
        hex(crypto::pbkdf2Sha1("password", "salt", 2, 20)) ==
        "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"
    );
    CHECK(
        hex(crypto::pbkdf2Sha1("password", "salt", 4096, 20)) ==
        "4b007901b765489abead49d926f721d065a429c1"
    );
    CHECK(
        hex(crypto::pbkdf2Sha1(
            "passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 25
        )) == "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"
    );
    CHECK(
        hex(crypto::pbkdf2Sha1(std::string("pass\0word", 9), std::string("sa\0lt", 5), 4096, 16)) ==
        "56fa6aa75548099dcc37d7f03425e0c3"
    );
    // Chromium's fixed cookie-key derivation: 1 iteration, "saltysalt", 16 bytes.
    CHECK(crypto::pbkdf2Sha1("peanuts", "saltysalt", 1, 16).size() == 16);
}

TEST("crypto: base64 and base64url round trips") {
    CHECK(crypto::base64("") == "");
    CHECK(crypto::base64("f") == "Zg==");
    CHECK(crypto::base64("fo") == "Zm8=");
    CHECK(crypto::base64("foo") == "Zm9v");
    CHECK(crypto::base64("foobar") == "Zm9vYmFy");
    CHECK(crypto::base64url("\xfb\xff") == "-_8");
    // RFC 7636 appendix B: the PKCE S256 challenge.
    CHECK(
        crypto::base64url(
            crypto::bytes(crypto::sha256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"))
        ) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
    );
    std::string out;
    CHECK(crypto::base64Decode("Zm9vYmFy", &out) && out == "foobar");
    CHECK(crypto::base64Decode("Zm9vYg", &out) && out == "foob"); // no padding
    CHECK(crypto::base64Decode("-_8", &out) && out == "\xfb\xff");
    CHECK(!crypto::base64Decode("Zm9v!", &out));
    uint8_t a[16] = {}, b[16] = {};
    CHECK(crypto::randomBytes(a, sizeof a) && crypto::randomBytes(b, sizeof b));
    CHECK(std::memcmp(a, b, sizeof a) != 0);
}

#if !defined(_WIN32) && !defined(__PEOS__) // /bin/sh: PEOS has no shell to run
TEST("process: start, run, kill the group") {
    const std::string sh = base::findExecutable("sh");
    REQUIRE(!sh.empty());
    CHECK(base::findExecutable("definitely-not-a-program-xyz").empty());
    base::Process p;
    // A child that starts a grandchild: killing the group stops both.
    REQUIRE(p.start(sh, {"-c", "sleep 30 & wait"}));
    CHECK(p.pid() > 0);
    CHECK(p.running());
    p.kill(false);
    for (int i = 0; i < 200 && p.running(); ++i) {
        struct timespec ts{0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    CHECK(!p.running());
    base::Process q;
    CHECK(!q.start("/nonexistent/binary", {}));
}
#endif

TEST("crypto: fnv1a 64-bit, and continuing a hash") {
    CHECK(crypto::fnv1a("") == crypto::kFnvOffset);
    // The tree's basis (see crypto.h), so not the published FNV test vectors.
    CHECK(crypto::fnv1a("a") == 0x44bd8ad473cd9906ull);
    CHECK(crypto::fnv1a("foobar") == 0x88fad7c0a8ff07f2ull);
    CHECK(crypto::fnv1a("bar", crypto::fnv1a("foo")) == crypto::fnv1a("foobar"));
}

TEST("crypto: uuid4 shape and version bits, randomHex") {
    const std::string a = crypto::uuid4(), b = crypto::uuid4();
    CHECK(a.size() == 36);
    CHECK(a != b);
    for (size_t i = 0; i < a.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            CHECK(a[i] == '-');
        else
            CHECK((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f'));
    }
    CHECK(a[14] == '4');                          // version 4
    CHECK(std::strchr("89ab", a[19]) != nullptr); // variant 10
    CHECK(crypto::randomHex(6).size() == 12);
    CHECK(crypto::randomHex(0).empty());
    CHECK(crypto::randomHex(65).empty()); // past the cap
}
