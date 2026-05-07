#ifndef VOIDWATCH_AUDIO_H
#define VOIDWATCH_AUDIO_H

#include <stdio.h>

typedef enum {
    BAND_SUB = 0,    /*    20 -    60 Hz */
    BAND_BASS,       /*    60 -   250 Hz */
    BAND_LOW_MID,    /*   250 -   500 Hz */
    BAND_MID,        /*   500 -  2000 Hz */
    BAND_HIGH_MID,   /*  2000 -  6000 Hz */
    BAND_TREBLE,     /*  6000 - 20000 Hz */
    BAND_COUNT
} AudioBand;

typedef struct {
    float bands[BAND_COUNT]; /* AGC-normalised + envelope-smoothed, 0..1 */
    float transient;          /* fast-decay onset detector, 0..1          */
    int   active;             /* 1 once the capture thread has produced data */
} AudioSnapshot;

/*
 * Open ALSA capture, build the FFTW plan, spawn the capture thread.
 *   device == NULL → checks $VOIDWATCH_AUDIO_DEVICE, then "default".
 * Returns 0 on success. Failure is non-fatal — call audio_snapshot() to
 * read zeroed bands, all modulation factors collapse to identity.
 */
int  audio_init(const char *device);
void audio_shutdown(void);

/* Thread-safe value-copy of the latest band snapshot. */
AudioSnapshot audio_snapshot(void);

/*
 * Print every ALSA PCM capture source to `out`, one per line, with a
 * "(monitor)" tag on names that look like a sink monitor. Independent
 * of audio_init() — safe to call without raw-mode set up.
 */
void audio_list_devices(FILE *out);

#endif
