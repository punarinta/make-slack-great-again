// Our mbedTLS 3.6 configuration (MBEDTLS_CONFIG_FILE): a TLS 1.2 + 1.3
// *client* and nothing else, because every enabled module is binary size.
//
// What a public HTTPS / WSS server (Slack, Cloudflare, Google…) needs from a
// client in 2026, and no more:
//   - key exchange: ECDHE over X25519, P-256, P-384 (no DHE, no static RSA,
//     no PSK — no server we talk to requires them).
//   - AEAD only: AES-128/256-GCM and ChaCha20-Poly1305 (no CBC suites).
//   - certificates: RSA (PKCS#1 v1.5 for chains, PSS for TLS 1.3
//     handshake signatures) and ECDSA, hashed with SHA-256/384/512.
//     SHA-1 is not accepted in chains (mbedTLS's default profile).
//   - SNI on, hostname verification on (done in tls.cpp), system CAs.
//   - session resumption (TLS 1.3 tickets with psk_dhe_ke, TLS 1.2 session
//     ids and tickets): a reconnect skips the certificate chain (tls.cpp).
// Off: the server side, DTLS, renegotiation, early data, debug and error
// strings, PEM/ASN.1 writing, key generation, every legacy cipher.
//
// Randomness comes straight from the kernel (crypto::randomBytes) through
// MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG: no entropy collector, no CTR-DRBG.
//
// MSGA_MBEDTLS_COOKIE_CRYPTO (CMake option) adds what decrypting Chromium's
// cookie store needs: AES-128-CBC, PBKDF2-HMAC-SHA1 (PKCS5), SHA-1.
#pragma once

// ── System ───────────────────────────────────────────────────────────────────
#define MBEDTLS_HAVE_ASM       // bignum multiply in inline asm: smaller and faster
#define MBEDTLS_HAVE_TIME      // }
#define MBEDTLS_HAVE_TIME_DATE // } certificate validity periods are checked
#define MBEDTLS_DEPRECATED_REMOVED
// Several worker threads run handshakes at once and PSA keeps global state
// (key slots, the init flag), so the library must lock.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

// ── PSA (TLS 1.3 is built on it; with USE_PSA_CRYPTO TLS 1.2 is too, so the
// legacy ECDH / cipher record paths are not linked twice) ────────────────────
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CLIENT
#define MBEDTLS_USE_PSA_CRYPTO
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG // mbedtls_psa_external_get_random in tls.cpp
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC

// ── Big numbers, curves, public keys ────────────────────────────────────────
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MPI_MAX_SIZE 512 // RSA up to 4096 bits (default 8192: twice the stack)
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
// No precomputed base-point tables (2.3 KB): one key generation per
// handshake does not need them.
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21 // RSA-PSS: TLS 1.3 servers with RSA keys sign with it
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

// ── Hashes, MACs, KDFs ──────────────────────────────────────────────────────
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C // shares sha512.c with SHA-384; a few chains use it
#define MBEDTLS_SHA512_SMALLER
#define MBEDTLS_HKDF_C // TLS 1.3 key schedule (PSA HKDF is enabled from this)

// ── Ciphers ──────────────────────────────────────────────────────────────────
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES // 2 KB of tables instead of 8 KB
// AES-NI + PCLMUL for AES and GCM when the CPU has them (runtime check, the
// tables stay as the fallback): +1.3 KB for constant-time, ~10× faster
// records. Compiles to nothing off x86-64.
#define MBEDTLS_AESNI_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

// ── X.509 ────────────────────────────────────────────────────────────────────
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_OID_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_REMOVE_INFO // no human-readable certificate dumps
// Roots are looked up per handshake from our DER store (tls.cpp) instead of
// being parsed into one big chain; PEM is decoded there too (crypto::base64),
// so neither PEM nor base64 from mbedTLS is built.
#define MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK
#define MBEDTLS_X509_MAX_INTERMEDIATE_CA 6
// Not only PSS-signed certificates: in 3.6 this also gates offering the
// rsa_pss_rsae_* signature algorithms, without which every TLS 1.3 server
// with an RSA key (slack.com) answers handshake_failure.
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

// ── TLS ──────────────────────────────────────────────────────────────────────
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
// Resumption: a ticket plus a fresh ECDHE (psk_dhe_ke, what servers offer;
// pure PSK without forward secrecy stays off).
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE // middleboxes drop "pure" 1.3 handshakes
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE // required by the 1.3 client code
// A server may send full 16 KB records, so input stays at the maximum; our
// own writes are split into 4 KB records (saves 12 KB per connection).
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
// Only these suites are compiled into the table (and offered).
#define MBEDTLS_SSL_CIPHERSUITES                                                                   \
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256, MBEDTLS_TLS1_3_AES_256_GCM_SHA384,                          \
        MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256, MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,  \
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,                                             \
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,                                           \
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,                                             \
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,                                     \
        MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256

// ── Chromium cookie store decryption ────────────────────────────────────────
// The Slack desktop app's cookie store is AES-128-CBC. We do the CBC chaining
// and PKCS#7 ourselves over mbedtls_aes_crypt_ecb (net/posix/aes.cpp), and
// PBKDF2-HMAC-SHA1 over crypto::sha1, so mbedTLS needs only AES *decryption*
// (the +1.8 KB of tables that MBEDTLS_BLOCK_CIPHER_NO_DECRYPT would drop) —
// not CBC in the cipher layer, nor PKCS5/SHA-1 (which together cost +15.3 KB).
// So we simply leave MBEDTLS_BLOCK_CIPHER_NO_DECRYPT off.
//
// MSGA_MBEDTLS_COOKIE_CRYPTO (CMake, default OFF) instead builds the full
// mbedTLS path, kept as an escape hatch; nothing uses it now.
#ifdef MSGA_MBEDTLS_COOKIE_CRYPTO
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_PADDING_PKCS7
#define MBEDTLS_PKCS5_C
#define MBEDTLS_SHA1_C
#endif
