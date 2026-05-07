#include <math.h>
#include <stdlib.h>

#include "body.h"
#include "config.h"
#include "palette.h"
#include "vwconfig.h"

static const Color PLANET_PALETTE[] = {
    { 0.70f, 0.62f, 0.50f }, /* desert  */
    { 0.50f, 0.62f, 0.74f }, /* ice     */
    { 0.66f, 0.48f, 0.40f }, /* rust    */
    { 0.42f, 0.62f, 0.52f }, /* terra   */
    { 0.62f, 0.50f, 0.66f }, /* gas     */
};
#define PLANET_PALETTE_N ((int)(sizeof PLANET_PALETTE / sizeof PLANET_PALETTE[0]))

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static float circular_v(float gm_central, float r) {
    return sqrtf(gm_central / r);
}

int body_system_init(BodySystem *bs, int sub_w, int sub_h) {
    /* Capacity: central + planets + neutron + black-hole + nebula-core. */
    int cap = 1 + PLANET_COUNT + 1 + 1 + 1;
    bs->bodies = calloc((size_t)cap, sizeof(Body));
    if (!bs->bodies) return -1;
    bs->capacity = cap;
    bs->count    = 0;

    float cx = (float)sub_w * 0.5f;
    float cy = (float)sub_h * 0.5f;
    float rmin = (float)((sub_w < sub_h) ? sub_w : sub_h);

    /* Central G-class star. */
    Body *star = &bs->bodies[bs->count++];
    star->wx        = cx;
    star->wy        = cy;
    star->vx        = 0.0f;
    star->vy        = 0.0f;
    star->mass      = CENTRAL_MASS;
    star->radius    = CENTRAL_RADIUS;
    star->intensity = CENTRAL_INTENSITY;
    star->color     = g_palette.star_g;
    star->kind      = BODY_STAR;

    float gm = g_config.gravity_g * CENTRAL_MASS;

    /* Planets in roughly-circular orbits, random initial phase, all CCW. */
    for (int i = 0; i < PLANET_COUNT; i++) {
        float r     = PLANET_ORBIT_FRAC[i] * rmin;
        float theta = frand() * 6.2831853f;
        float v     = circular_v(gm, r);

        Body *p     = &bs->bodies[bs->count++];
        p->wx       = cx + r * cosf(theta);
        p->wy       = cy + r * sinf(theta);
        /* Tangent (90deg CCW from radial). */
        p->vx       = -v * sinf(theta);
        p->vy       =  v * cosf(theta);
        p->mass     = 0.1f + 1.9f * frand();
        p->radius   = 1.0f + 0.6f * frand();
        p->intensity= 0.55f + 0.25f * frand();
        p->color    = PLANET_PALETTE[i % PLANET_PALETTE_N];
        p->kind     = BODY_PLANET;
    }

    /* Neutron-star companion on an eccentric orbit. */
    {
        float r     = NEUTRON_ORBIT_FRAC * rmin;
        float theta = frand() * 6.2831853f;
        float v     = circular_v(gm, r) * NEUTRON_ECCENTRIC;

        Body *n     = &bs->bodies[bs->count++];
        n->wx       = cx + r * cosf(theta);
        n->wy       = cy + r * sinf(theta);
        n->vx       = -v * sinf(theta);
        n->vy       =  v * cosf(theta);
        n->mass     = 4.0f;
        n->radius   = 0.7f;
        n->intensity= 1.4f;
        n->color    = g_palette.star_b;
        n->kind     = BODY_NEUTRON_STAR;
    }

    /* Black-hole companion: tight bright pinprick (we can't render true
     * darkness against an additive framebuffer, so it reads as a hot
     * accretion-ring point source rather than a literal void). Circular
     * orbit, low mass — keeps the system stable. */
    {
        float r     = BLACKHOLE_ORBIT_FRAC * rmin;
        float theta = frand() * 6.2831853f;
        float v     = circular_v(gm, r);

        Body *h     = &bs->bodies[bs->count++];
        h->wx       = cx + r * cosf(theta);
        h->wy       = cy + r * sinf(theta);
        h->vx       = -v * sinf(theta);
        h->vy       =  v * cosf(theta);
        h->mass     = BLACKHOLE_MASS;
        h->radius   = 0.5f;
        h->intensity= 1.8f;
        h->color    = (Color){ 1.00f, 0.55f, 0.20f }; /* warm accretion */
        h->kind     = BODY_BLACK_HOLE;
    }

    /* Nebula-core companion: large soft violet glow on a far circular orbit.
     * Effectively passive (mass 0.1) so it doesn't perturb the inner system. */
    {
        float r     = NEBULA_ORBIT_FRAC * rmin;
        float theta = frand() * 6.2831853f;
        float v     = circular_v(gm, r);

        Body *m     = &bs->bodies[bs->count++];
        m->wx       = cx + r * cosf(theta);
        m->wy       = cy + r * sinf(theta);
        m->vx       = -v * sinf(theta);
        m->vy       =  v * cosf(theta);
        m->mass     = NEBULA_MASS;
        m->radius   = 4.5f;
        m->intensity= 0.45f;
        m->color    = g_palette.nebula_violet;
        m->kind     = BODY_NEBULA_CORE;
    }

    return 0;
}

