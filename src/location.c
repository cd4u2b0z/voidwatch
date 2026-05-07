#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "location.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD (M_PI / 180.0)

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int parse_double(const char *s, double *out) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || end == s) return -1;
    *out = v;
    return 0;
}

static int try_file(double *lat, double *lon) {
    char path[512];
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(path, sizeof path, "%s/.config/voidwatch/location.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int got_lat = 0, got_lon = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p);
        char *val = trim(eq + 1);
        double v;
        if (parse_double(val, &v) != 0) continue;
        if      (!strcmp(key, "lat")) { *lat = v; got_lat = 1; }
        else if (!strcmp(key, "lon")) { *lon = v; got_lon = 1; }
    }
    fclose(f);
    return (got_lat && got_lon) ? 0 : -1;
}

static int try_env(double *lat, double *lon) {
    const char *e_lat = getenv("VOIDWATCH_LAT");
    const char *e_lon = getenv("VOIDWATCH_LON");
    if (!e_lat || !e_lon) return -1;
    if (parse_double(e_lat, lat) != 0) return -1;
    if (parse_double(e_lon, lon) != 0) return -1;
    return 0;
}

int location_resolve(double cli_lat, double cli_lon,
                     Observer *out, int *fallback) {
    if (fallback) *fallback = 0;
    double lat = NAN, lon = NAN;

    if (!isnan(cli_lat) && !isnan(cli_lon)) {
        lat = cli_lat;
        lon = cli_lon;
    } else if (try_file(&lat, &lon) != 0
            && try_env (&lat, &lon) != 0) {
        lat = 0.0;
        lon = 0.0;
        if (fallback) *fallback = 1;
    }

    if (lat < -90.0  || lat > 90.0)  return -1;
    if (lon < -180.0 || lon > 180.0) return -1;

    out->lat_rad = lat * DEG2RAD;
    out->lon_rad = lon * DEG2RAD;
    return 0;
}
