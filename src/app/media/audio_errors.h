// The user-facing wording of a plat audio failure (plat/audio.h reports codes),
// translated.
#pragma once

#include "plat/audio.h"

#include <string>

namespace media {

std::string audioErrorText(const plat::audio::Failure &f);

} // namespace media
