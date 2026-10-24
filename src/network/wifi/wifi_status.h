#ifndef WIFI_STATUS_H
#define WIFI_STATUS_H

#include <stdbool.h>
#include <stddef.h>

/* Cached link state from the Wi-Fi core (src/network/wifi/), kept current
 * by wpa_supplicant events and a 10 s signal refresh while the screen is
 * on. Non-blocking (a mutex-protected copy): fine on the UI thread.
 *
 * True when associated (wpa_state=COMPLETED); *out_signal_level then gets
 * the 0 (weakest) - 3 (strongest) bucket of the last RSSI reading. False
 * (leaving *out_signal_level untouched) when not connected or Wi-Fi is off
 * (always on the host simulator without a supplicant). */
bool wifi_get_status(int * out_signal_level);
/* SSID of the current connection; false (out = "") when not connected. */
bool wifi_status_last_ssid(char * out, size_t size);
/* Bumped whenever the cached state changes (status bar refresh). */
unsigned wifi_status_generation(void);

/* Events since the last call, for the Wi-Fi screens (UI thread, one consumer). */
enum {
    WIFI_STATUS_EVT_ENABLED = 1u << 0,      /* on/off changed */
    WIFI_STATUS_EVT_CONNECTED = 1u << 1,
    WIFI_STATUS_EVT_DISCONNECTED = 1u << 2,
    WIFI_STATUS_EVT_SCAN_DONE = 1u << 3,
    WIFI_STATUS_EVT_WRONG_KEY = 1u << 4,    /* the access point rejected the saved key */
};
unsigned wifi_status_take_events(void);

#endif /* WIFI_STATUS_H */
