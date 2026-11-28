// Includes screens::Context (app/screens/common/context.h). That header names
// model types unqualified (ConvRef, Store, …) inside namespace screens, so
// the using-declarations it needs come first. Redundant (and harmless) once
// context.h brings them itself.
#pragma once

#include "app/model/backend.h"

namespace screens {
using model::Backend;
using model::ConvRef;
using model::Store;
using model::Ts;
using model::UserRef;
} // namespace screens

#include "app/screens/common/context.h"
