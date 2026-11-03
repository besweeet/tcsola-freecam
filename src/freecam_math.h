/* Free camera math. Pure C, no OS dependencies, so it can be unit-tested natively.
 *
 * The game stores the camera as a 4x4 matrix of four 16-byte rows (x,y,z,w):
 * three orientation rows followed by a position row. The world is Y-up.
 * Which orientation row is right / up / forward is detected at activation time
 * (see fc_init) rather than assumed. */
#ifndef FREECAM_MATH_H
#define FREECAM_MATH_H
#include <math.h>

typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static inline V3 v3_sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 v3_mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v3_len(V3 a) { return sqrtf(v3_dot(a, a)); }
static inline V3 v3_cross(V3 a, V3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

#define FC_PITCH_MAX 1.5533f /* ~89 degrees */

typedef struct {
    int   fi, ui, ri;       /* which matrix row (0..2) holds forward / up / right */
    float sF, sU, sR;       /* sign (+1/-1) applied when writing each row back */
    float yaw, pitch;       /* radians. yaw 0 = looking along +Z, increasing toward +X */
    V3    pos;
} FreeCam;

static inline float fc_sign(float v) { return v >= 0.0f ? 1.0f : -1.0f; }

/* m: 16 floats (4 rows x 4). player: world position of the player, or NULL.
 * forceFi: -1 = auto, else 0..2.  forceSF: 0 = auto, else +1/-1. */
static inline void fc_init(FreeCam *c, const float *m, const V3 *player, int forceFi, int forceSF)
{
    V3 r[3];
    int i, fi = 2, a = -1, b = -1, ri, ui;
    float sF = 1.0f;
    for (i = 0; i < 3; i++) r[i] = v3(m[i * 4], m[i * 4 + 1], m[i * 4 + 2]);
    c->pos = v3(m[12], m[13], m[14]);

    /* Forward = the row best aligned with the direction toward the player
     * (the stock game camera always looks at the player). */
    if (player) {
        V3 d = v3_sub(*player, c->pos);
        float l = v3_len(d);
        if (l > 1.0f) {   /* too close and the direction is noise: fall back to the stock layout */
            float best = -1.0f;
            d = v3_mul(d, 1.0f / l);
            for (i = 0; i < 3; i++) {
                float dp = v3_dot(r[i], d);
                if (fabsf(dp) > best) { best = fabsf(dp); fi = i; sF = fc_sign(dp); }
            }
        }
    }
    if (forceFi >= 0 && forceFi <= 2) fi = forceFi;
    if (forceSF != 0) sF = forceSF > 0 ? 1.0f : -1.0f;

    /* Of the other two rows, the one closest to horizontal is "right"
     * (a non-rolled camera's right vector has y == 0); the other is "up". */
    for (i = 0; i < 3; i++) if (i != fi) { if (a < 0) a = i; else b = i; }
    if (fabsf(r[a].y) <= fabsf(r[b].y)) { ri = a; ui = b; } else { ri = b; ui = a; }

    c->fi = fi; c->ri = ri; c->ui = ui; c->sF = sF;
    c->sU = fc_sign(r[ui].y);

    {
        V3 F = v3_mul(r[fi], sF);
        float fy = F.y;
        if (fy > 1.0f) fy = 1.0f;
        if (fy < -1.0f) fy = -1.0f;
        c->yaw = atan2f(F.x, F.z);
        c->pitch = asinf(fy);
        if (c->pitch > FC_PITCH_MAX) c->pitch = FC_PITCH_MAX;
        if (c->pitch < -FC_PITCH_MAX) c->pitch = -FC_PITCH_MAX;
        /* Our "right" is Y x F. Record whether the game's right row points the same way. */
        c->sR = fc_sign(v3_dot(r[ri], v3(cosf(c->yaw), 0.0f, -sinf(c->yaw))));
    }
}

static inline void fc_basis(const FreeCam *c, V3 *F, V3 *R, V3 *U)
{
    float cp = cosf(c->pitch);
    *F = v3(cp * sinf(c->yaw), sinf(c->pitch), cp * cosf(c->yaw));
    *R = v3(cosf(c->yaw), 0.0f, -sinf(c->yaw));
    *U = v3_cross(*F, *R);
}

/* move: fwd/strafe/vert are -1, 0 or +1; dist = speed * dt */
static inline void fc_update(FreeCam *c, float dyaw, float dpitch,
                             float fwd, float strafe, float vert, float dist)
{
    V3 F, R, U;
    c->yaw += dyaw;
    c->pitch += dpitch;
    if (c->pitch > FC_PITCH_MAX) c->pitch = FC_PITCH_MAX;
    if (c->pitch < -FC_PITCH_MAX) c->pitch = -FC_PITCH_MAX;
    fc_basis(c, &F, &R, &U);
    c->pos.x += (F.x * fwd + R.x * strafe) * dist;
    c->pos.y += (F.y * fwd + R.y * strafe + vert) * dist;
    c->pos.z += (F.z * fwd + R.z * strafe) * dist;
}

/* Overwrite xyz of the three orientation rows and the position row. w is untouched. */
static inline void fc_write(const FreeCam *c, float *m)
{
    V3 F, R, U;
    V3 row[3];
    int i;
    fc_basis(c, &F, &R, &U);
    row[c->fi] = v3_mul(F, c->sF);
    row[c->ui] = v3_mul(U, c->sU);
    row[c->ri] = v3_mul(R, c->sR);
    for (i = 0; i < 3; i++) { m[i * 4] = row[i].x; m[i * 4 + 1] = row[i].y; m[i * 4 + 2] = row[i].z; }
    m[12] = c->pos.x; m[13] = c->pos.y; m[14] = c->pos.z;
}
#endif
