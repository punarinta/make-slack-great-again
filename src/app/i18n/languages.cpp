#include "app/i18n/languages.h"

#include "stb/stb_config.h"
#include "stb/stb_image.h"

#include <cstdlib>

namespace app_i18n {

std::string inflate(const unsigned char *data, size_t len, size_t size) {
    // The PNG decoder's inflate: already in the binary, so a table costs only
    // its compressed bytes.
    int   got = 0;
    char *raw = stbi_zlib_decode_malloc_guesssize_headerflag(
        reinterpret_cast<const char *>(data), int(len), int(size), &got, 0
    );
    std::string out;
    if (raw && size_t(got) == size)
        out.assign(raw, size);
    std::free(raw);
    return out;
}

} // namespace app_i18n
