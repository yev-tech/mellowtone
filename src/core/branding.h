#ifndef BRANDING_H
#define BRANDING_H

/* Product identity. Every name this application presents to the user or to
 * other software (HTTP User-Agent, Subsonic client id, MPRIS identity,
 * mDNS service, remote-control PIN header, Bluetooth service name) comes
 * from here.
 *
 * On-device paths (storage_paths.h, board_config.h) and installed file
 * names are not built from it. */

#define MT_PRODUCT_NAME "Mellowtone"

/* Contact appended to the User-Agent as "Name/version ( contact )" (the
 * MusicBrainz convention). Empty: no contact is sent. */
#define MT_CONTACT ""

/* Subsonic API `c=` client name. */
#define MT_SUBSONIC_CLIENT MT_PRODUCT_NAME
/* MPRIS MediaPlayer2 Identity (Bluetooth media player). */
#define MT_MPRIS_IDENTITY MT_PRODUCT_NAME
/* DNS-SD service type and instance of the web remote control. */
#define MT_MDNS_SERVICE "_mellowtone._tcp"
#define MT_MDNS_INSTANCE MT_PRODUCT_NAME " Remote"
/* HTTP header that carries the web remote-control PIN. */
#define MT_REMOTE_PIN_HEADER "X-Mellowtone-PIN"
/* SDP service name of the Bluetooth remote control. */
#define MT_BT_REMOTE_NAME MT_PRODUCT_NAME " Remote"

/* "Mellowtone/<version>" plus " ( MT_CONTACT )" when MT_CONTACT is set.
 * The version is app_version_label() reduced to HTTP token characters
 * (spaces and other separators become '-'). Built once, thread-safe; the
 * returned string is static. */
const char * mt_user_agent(void);

#endif
