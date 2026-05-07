#include <math.h>
#include <stdlib.h>

#include "config.h"
#include "particle.h"

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float frand_signed(void) { return frand() * 2.0f - 1.0f; }

int particle_init(ParticleArena *pa) {
    pa->arr = calloc((size_t)PARTICLE_CAP, sizeof(Particle));
    if (!pa->arr) return -1;
    pa->cap    = PARTICLE_CAP;
    pa->cursor = 0;
    return 0;
}

void particle_free(ParticleArena *pa) {
    free(pa->arr);
    pa->arr = NULL;
    pa->cap = 0;
}

/* Pick a slot: prefer dead, else round-robin overwrite oldest at cursor. */
static Particle *acquire_slot(ParticleArena *pa) {
    for (int probe = 0; probe < pa->cap; probe++) {
        int i = (pa->cursor + probe) % pa->cap;
        if (pa->arr[i].kind == PK_DEAD) {
            pa->cursor = (i + 1) % pa->cap;
            return &pa->arr[i];
        }
    }
    Particle *p = &pa->arr[pa->cursor];
    pa->cursor = (pa->cursor + 1) % pa->cap;
    return p;
}

static void spawn(ParticleArena *pa, ParticleKind kind,
                  float wx, float wy, float vx, float vy,
                  float life, float intensity, Color color, int gravity) {
    Particle *p   = acquire_slot(pa);
    p->wx         = wx;
    p->wy         = wy;
    p->vx         = vx;
    p->vy         = vy;
    p->max_life   = life;
    p->life       = life;
    p->intensity  = intensity;
    p->color      = color;
    p->kind       = (unsigned char)kind;
    p->gravity    = (unsigned char)(gravity ? 1 : 0);
}

/* Probabilistic spawn that handles rates >= 1/frame. */
static void emit_at_rate(ParticleArena *pa, float rate, float dt,
                         void (*spawn_fn)(ParticleArena *, void *), void *ctx) {
    float expected = rate * dt;
    while (expected > 0.0f) {
        if (frand() < expected) spawn_fn(pa, ctx);
        expected -= 1.0f;
    }
}

/* ---- emission helpers ----------------------------------------------- */

typedef struct { const Body *b; } BodyCtx;

static void emit_photon(ParticleArena *pa, void *ctx) {
    const Body *b = ((BodyCtx *)ctx)->b;
    float theta = frand() * 6.2831853f;
    float c = cosf(theta), s = sinf(theta);
    /* Stronger photon flux from the neutron star, bluer. */
    float speed = (b->kind == BODY_NEUTRON_STAR) ? SPD_PHOTON * 1.4f : SPD_PHOTON;
    spawn(pa, PK_PHOTON,
          b->wx + b->radius * c, b->wy + b->radius * s,
          speed * c,             speed * s,
          LIFE_PHOTON, I_PHOTON, b->color, 0);
}

static void emit_wind(ParticleArena *pa, void *ctx) {
    const Body *b = ((BodyCtx *)ctx)->b;
    float theta = frand() * 6.2831853f;
    float c = cosf(theta), s = sinf(theta);
    float speed = SPD_WIND * (0.7f + 0.6f * frand());
    Color dim   = { b->color.r * 0.7f, b->color.g * 0.7f, b->color.b * 0.85f };
    spawn(pa, PK_SOLAR_WIND,
          b->wx + b->radius * c, b->wy + b->radius * s,
          b->vx + speed * c,     b->vy + speed * s,
          LIFE_WIND, I_WIND, dim, 1);
}

static void emit_debris(ParticleArena *pa, void *ctx) {
    const Body *b = ((BodyCtx *)ctx)->b;
    float theta = frand() * 6.2831853f;
    float c = cosf(theta), s = sinf(theta);
    float speed = SPD_DEBRIS * (0.4f + 1.0f * frand());
    Color dim   = { b->color.r * 0.65f, b->color.g * 0.65f, b->color.b * 0.65f };
    spawn(pa, PK_DEBRIS,
          b->wx + b->radius * c, b->wy + b->radius * s,
          b->vx + speed * c,     b->vy + speed * s,
          LIFE_DEBRIS, I_DEBRIS, dim, 1);
}

static void spawn_dust(ParticleArena *pa, int sub_w, int sub_h,
                       float cam_x, float cam_y, int stagger_life) {
    static const Color DUST_TINT = { 0.78f, 0.84f, 1.00f };
    float wx = cam_x + frand() * (float)sub_w;
    float wy = cam_y + frand() * (float)sub_h;
    float vx = SPD_DUST * frand_signed();
    float vy = SPD_DUST * frand_signed();
    /* Stagger initial life so populations don't synchronously die out. */
    float life = LIFE_DUST * (stagger_life ? frand() : 1.0f);
    float i = I_DUST * (0.6f + 0.4f * frand());
    spawn(pa, PK_STAR_DUST, wx, wy, vx, vy, life, i, DUST_TINT, 0);
}

void particle_spawn_supernova(ParticleArena *pa, const Body *origin) {
    if (!pa || !origin) return;
    /* Bias toward white — a colour-shifted shell still needs to read as
     * a flash regardless of host tint. Mix 60% white into the body color. */
    Color shell = {
        origin->color.r * 0.4f + 0.6f,
        origin->color.g * 0.4f + 0.6f,
        origin->color.b * 0.4f + 0.6f,
    };
    float speed = SPD_PHOTON  * SUPERNOVA_SPEED_MULT;
    float life  = LIFE_PHOTON * SUPERNOVA_LIFE_MULT;

    for (int i = 0; i < SUPERNOVA_PHOTONS; i++) {
        float theta = 6.2831853f * (float)i / (float)SUPERNOVA_PHOTONS;
        /* Tiny per-particle jitter so the shell doesn't read as a perfect
         * polygon when it's young. */
        theta += (frand() - 0.5f) * 0.04f;
        float c = cosf(theta), s = sinf(theta);
        spawn(pa, PK_PHOTON,
              origin->wx + origin->radius * c,
              origin->wy + origin->radius * s,
              speed * c,
              speed * s,
              life, SUPERNOVA_INTENSITY, shell, 0);
    }
}

