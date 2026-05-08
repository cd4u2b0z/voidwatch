#include "dso.h"

/* 30 famous deep-sky objects spanning galaxies, bright nebulae,
 * planetary nebulae, open clusters, and globulars. Positions and sizes
 * sourced from the Messier catalogue and SEDS NGC; values rounded to
 * ~arcminute precision (well below cell scale). */
const DSObject dso_catalog[] = {
    /* Galaxies */
    { "M31 Andromeda",   0.7117,  41.269,  178.0f, 3.4f, DSO_GALAXY },
    { "M33 Triangulum",  1.5644,  30.660,   70.8f, 5.7f, DSO_GALAXY },
    { "M51 Whirlpool",  13.4979,  47.195,   11.2f, 8.4f, DSO_GALAXY },
    { "M81 Bode's",      9.9258,  69.067,   25.7f, 6.9f, DSO_GALAXY },
    { "M101 Pinwheel",  14.0533,  54.349,   28.8f, 7.9f, DSO_GALAXY },
    { "M104 Sombrero",  12.6663, -11.623,    8.7f, 8.0f, DSO_GALAXY },

    /* Bright (emission/reflection) nebulae */
    { "M42 Orion Nebula", 5.5900, -5.4500,  65.0f, 4.0f, DSO_NEBULA_BRIGHT },
    { "M8 Lagoon",       18.0633, -24.383,  90.0f, 6.0f, DSO_NEBULA_BRIGHT },
    { "M16 Eagle",       18.3133, -13.783,  35.0f, 6.0f, DSO_NEBULA_BRIGHT },
    { "M17 Omega",       18.3467, -16.183,  11.0f, 6.0f, DSO_NEBULA_BRIGHT },
    { "M20 Trifid",      18.0433, -23.033,  28.0f, 6.3f, DSO_NEBULA_BRIGHT },
    { "NGC 7000 N. America", 20.9833, 44.333, 120.0f, 4.0f, DSO_NEBULA_BRIGHT },
    { "NGC 6960 Veil",   20.7500,  30.717,  60.0f, 7.0f, DSO_NEBULA_BRIGHT },

    /* Planetary nebulae */
    { "M57 Ring",        18.8933,  33.033,   1.4f, 8.8f, DSO_NEBULA_PLANETARY },
    { "M27 Dumbbell",    19.9933,  22.717,   8.0f, 7.4f, DSO_NEBULA_PLANETARY },
    { "M1 Crab",          5.5750,  22.017,   6.0f, 8.4f, DSO_NEBULA_PLANETARY },

    /* Open clusters */
    { "M45 Pleiades",     3.7833,  24.117, 110.0f, 1.6f, DSO_CLUSTER_OPEN },
    { "M44 Beehive",      8.6733,  19.983,  95.0f, 3.7f, DSO_CLUSTER_OPEN },
    { "M67",              8.8400,  11.817,  30.0f, 6.1f, DSO_CLUSTER_OPEN },
    { "M11 Wild Duck",   18.8517,  -6.267,  14.0f, 5.8f, DSO_CLUSTER_OPEN },
    { "M6 Butterfly",    17.6683, -32.217,  25.0f, 4.2f, DSO_CLUSTER_OPEN },
    { "M7 Ptolemy",      17.8983, -34.783,  80.0f, 3.3f, DSO_CLUSTER_OPEN },
    { "NGC 869 Double",   2.3300,  57.133,  60.0f, 4.0f, DSO_CLUSTER_OPEN },

    /* Globular clusters */
    { "M13 Hercules",    16.6950,  36.467,  16.6f, 5.8f, DSO_CLUSTER_GLOBULAR },
    { "M22",             18.6067, -23.900,  24.0f, 5.1f, DSO_CLUSTER_GLOBULAR },
    { "M3",              13.7033,  28.383,  18.0f, 6.2f, DSO_CLUSTER_GLOBULAR },
    { "M5",              15.3100,   2.083,  23.0f, 5.6f, DSO_CLUSTER_GLOBULAR },
    { "M15",             21.5000,  12.167,  12.0f, 6.2f, DSO_CLUSTER_GLOBULAR },
    { "M55",             19.6667, -30.967,  19.0f, 7.4f, DSO_CLUSTER_GLOBULAR },
    { "Omega Centauri",  13.4467, -47.483,  36.3f, 3.7f, DSO_CLUSTER_GLOBULAR },
};
const int dso_count = (int)(sizeof dso_catalog / sizeof dso_catalog[0]);
