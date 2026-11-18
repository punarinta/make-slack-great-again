/* Compiles every contract header as plain C11 (see CMakeLists.txt). */
#include "port/port.h"
#include "port/port_test.h"

/* ISO C forbids an empty translation unit. */
int port_header_check(void);
int port_header_check(void) {
    return port_check_abi(PORT_ABI_VERSION) + PORT_KEY_COUNT + (int)sizeof(port_event);
}
