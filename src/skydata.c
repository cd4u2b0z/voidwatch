#include "skydata.h"

/* J2000 positions for ~75 of the brightest stars. Indices are stable —
 * `sky_lines[]` below references them. Values rounded to roughly ~1
 * arcminute; well below a Braille cell. */
const SkyStar sky_stars[] = {
    /* 0  */ { "Sirius",      6.752,  -16.72, -1.46f, 'A' },
    /* 1  */ { "Canopus",     6.399,  -52.69, -0.74f, 'A' },
    /* 2  */ { "Arcturus",   14.261,   19.18, -0.05f, 'K' },
    /* 3  */ { "Vega",       18.616,   38.78,  0.03f, 'A' },
    /* 4  */ { "Capella",     5.278,   45.998, 0.08f, 'G' },
    /* 5  */ { "Rigel",       5.242,   -8.20,  0.13f, 'B' },
    /* 6  */ { "Procyon",     7.655,    5.225, 0.34f, 'F' },
    /* 7  */ { "Achernar",    1.629,  -57.24,  0.46f, 'B' },
    /* 8  */ { "Betelgeuse",  5.919,    7.41,  0.50f, 'M' },
    /* 9  */ { "Hadar",      14.064,  -60.37,  0.61f, 'B' },
    /* 10 */ { "Altair",     19.846,    8.87,  0.77f, 'A' },
    /* 11 */ { "Acrux",      12.443,  -63.10,  0.77f, 'B' },
    /* 12 */ { "Aldebaran",   4.599,   16.51,  0.85f, 'K' },
    /* 13 */ { "Antares",    16.490,  -26.43,  1.06f, 'M' },
    /* 14 */ { "Spica",      13.420,  -11.16,  1.04f, 'B' },
    /* 15 */ { "Pollux",      7.755,   28.03,  1.14f, 'K' },
    /* 16 */ { "Fomalhaut",  22.961,  -29.62,  1.16f, 'A' },
    /* 17 */ { "Mimosa",     12.795,  -59.69,  1.25f, 'B' },
    /* 18 */ { "Deneb",      20.690,   45.28,  1.25f, 'A' },
    /* 19 */ { "Regulus",    10.140,   11.97,  1.36f, 'B' },
    /* 20 */ { "Adhara",      6.977,  -28.97,  1.50f, 'B' },
    /* 21 */ { "Castor",      7.577,   31.89,  1.58f, 'A' },
    /* 22 */ { "Gacrux",     12.519,  -57.11,  1.63f, 'M' },
    /* 23 */ { "Shaula",     17.560,  -37.10,  1.62f, 'B' },
    /* 24 */ { "Bellatrix",   5.418,    6.35,  1.64f, 'B' },
    /* 25 */ { "Elnath",      5.438,   28.61,  1.65f, 'B' },
    /* 26 */ { "Miaplacidus", 9.220,  -69.72,  1.68f, 'A' },
    /* 27 */ { "Alnilam",     5.604,   -1.20,  1.69f, 'B' },
    /* 28 */ { "Alnitak",     5.679,   -1.94,  1.74f, 'O' },
    /* 29 */ { "Alioth",     12.900,   55.96,  1.76f, 'A' },
    /* 30 */ { "Mirfak",      3.405,   49.86,  1.79f, 'F' },
    /* 31 */ { "Dubhe",      11.062,   61.75,  1.79f, 'K' },
    /* 32 */ { "Wezen",       7.140,  -26.39,  1.83f, 'F' },
    /* 33 */ { "Kaus Aus.", 18.403,  -34.38,  1.85f, 'B' },
    /* 34 */ { "Avior",       8.375,  -59.51,  1.86f, 'K' },
    /* 35 */ { "Alkaid",     13.793,   49.31,  1.85f, 'B' },
    /* 36 */ { "Atria",      16.811,  -69.03,  1.91f, 'K' },
    /* 37 */ { "Alhena",      6.628,   16.40,  1.93f, 'A' },
    /* 38 */ { "Peacock",    20.428,  -56.74,  1.94f, 'B' },
    /* 39 */ { "Polaris",     2.530,   89.26,  1.97f, 'F' },
    /* 40 */ { "Mirzam",      6.378,  -17.96,  1.98f, 'B' },
    /* 41 */ { "Alphard",     9.460,   -8.66,  1.99f, 'K' },
    /* 42 */ { "Algieba",    10.333,   19.84,  2.01f, 'K' },
    /* 43 */ { "Hamal",       2.119,   23.46,  2.00f, 'K' },
    /* 44 */ { "Diphda",      0.726,  -17.99,  2.04f, 'K' },
    /* 45 */ { "Nunki",      18.921,  -26.29,  2.05f, 'B' },
    /* 46 */ { "Mizar",      13.398,   54.93,  2.04f, 'A' },
    /* 47 */ { "Saiph",       5.795,   -9.67,  2.06f, 'B' },
    /* 48 */ { "Kochab",     14.846,   74.16,  2.08f, 'K' },
    /* 49 */ { "Alnair",     22.137,  -46.96,  1.74f, 'B' },
    /* 50 */ { "Mintaka",     5.533,   -0.30,  2.23f, 'O' },
    /* 51 */ { "Caph",        0.153,   59.15,  2.27f, 'F' },
    /* 52 */ { "Schedar",     0.675,   56.54,  2.24f, 'K' },
    /* 53 */ { "Almach",      2.065,   42.33,  2.10f, 'K' },
    /* 54 */ { "Algol",       3.136,   40.96,  2.12f, 'B' },
    /* 55 */ { "Sadr",       20.371,   40.26,  2.23f, 'F' },
    /* 56 */ { "Gienah Cyg", 20.770,   33.97,  2.48f, 'K' },
    /* 57 */ { "Aljanah",    19.749,   45.13,  2.87f, 'K' },
    /* 58 */ { "Albireo",    19.512,   27.96,  3.18f, 'K' },
    /* 59 */ { "Markab",     23.079,   15.21,  2.49f, 'A' },
    /* 60 */ { "Scheat",     23.063,   28.08,  2.42f, 'M' },
    /* 61 */ { "Algenib",     0.220,   15.18,  2.83f, 'B' },
    /* 62 */ { "Alpheratz",   0.140,   29.09,  2.06f, 'B' },
    /* 63 */ { "Mirach",      1.162,   35.62,  2.05f, 'M' },
    /* 64 */ { "Merak",      11.030,   56.38,  2.37f, 'A' },
    /* 65 */ { "Phecda",     11.897,   53.69,  2.41f, 'A' },
    /* 66 */ { "Megrez",     12.257,   57.03,  3.31f, 'A' },
    /* 67 */ { "Denebola",   11.818,   14.57,  2.13f, 'A' },
    /* 68 */ { "Izar",       14.749,   27.07,  2.37f, 'K' },
    /* 69 */ { "Sheliak",    18.835,   33.36,  3.45f, 'A' },
    /* 70 */ { "Sulafat",    18.983,   32.69,  3.24f, 'B' },
    /* 71 */ { "Ruchbah",     1.430,   60.235, 2.66f, 'A' },
    /* 72 */ { "Segin",       1.907,   63.67,  3.34f, 'B' },
    /* 73 */ { "Gamma Cas",   0.945,   60.717, 2.20f, 'B' },
    /* 74 */ { "Tarazed",    19.771,   10.61,  2.72f, 'K' },
};
const int sky_stars_count =
    (int)(sizeof sky_stars / sizeof sky_stars[0]);