void body_system_free(BodySystem *bs) {
    free(bs->bodies);
    bs->bodies = NULL;
    bs->count = bs->capacity = 0;
}

/* Semi-implicit Euler: accumulate accel from all-pairs, integrate velocity,
 * then position. With softening this is stable for the scales we care about
 * and conserves energy well enough that circular orbits stay round. */
void body_step(BodySystem *bs, float dt) {
    int n = bs->count;
    Body *b = bs->bodies;

    for (int i = 0; i < n; i++) {
        float ax = 0.0f, ay = 0.0f;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            float dx = b[j].wx - b[i].wx;
            float dy = b[j].wy - b[i].wy;
            float r2 = dx * dx + dy * dy + GRAVITY_SOFTEN_SQ;
            float inv_r  = 1.0f / sqrtf(r2);
            float inv_r3 = inv_r * inv_r * inv_r;
            float f = g_config.gravity_g * b[j].mass * inv_r3;
            ax += f * dx;
            ay += f * dy;
        }
        b[i].vx += ax * dt;
        b[i].vy += ay * dt;
    }
    for (int i = 0; i < n; i++) {
        b[i].wx += b[i].vx * dt;
        b[i].wy += b[i].vy * dt;
    }
}

void body_draw(const BodySystem *bs, Framebuffer *fb,
               float cam_x, float cam_y,
               const AudioSnapshot *snap) {
    /* Stars (and the neutron companion) brighten with bass. Planets stay
     * flat — they reflect light, they don't emit. */
    float bass_gain = 1.0f + snap->bands[BAND_BASS] * MOD_BASS_BODY;

    for (int i = 0; i < bs->count; i++) {
        const Body *b = &bs->bodies[i];
        float intensity = b->intensity;
        if (b->kind == BODY_STAR || b->kind == BODY_NEUTRON_STAR) {
            intensity *= bass_gain;
        }

        float sx = b->wx - cam_x;
        float sy = b->wy - cam_y;

        float sigma = b->radius * GLOW_SIGMA_K;
        if (sigma < 0.5f) sigma = 0.5f;
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = b->radius * GLOW_BBOX_K;

        int x0 = (int)floorf(sx - box);
        int x1 = (int)ceilf (sx + box);
        int y0 = (int)floorf(sy - box);
        int y1 = (int)ceilf (sy + box);

        /* Cull entirely off-screen bodies. */
        if (x1 < 0 || y1 < 0)                 continue;
        if (x0 >= fb->sub_w || y0 >= fb->sub_h) continue;

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;

        /* Chromatic core: blend toward white at the bright centre so stars
         * read as hot-core + coloured halo instead of flat tinted blobs.
         * Planets stay flat — they reflect, they don't emit. */
        float core_blend = 0.0f;
        if      (b->kind == BODY_STAR)         core_blend = 0.70f;
        else if (b->kind == BODY_NEUTRON_STAR) core_blend = 0.90f;
        else if (b->kind == BODY_BLACK_HOLE)   core_blend = 0.95f;
        /* Nebula core is a diffuse cloud, not a point source — keep it flat. */

        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - sx;
                float d2 = dx * dx + dy * dy;
                float t  = expf(-d2 / two_sigma_sq);
                float k  = t * intensity;
                if (k < 0.005f) continue;

                float w = t * t * core_blend;     /* quadratic — only near centre */
                float r = (b->color.r * (1.0f - w) + w) * k;
                float g = (b->color.g * (1.0f - w) + w) * k;
                float bl = (b->color.b * (1.0f - w) + w) * k;
                fb_add(fb, x, y, r, g, bl);
            }
        }
    }
}
