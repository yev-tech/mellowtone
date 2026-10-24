#ifndef LX_INPUT_H
#define LX_INPUT_H

#include "lx.h"

/* Opens the named key devices and touch panels (descriptor names). */
void lx_input_open(const char * const * buttons, const char * const * touch, int screen_h);
void lx_input_close(void);
/* Next key press or tap, up to timeout_ms (-1: forever). */
lx_key_t lx_input_next(int timeout_ms, int * tap_y);
/* Whether `code` is held on one of the named devices (NULL: any device) at
 * some point within window_ms (the shell launcher's `keyheld CODE 1`). */
bool lx_input_key_held(const char * const * devices, int code, int window_ms);

#endif