/* Constellation stick figures — minimal patterns that read as the named
 * shape. Indices reference sky_stars[] above. */
const SkyLine sky_lines[] = {
    /* Orion's belt */
    { 50, 27 }, { 27, 28 },
    /* Orion outline */
    { 24,  8 },         /* shoulders                */
    { 24, 50 },         /* left shoulder → west belt */
    {  8, 28 },         /* right shoulder → east belt */
    { 50,  5 },         /* west belt → Rigel        */
    { 28, 47 },         /* east belt → Saiph        */
    {  5, 47 },         /* Rigel → Saiph (skirt)    */
    /* Big Dipper (UMa) */
    { 31, 64 }, { 64, 65 }, { 65, 66 },
    { 66, 29 }, { 29, 46 }, { 46, 35 },
    /* Cassiopeia W */
    { 51, 52 }, { 52, 73 }, { 73, 71 }, { 71, 72 },
    /* Northern Cross (Cygnus) */
    { 18, 55 }, { 55, 58 },
    { 56, 55 }, { 55, 57 },
    /* Lyra parallelogram */
    {  3, 70 }, { 70, 69 }, { 69,  3 },
    /* Southern Cross (Crux) */
    { 11, 22 }, { 17, 11 }, { 17, 22 },
    /* Leo sickle */
    { 19, 42 }, { 42, 67 },
    /* Scorpius — minimal head/tail */
    { 13, 23 },
    /* Great Square of Pegasus */
    { 59, 60 }, { 60, 62 }, { 62, 61 }, { 61, 59 },
    /* Andromeda chain off Pegasus */
    { 62, 63 }, { 63, 53 },
    /* Perseus (Mirfak → Algol) */
    { 30, 54 },
    /* Gemini twins */
    { 21, 15 },
    /* Auriga to Taurus tip */
    {  4, 25 },
    /* Pointer line: Polaris from Big Dipper bowl edge */
    { 31, 39 },
};
const int sky_lines_count =
    (int)(sizeof sky_lines / sizeof sky_lines[0]);
