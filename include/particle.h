#ifndef VOIDWATCH_PARTICLE_H
#define VOIDWATCH_PARTICLE_H

#include "audio.h"
#include "body.h"
#include "config.h"
#include "framebuffer.h"

typedef enum {
    PK_DEAD = 0,
    PK_STAR_DUST,
    PK_GAS,
    PK_DEBRIS,
    PK_PHOTON,
    PK_SOLAR_WIND,
} ParticleKind;

typedef struct {
    float wx, wy;
    float vx, vy;
    float life, max_life;
    float intensity;
    Color color;
    unsigned char kind;
    unsigned char gravity; /* 1 = body gravity applies */
} Particle;

/*
 * Fixed-cap arena. Spawning prefers PK_DEAD slots, falling back to
 * round-robin overwrite at `cursor` when the arena is saturated. No
 * malloc in the hot path.
 */
typedef struct {
    Particle *arr;
    int       cap;
    int       cursor;
} ParticleArena;

int  particle_init(ParticleArena *pa);
void particle_free(ParticleArena *pa);

/*
 * One step of physics + emission. Bodies emit photons / wind / debris at
 * their configured rates; ambient star-dust population is topped up to
 * PARTICLE_DUST_TARGET inside the current viewport.
 */
void particle_update(ParticleArena *pa,
                     const BodySystem *bs,
                     int sub_w, int sub_h,
                     float cam_x, float cam_y,
                     float dt,
                     const AudioSnapshot *snap);

void particle_draw(const ParticleArena *pa, Framebuffer *fb,
                   float cam_x, float cam_y);

/*
 * One-shot photon shell radiating from `origin`. Caller is responsible for
 * rate-limiting; this just stamps SUPERNOVA_PHOTONS particles into the
 * arena unconditionally. Inherits the body's tint at full white-shifted
 * intensity so the flash reads regardless of the host's colour.
 */
void particle_spawn_supernova(ParticleArena *pa, const Body *origin);

#endif
