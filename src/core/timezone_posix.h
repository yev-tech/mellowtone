#ifndef TIMEZONE_POSIX_H
#define TIMEZONE_POSIX_H

/* POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") for an IANA id from
 * TIMEZONE_TABLE, or NULL. For boards without a zoneinfo tree (R3 Pro). */
const char * timezone_posix_rule(const char * iana_id);

#endif
