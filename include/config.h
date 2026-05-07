#ifndef VOIDWATCH_CONFIG_H
#define VOIDWATCH_CONFIG_H

typedef struct { float r, g, b; } Color;

#define TARGET_FPS 60
#define FRAME_NS   (1000000000L / TARGET_FPS)

/* Background void / spectral / nebula colours all live in `Palette` now
 * — see palette.h. config.h holds only non-themable scalars. */

/* Luminance threshold for marking a sub-pixel "lit" in the Braille pass. */
#define LUM_THRESHOLD 0.04f

/* Phosphorescent decay per render tick (Phase 2 trails). */
#define FB_DECAY 0.92f

/* Camera drift in sub-pixels per second — ~1-2 cells/min per spec. */
#define CAMERA_DRIFT_X 0.05f
#define CAMERA_DRIFT_Y 0.018f

/* Stars per terminal cell (density-scaled to viewport). */
#define STAR_DENSITY 0.08f
#define STAR_COUNT_MAX 4096
#define STAR_LAYERS 3

/* Orbital physics (sub-pixel world units, seconds). */
#define GRAVITY_G          0.25f
#define GRAVITY_SOFTEN_SQ  1.0f

/* Initial system. */
#define CENTRAL_MASS       100.0f
#define CENTRAL_RADIUS     3.0f
#define CENTRAL_INTENSITY  2.2f

#define PLANET_COUNT       5
/* Orbit radii as fractions of min(sub_w, sub_h) so the system fits any term. */
static const float PLANET_ORBIT_FRAC[PLANET_COUNT] = {
    0.05f, 0.10f, 0.16f, 0.23f, 0.32f
};

#define NEUTRON_ORBIT_FRAC 0.42f
#define NEUTRON_ECCENTRIC  0.65f  /* v factor; 1.0 = circular, <1 = elliptic */

/* Optional companions. Both kept low-mass so the n-body solution stays
 * tame (unbalanced systems go chaotic — see CLAUDE.md "Don't"). */
#define BLACKHOLE_ORBIT_FRAC 0.50f
#define BLACKHOLE_MASS       1.5f
#define NEBULA_ORBIT_FRAC    0.65f
#define NEBULA_MASS          0.1f

/* Body draw — Gaussian falloff sigma is RADIUS * GLOW_SIGMA_K. */
#define GLOW_SIGMA_K       0.65f
#define GLOW_BBOX_K        3.5f   /* clip beyond this many sigma */

/* Spectral classes live in g_palette.star_m / star_g / star_b. */

/* ---- Phase 3 -------------------------------------------------------- */

/* Nebula (Perlin wash, sampled once per cell). */
#define NEBULA_SCALE_BIG    0.045f  /* low-freq world->noise */
#define NEBULA_SCALE_SML    0.130f  /* high-freq detail */
/* Time drift is *deliberately* near-static. Deep space doesn't evolve on
 * human timescales; values >0.01 start to read as flicker / vibration. */
#define NEBULA_DRIFT_BIG    0.004f
#define NEBULA_DRIFT_SML    0.006f
#define NEBULA_PARALLAX     0.25f   /* between bg-stars (0.1) and mid (0.4) */
#define NEBULA_THRESHOLD    0.42f   /* high → most of frame is empty void */
#define NEBULA_SHAPE        2.8f    /* sharper edges, sparser wisps */
#define NEBULA_INTENSITY    0.18f   /* peak fb_max value */

/* Nebula tones live in g_palette.nebula_violet / nebula_crimson. */

/* Particle system. */
#define PARTICLE_CAP            2048
#define PARTICLE_DUST_TARGET    80      /* sparse twinkle, not a sparkle-grid */

/* Emission rates (events / second / emitter). Photon flux deliberately
 * sparse — omnidirectional bursts from a point source read as fireworks
 * if cranked up. Reserve big bursts for Phase 4 audio transients. */
#define EMIT_PHOTON_PER_STAR     3.0f
#define EMIT_WIND_PER_STAR       4.0f
#define EMIT_DEBRIS_PER_PLANET   0.3f

/* Initial speeds (sub-pixels / second). */
#define SPD_PHOTON              22.0f
#define SPD_WIND                 8.0f
#define SPD_DEBRIS               3.0f
#define SPD_DUST                 0.8f

/* Lifetimes (seconds). */
#define LIFE_PHOTON              6.0f
#define LIFE_WIND               18.0f
#define LIFE_DEBRIS             25.0f
#define LIFE_DUST               40.0f

/* Per-particle peak intensity (additive). */
#define I_PHOTON                 0.45f
#define I_WIND                   0.30f
#define I_DEBRIS                 0.40f
#define I_DUST                   0.30f   /* lower since stamp is single sub-pixel now */

/* ---- Phase 4 — audio reactivity ------------------------------------ */
#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_CHUNK         2048
#define AUDIO_DEVICE_DEFAULT "default"

/* Per-band envelope follower: lerp factor per FFT frame. */
#define AUDIO_ATTACK         0.45f
#define AUDIO_RELEASE        0.06f

/* Auto-gain — running peak with slow decay; floor prevents divide-by-zero
 * and stops idle-room hiss from being amplified to full scale. */
#define AUDIO_AGC_DECAY      0.9995f
#define AUDIO_AGC_FLOOR      0.05f

/* Transient detector decay (per FFT frame). */
#define AUDIO_TRANSIENT_DECAY 0.85f

/* How hard each band pushes its modulation target.
 * Final factor is (1 + band[i] * MOD_*) — 0 audio → 1.0 → identity. */
#define MOD_BASS_BODY        2.00f   /* star/neutron brightness  — was 0.85 */
#define MOD_BASS_NEBULA      1.20f   /* nebula intensity         — was 0.55 */
#define MOD_LOWMID_EMIT      3.50f   /* particle emission rate   — was 1.40 */
#define MOD_TREBLE_TWINKLE   5.00f   /* starfield shimmer depth  — was 2.50 */
#define MOD_SUBBASS_SHAKE    4.00f   /* sub-pixel camera jitter  — was 1.80 */

/* ---- Supernova (audio-triggered photon shell) ---------------------- */
#define SUPERNOVA_THRESHOLD   0.55f   /* snap.transient must clear this */
#define SUPERNOVA_COOLDOWN    8.0     /* seconds; rate-limit the event  */
#define SUPERNOVA_PHOTONS     96      /* points around the shell        */
#define SUPERNOVA_SPEED_MULT  2.5f    /* relative to SPD_PHOTON         */
#define SUPERNOVA_LIFE_MULT   2.0f    /* relative to LIFE_PHOTON        */
#define SUPERNOVA_INTENSITY   0.85f   /* additive peak per particle     */

/* No-audio fallback: with audio off, the transient path never fires.
 * Schedule a periodic supernova so the visual stays alive. */
#define SUPERNOVA_FALLBACK_PERIOD 90.0  /* seconds; 60-120 per spec */

#endif
