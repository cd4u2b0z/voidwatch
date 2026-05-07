#ifndef VOIDWATCH_LOCATION_H
#define VOIDWATCH_LOCATION_H

#include "ephem.h"

/*
 * Resolve observer location in this precedence order:
 *   1. CLI: cli_lat / cli_lon (NaN = unset)
 *   2. ~/.config/voidwatch/location.conf  (lat = ..., lon = ...)
 *   3. $VOIDWATCH_LAT / $VOIDWATCH_LON env vars
 *   4. fallback (0, 0)  — `*resolved_via_fallback` set to 1 so the caller
 *      can warn the user.
 *
 * Latitude  positive = north, negative = south.
 * Longitude positive = east,  negative = west.
 *
 * Returns 0 on success, -1 if values were invalid (out of range).
 */
int location_resolve(double cli_lat, double cli_lon,
                     Observer *out, int *resolved_via_fallback);

#endif
