#ifndef VOIDWATCH_DSO_H
#define VOIDWATCH_DSO_H

/*
 * Named deep-sky objects — Messier + a handful of bright NGC entries.
 * Hand-curated catalogue, baked into the binary at compile time. Same
 * data posture as skydata.c.
 *
 * Position: J2000 RA/Dec, no precession applied (drift is sub-arcminute
 * over a few decades — under one terminal cell).
 *
 * Apparent size + magnitude are integrated values; we render as a soft
 * Gaussian patch sized by the apparent angular extent and tinted by
 * the object kind. No detail beyond a fuzzy spot at terminal scale.
 */

typedef enum {
    DSO_GALAXY = 0,
    DSO_NEBULA_BRIGHT,
    DSO_NEBULA_PLANETARY,
    DSO_CLUSTER_OPEN,
    DSO_CLUSTER_GLOBULAR,
} DSOKind;

typedef struct {
    const char *name;        /* M31, M42, NGC 869, … */
    double      ra_h;        /* J2000 right ascension, hours */
    double      dec_deg;     /* J2000 declination, degrees   */
    float       size_arcmin; /* major axis of apparent extent */
    float       mag;         /* integrated visual magnitude   */
    DSOKind     kind;
} DSObject;

extern const DSObject dso_catalog[];
extern const int      dso_count;

#endif
