#include <stdio.h>
#include <string.h>
#include "freecam_math.h"

static void build(float *m, int perm[3], float sg[3], float yaw, float pitch, V3 pos)
{
    float cp = cosf(pitch);
    V3 F = v3(cp * sinf(yaw), sinf(pitch), cp * cosf(yaw));
    V3 R = v3(cosf(yaw), 0, -sinf(yaw));
    V3 U = v3_cross(F, R);
    V3 src[3] = { R, U, F };   /* canonical (right, up, forward) */
    int i;
    memset(m, 0, 16 * sizeof(float));
    for (i = 0; i < 3; i++) {
        V3 v = v3_mul(src[perm[i]], sg[i]);
        m[i * 4] = v.x; m[i * 4 + 1] = v.y; m[i * 4 + 2] = v.z; m[i * 4 + 3] = 0;
    }
    m[12] = pos.x; m[13] = pos.y; m[14] = pos.z; m[15] = 1;
}

int main(void)
{
    int perms[6][3] = { {0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0} };
    float worst = 0; int fails = 0, cases = 0;
    int p, s, y, t;
    for (p = 0; p < 6; p++)
    for (s = 0; s < 8; s++)
    for (y = 0; y < 12; y++)
    for (t = -3; t <= 3; t++) {
        float sg[3] = { (s & 1) ? -1.f : 1.f, (s & 2) ? -1.f : 1.f, (s & 4) ? -1.f : 1.f };
        float yaw = (float)y * 0.5f - 3.0f, pitch = (float)t * 0.35f;  /* up to ~60 deg */
        V3 pos = v3(163.6f, -39.3f, 2138.3f), F, player;
        float m[16], out[16], d; int i;
        FreeCam c;
        { float cp = cosf(pitch); F = v3(cp*sinf(yaw), sinf(pitch), cp*cosf(yaw)); }
        player = v3(pos.x + F.x * 3.0f + 0.2f, pos.y + F.y * 3.0f - 0.1f, pos.z + F.z * 3.0f);
        build(m, perms[p], sg, yaw, pitch, pos);
        memcpy(out, m, sizeof m);
        fc_init(&c, m, &player, -1, 0);
        /* scramble then rewrite: must reproduce the original matrix */
        memset(out, 0, sizeof out);
        out[3] = out[7] = out[11] = 0; out[15] = 1;
        fc_write(&c, out);
        d = 0;
        for (i = 0; i < 15; i++) if (fabsf(out[i] - m[i]) > d) d = fabsf(out[i] - m[i]);
        if (d > worst) worst = d;
        cases++;
        if (d > 1e-3f) { fails++; if (fails < 5) printf("FAIL perm=%d s=%d yaw=%.2f pitch=%.2f err=%.5f fi=%d\n", p, s, yaw, pitch, d, c.fi); }
    }
    printf("round-trip: %d cases, %d failures, worst error %.6f\n", cases, fails, worst);

    /* behaviour checks on the canonical layout */
    {
        int perm[3] = {0,1,2}; float sg[3] = {1,1,1}; float m[16]; FreeCam c; V3 pos = v3(0,0,0), pl = v3(0,0,3), F,R,U;
        build(m, perm, sg, 0, 0, pos);
        fc_init(&c, m, &pl, -1, 0);
        {   /* player nearly on top of the camera (0.7 away, off to the side): must still pick the stock layout */
            FreeCam c2; V3 near_ = v3(0.5f, 0.3f, 0.4f); float m2[16];
            build(m2, perm, sg, 0.404f, 1.474f, pos);
            fc_init(&c2, m2, &near_, -1, 0);
            printf("close-player detect: fi=%d ui=%d ri=%d (expect 2 1 0)\n", c2.fi, c2.ui, c2.ri);
        }
        printf("canonical detect: fi=%d ui=%d ri=%d sF=%.0f sU=%.0f sR=%.0f yaw=%.3f pitch=%.3f\n", c.fi, c.ui, c.ri, c.sF, c.sU, c.sR, c.yaw, c.pitch);
        fc_update(&c, 0.0f, 0.0f, 1, 0, 0, 2.0f);           /* forward 2 units */
        printf("forward 2 -> pos (%.2f %.2f %.2f)  expect (0 0 2)\n", c.pos.x, c.pos.y, c.pos.z);
        fc_update(&c, 0.0f, 0.0f, 0, 1, 0, 1.0f);           /* strafe right 1 */
        printf("strafe right 1 -> pos (%.2f %.2f %.2f)  expect (1 0 2)\n", c.pos.x, c.pos.y, c.pos.z);
        fc_update(&c, 0.5f, 0.0f, 0, 0, 0, 0);              /* turn right */
        fc_basis(&c, &F, &R, &U);
        printf("yaw +0.5 -> F (%.2f %.2f %.2f) toward +X (right): %s\n", F.x, F.y, F.z, F.x > 0 ? "yes" : "NO");
        fc_update(&c, 0.0f, 0.4f, 0, 0, 0, 0);              /* look up */
        fc_basis(&c, &F, &R, &U);
        printf("pitch +0.4 -> F.y = %.2f (positive = up): %s\n", F.y, F.y > 0 ? "yes" : "NO");
        fc_update(&c, 0.0f, 0.0f, 0, 0, 1, 3.0f);
        printf("vertical +3 -> y = %.2f  expect 3\n", c.pos.y);
    }
    return fails ? 1 : 0;
}
