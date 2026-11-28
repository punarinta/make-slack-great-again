/* HarfBuzz's lock (hb-mutex.hh, "defined externally") over the port
 * contract's port_mutex, for port builds (font_libs.cmake force-includes
 * it): the core reaches no OS threads library, pthreads included. */
#ifndef MSGA_HB_PORT_MUTEX_H
#define MSGA_HB_PORT_MUTEX_H

#include "port/port_sys.h"

typedef port_mutex *hb_mutex_impl_t;
#define hb_mutex_impl_init(M) port_mutex_create(M)
#define hb_mutex_impl_lock(M) port_mutex_lock(*(M))
#define hb_mutex_impl_unlock(M) port_mutex_unlock(*(M))
#define hb_mutex_impl_finish(M) port_mutex_destroy(*(M))

#endif
