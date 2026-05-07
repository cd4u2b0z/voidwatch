#ifndef VOIDWATCH_SKYDATA_H
#define VOIDWATCH_SKYDATA_H

/*
 * Hand-bundled bright-star catalogue + constellation stick figures.
 *
 * Data source: J2000 mean positions + apparent visual magnitude + first
 * spectral letter. Values are sourced from common references (SIMBAD,
 * Hipparcos / BSC) and rounded to ~0.001h / ~0.01° / 0.01 mag — well
 * within the cell-resolution accuracy of the renderer.
 *
 * No external data files per project policy. The arrays below are linked
 * into the binary directly. Roughly ~60 stars + ~40 line segments — the
 * minimum that conveys "real sky" without committing to a 9000-row BSC
 * parser.
 */

typedef struct {
    const char *name;     /* short common name; NULL = no label    */
    double      ra_h;     /* right ascension, hours [0, 24)        */
    double      dec_deg;  /* declination, degrees [-90, 90]        */
    float       mag;      /* apparent visual magnitude             */
    char        spectral; /* O/B/A/F/G/K/M; '?' if unknown         */
} SkyStar;

typedef struct {
    short a, b;           /* indices into sky_stars[]              */
} SkyLine;

extern const SkyStar  sky_stars[];
extern const int      sky_stars_count;
extern const SkyLine  sky_lines[];
extern const int      sky_lines_count;

#endif
