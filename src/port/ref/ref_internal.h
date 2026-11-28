/* Shared by the reference port's files (not part of the contract). */
#ifndef PORT_REF_INTERNAL_H
#define PORT_REF_INTERNAL_H

#include "port/port.h"

#include <stddef.h>
#include <stdint.h>

/* errno → PORT_E_*. */
int   ref_errno(int e);
/* snprintf-style copy into a caller buffer: returns strlen(s). */
int   ref_copy_out(const char *s, char *buf, size_t cap);
/* Duplicate n bytes plus a NUL (NULL on no memory). */
char *ref_strndup(const char *s, size_t n);
/* A detail message into a caller's buffer (may be NULL). */
void  ref_detail(char *buf, size_t cap, const char *msg);

/* Single-surface mode (what a port without PORT_CAP_WINDOWS does, PEOS's):
 * $MSGA_PORT_REF_SURFACE = "<w>x<h>" (logical px) or "auto" (the size the
 * first window asks for) turns it on, read once. 1 when on, with the
 * surface's size into *w / *h (0 x 0 for "auto"). */
int ref_single_surface(int32_t *w, int32_t *h);

#endif
