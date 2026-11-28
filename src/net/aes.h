// A sliver of symmetric crypto the app needs outside TLS: AES-128-CBC
// decryption, for the Slack desktop app's Chromium cookie store. It lives in
// net/ because that is where mbedTLS is linked (the OS stacks on Windows and
// macOS provide their own); app code includes this, never an mbedTLS header.
//
// Only where mbedTLS is built (Linux) is there an implementation; elsewhere
// it returns false, and the local-import path that uses it is not compiled.
#pragma once

#include <string>
#include <string_view>

namespace net {

// CBC-decrypts `in` (a whole number of 16-byte blocks) with a 16-byte `key`
// and 16-byte `iv`, strips PKCS#7 padding, and writes the plaintext to *out.
// False on a bad size, a bad key/iv length, or invalid padding.
bool aes128CbcDecrypt(
    std::string_view key, std::string_view iv, std::string_view in, std::string *out
);

} // namespace net
