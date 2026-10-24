#ifndef HIBY_SYS_CLIENT_H
#define HIBY_SYS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* Serves /var/run/sys_client as the stock player does (R3 Pro), so
 * sys_server command results and the BT ALSA plugin's messages are answered.
 * Call once at startup. */
bool hiby_sys_client_start(void);

/* Sends `cmd` to sys_server and waits up to timeout_ms for a result message
 * starting with one of result_prefixes (NULL-terminated), e.g.
 * { "WIFI:SCAN:OK", "WIFI:SCAN:FAIL", NULL }. The message is copied to out.
 * Returns false on timeout. */
bool hiby_sys_request(const char * cmd, const char * const * result_prefixes, int timeout_ms,
                      char * out, size_t out_size);

/* Tests only: serve this path instead of /var/run/sys_client (before start). */
void hiby_sys_client_test_set_path(const char * path);

#endif
