// Between the port adapter's files (port_app.cpp, port_audio.cpp).
#pragma once

#include "plat/plat.h"

#include "port/port.h"

namespace plat {

// The port_app behind an App made by createPortApp.
port_app *portApp(App &app);

} // namespace plat
