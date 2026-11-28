# The Linux (POSIX) transport: our own HTTP/1.1 + WebSocket client over
# non-blocking sockets, with TLS from mbedTLS 3.6 LTS built here from source
# with our own trimmed config (posix/mbedtls_config.h). Included from
# net/CMakeLists.txt, so CMAKE_CURRENT_SOURCE_DIR is src/net.

# Default OFF: the Chromium cookie store is decrypted with our own CBC +
# PBKDF2 (net/posix/aes.cpp, crypto::pbkdf2Sha1) over mbedTLS's AES-ECB and
# crypto::sha1, which is ~13 KB smaller than mbedTLS's CBC/PKCS5/SHA-1.
option(MSGA_MBEDTLS_COOKIE_CRYPTO
    "Build AES-CBC + PBKDF2-HMAC-SHA1 into mbedTLS instead of doing it ourselves" OFF)

# ── mbedTLS ─────────────────────────────────────────────────────────────────
# The official release tarball (it carries the generated sources the git
# archive lacks), pinned by hash. SOURCE_SUBDIR points at nothing so its own
# CMakeLists (programs, tests, its flags) is never added: we compile only the
# library sources this config needs. Offline builds: pass
# -DFETCHCONTENT_SOURCE_DIR_MSGA_MBEDTLS=<unpacked mbedtls-3.6.7>.
include(FetchContent)
FetchContent_Declare(msga_mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-here)
FetchContent_MakeAvailable(msga_mbedtls)
set(_mbed ${msga_mbedtls_SOURCE_DIR})

# Every source that is non-empty under our config (the rest compile to
# nothing): measured by building library/*.c with it.
set(_mbed_sources
    aes aesni asn1parse asn1write base64 bignum bignum_core cipher cipher_wrap
    chacha20 chachapoly constant_time ecdh ecdsa ecp ecp_curves gcm hkdf md
    oid pk pk_ecc pk_wrap pkparse platform_util poly1305 psa_crypto
    psa_crypto_aead psa_crypto_cipher psa_crypto_client
    psa_crypto_driver_wrappers_no_static psa_crypto_ecp psa_crypto_hash
    psa_crypto_mac psa_crypto_rsa psa_crypto_slot_management psa_util rsa
    rsa_alt_helpers sha256 sha512 ssl_ciphersuites ssl_client ssl_msg ssl_tls
    ssl_tls12_client ssl_tls13_client ssl_tls13_generic ssl_tls13_keys
    threading x509 x509_crt)
if(MSGA_MBEDTLS_COOKIE_CRYPTO)
    list(APPEND _mbed_sources pkcs5 sha1)
endif()
list(TRANSFORM _mbed_sources PREPEND ${_mbed}/library/)
list(TRANSFORM _mbed_sources APPEND .c)

add_library(msga_mbedtls STATIC ${_mbed_sources})
target_include_directories(msga_mbedtls SYSTEM PUBLIC ${_mbed}/include)
target_include_directories(msga_mbedtls PRIVATE ${_mbed}/library)
target_compile_definitions(msga_mbedtls PUBLIC
    "MBEDTLS_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/posix/mbedtls_config.h\""
    $<$<BOOL:${MSGA_MBEDTLS_COOKIE_CRYPTO}>:MSGA_MBEDTLS_COOKIE_CRYPTO>)
# Third-party code: not msga_flags (its warnings are not ours), but the same
# size discipline. -Os only where the build type is about size.
target_compile_options(msga_mbedtls PRIVATE
    -ffunction-sections -fdata-sections -fvisibility=hidden -w
    $<$<CONFIG:MinSizeRel>:-Os -fno-asynchronous-unwind-tables -fno-unwind-tables>)
find_package(Threads REQUIRED)
target_link_libraries(msga_mbedtls PUBLIC Threads::Threads)

# ── The transport ───────────────────────────────────────────────────────────
target_sources(msga_net PRIVATE
    posix/socket.cpp
    posix/loopback.cpp
    posix/tls.cpp
    posix/http.cpp
    posix/ws.cpp
    posix/aes.cpp)
target_link_libraries(msga_net PRIVATE msga_mbedtls)
