#ifndef VOIDWATCH_EPHEM_H
#define VOIDWATCH_EPHEM_H

#include <time.h>

/*
 * Low-precision ephemeris (Meeus, "Astronomical Algorithms", 2e).
 * Accuracy: solar ~0.01°, lunar ~0.1°, planets ~few arcminutes.
 * That's well under one terminal cell at any sane viewport, so we trade
 * VSOP87 series for the pocket-sized formulas. No external data files,
 * no allocations, no thread-local state — pure.
 */

typedef enum {
    EPHEM_SUN = 0,
    EPHEM_MOON,
    EPHEM_MERCURY,
    EPHEM_VENUS,
    EPHEM_MARS,
    EPHEM_JUPITER,
    EPHEM_SATURN,
    EPHEM_URANUS,
    EPHEM_NEPTUNE,
    EPHEM_COUNT
} EphemBody;

typedef struct {
    /* Geocentric apparent equatorial coordinates of date. */
    double ra_rad;        /* right ascension, [0, 2π)        */
    double dec_rad;       /* declination, [-π/2, π/2]         */
    double distance_au;   /* Earth-body distance, AU (Moon: km) */
    double magnitude;     /* apparent visual magnitude         */
    /* Topocentric horizontal coordinates (filled by ephem_to_topocentric). */
    double alt_rad;       /* altitude above horizon            */
    double az_rad;        /* azimuth, North=0 increasing East  */
} EphemPosition;

typedef struct {
    double lat_rad;
    double lon_rad;       /* east-positive, west-negative     */
} Observer;

/* Unix epoch → Julian Day (UT). */
double ephem_julian_day_from_unix(time_t unix_time);

/* Geocentric apparent (RA, Dec) for `body` at Julian Day `jd`. */
void ephem_compute(EphemBody body, double jd, EphemPosition *pos);

/* Convert (ra, dec) in `pos` to topocentric (alt, az) — fills pos->alt_rad
 * and pos->az_rad. Doesn't apply atmospheric refraction (negligible for
 * the cell-scale visual). */
void ephem_to_topocentric(EphemPosition *pos, const Observer *obs, double jd);

/* Local Apparent Sidereal Time, hours [0, 24). */
double ephem_local_sidereal_hours(double jd, double lon_east_rad);

/* Earth's heliocentric ecliptic position (J2000-of-date) in AU. Comets
 * and similar Keplerian propagators subtract this to get the geocentric
 * vector. */
void ephem_earth_helio_xyz(double jd, double *x, double *y, double *z);

/* Heliocentric ecliptic position of any planet (Mercury through Neptune).
 * Sun returns (0,0,0); Moon is rejected (its motion is geocentric — use
 * ephem_compute and add Earth's helio if you need helio Moon). */
void ephem_helio_xyz_for(EphemBody body, double jd,
                         double *x, double *y, double *z);

/* Mean obliquity of the ecliptic at jd, radians. Exposed for converting
 * heliocentric ecliptic vectors (comets, asteroids) to equatorial. */
double ephem_obliquity_rad(double jd);

/* Display strings. `name` is full ("Mercury"); `short_name` is 3-char
 * codes for compact HUD use ("MER"). */
const char *ephem_name(EphemBody body);
const char *ephem_short(EphemBody body);

#endif