/* ---- step ----------------------------------------------------------- */

static void apply_gravity(const BodySystem *bs, float px, float py,
                          float *ax, float *ay) {
    float ax_acc = 0.0f, ay_acc = 0.0f;
    for (int i = 0; i < bs->count; i++) {
        const Body *b = &bs->bodies[i];
        float dx = b->wx - px;
        float dy = b->wy - py;
        float r2 = dx * dx + dy * dy + GRAVITY_SOFTEN_SQ;
        float inv_r  = 1.0f / sqrtf(r2);
        float inv_r3 = inv_r * inv_r * inv_r;
        float f = GRAVITY_G * b->mass * inv_r3;
        ax_acc += f * dx;
        ay_acc += f * dy;
    }
    *ax = ax_acc;
    *ay = ay_acc;
}

void particle_update(ParticleArena *pa, const BodySystem *bs,
                     int sub_w, int sub_h,
                     float cam_x, float cam_y, float dt,
                     const AudioSnapshot *snap) {
    float emit_gain = 1.0f + snap->bands[BAND_LOW_MID] * MOD_LOWMID_EMIT;
    /* 1. Integrate + age. */
    int dust_alive = 0;
    for (int i = 0; i < pa->cap; i++) {
        Particle *p = &pa->arr[i];
        if (p->kind == PK_DEAD) continue;

        if (p->gravity) {
            float ax, ay;
            apply_gravity(bs, p->wx, p->wy, &ax, &ay);
            p->vx += ax * dt;
            p->vy += ay * dt;
        }
        p->wx += p->vx * dt;
        p->wy += p->vy * dt;
        p->life -= dt;

        if (p->life <= 0.0f) {
            p->kind = PK_DEAD;
            continue;
        }
        if (p->kind == PK_STAR_DUST) dust_alive++;
    }

    /* 2. Body emissions. */
    for (int i = 0; i < bs->count; i++) {
        const Body *b   = &bs->bodies[i];
        BodyCtx     ctx = { .b = b };
        switch (b->kind) {
        case BODY_STAR:
            emit_at_rate(pa, EMIT_PHOTON_PER_STAR * emit_gain, dt, emit_photon, &ctx);
            emit_at_rate(pa, EMIT_WIND_PER_STAR   * emit_gain, dt, emit_wind,   &ctx);
            break;
        case BODY_NEUTRON_STAR:
            emit_at_rate(pa, EMIT_PHOTON_PER_STAR * 1.5f * emit_gain, dt, emit_photon, &ctx);
            break;
        case BODY_PLANET:
            emit_at_rate(pa, EMIT_DEBRIS_PER_PLANET * emit_gain, dt, emit_debris, &ctx);
            break;
        default:
            break;
        }
    }

    /* 3. Top up ambient dust. First-time fills get staggered lifetimes. */
    int first_fill = (dust_alive == 0);
    while (dust_alive < PARTICLE_DUST_TARGET) {
        spawn_dust(pa, sub_w, sub_h, cam_x, cam_y, first_fill);
        dust_alive++;
    }
}

/* ---- draw ----------------------------------------------------------- */

void particle_draw(const ParticleArena *pa, Framebuffer *fb,
                   float cam_x, float cam_y) {
    for (int i = 0; i < pa->cap; i++) {
        const Particle *p = &pa->arr[i];
        if (p->kind == PK_DEAD) continue;

        float sx = p->wx - cam_x;
        float sy = p->wy - cam_y;
        if (sx < -1 || sy < -1 || sx >= fb->sub_w + 1 || sy >= fb->sub_h + 1)
            continue;

        /* Smooth fade-in / fade-out at the edges of life. */
        float life_frac = p->life / p->max_life;
        float env       = life_frac;
        if (life_frac > 0.85f) env = (1.0f - life_frac) / 0.15f;
        if (env < 0.0f) env = 0.0f;
        if (env > 1.0f) env = 1.0f;
        float k = p->intensity * env;

        int ix = (int)sx;
        int iy = (int)sy;
        float r = p->color.r * k;
        float g = p->color.g * k;
        float b = p->color.b * k;

        /* Photons / wind / debris move enough that the decay tail draws the
         * trail; dust barely moves, so a multi-pixel stamp reads as a
         * static cross. Single sub-pixel for everything keeps the field
         * sparse and starlike. */
        fb_add(fb, ix, iy, r, g, b);
        if (p->kind == PK_PHOTON || p->kind == PK_SOLAR_WIND) {
            /* Tiny halo only on emissive types — gives streaks a hint of
             * bloom without the cross artifact. */
            fb_add(fb, ix + 1, iy, r * 0.25f, g * 0.25f, b * 0.25f);
            fb_add(fb, ix - 1, iy, r * 0.25f, g * 0.25f, b * 0.25f);
            fb_add(fb, ix, iy + 1, r * 0.25f, g * 0.25f, b * 0.25f);
            fb_add(fb, ix, iy - 1, r * 0.25f, g * 0.25f, b * 0.25f);
        }
    }
}
