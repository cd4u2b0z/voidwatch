#ifndef VOIDWATCH_BODY_H
#define VOIDWATCH_BODY_H

#include "audio.h"
#include "config.h"
#include "framebuffer.h"

typedef enum {
    BODY_PLANET = 0,
    BODY_STAR,
    BODY_BLACK_HOLE,
    BODY_NEUTRON_STAR,
    BODY_NEBULA_CORE,
} BodyKind;

typedef struct {
    float    wx, wy;     /* world position, sub-pixels */
    float    vx, vy;     /* velocity, sub-pixels/sec   */
    float    mass;
    float    radius;     /* visual radius, sub-pixels  */
    float    intensity;  /* peak additive brightness   */
    Color    color;
    BodyKind kind;
} Body;

typedef struct {
    Body *bodies;
    int   count;
    int   capacity;
} BodySystem;

int  body_system_init(BodySystem *bs, int sub_w, int sub_h);
void body_system_free(BodySystem *bs);

/* Semi-implicit (symplectic) Euler. dt in seconds. */
void body_step(BodySystem *bs, float dt);

/* Soft Gaussian glow, additive into the framebuffer. */
void body_draw(const BodySystem *bs, Framebuffer *fb,
               float cam_x, float cam_y,
               const AudioSnapshot *snap);

#endif
