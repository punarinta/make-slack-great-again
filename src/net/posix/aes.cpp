// AES-128-CBC decryption over mbedTLS's single-block ECB primitive (see
// aes.h). Chaining is done here so the build needs neither MBEDTLS_CIPHER_C's
// CBC path nor the cipher-layer machinery: ~1.8 KB (AES decrypt tables)
// instead of ~15 KB.
#include "net/aes.h"

#include <mbedtls/aes.h>

#include <cstring>

namespace net {

bool aes128CbcDecrypt(
    std::string_view key, std::string_view iv, std::string_view in, std::string *out
) {
    out->clear();
    if (key.size() != 16 || iv.size() != 16 || in.empty() || in.size() % 16 != 0)
        return false;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, reinterpret_cast<const unsigned char *>(key.data()), 128) !=
        0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    std::string plain;
    plain.resize(in.size());
    unsigned char chain[16];
    std::memcpy(chain, iv.data(), 16);
    for (size_t off = 0; off < in.size(); off += 16) {
        unsigned char block[16];
        if (mbedtls_aes_crypt_ecb(
                &ctx,
                MBEDTLS_AES_DECRYPT,
                reinterpret_cast<const unsigned char *>(in.data() + off),
                block
            ) != 0) {
            mbedtls_aes_free(&ctx);
            return false;
        }
        for (int i = 0; i < 16; ++i)
            plain[off + i] = char(block[i] ^ chain[i]);
        std::memcpy(chain, in.data() + off, 16); // the ciphertext block feeds the next XOR
    }
    mbedtls_aes_free(&ctx);

    // PKCS#7: the last byte is the pad length (1..16), repeated.
    const auto pad = static_cast<unsigned char>(plain.back());
    if (pad == 0 || pad > 16 || pad > plain.size())
        return false;
    for (size_t i = plain.size() - pad; i < plain.size(); ++i)
        if (static_cast<unsigned char>(plain[i]) != pad)
            return false;
    plain.resize(plain.size() - pad);
    *out = std::move(plain);
    return true;
}

} // namespace net
