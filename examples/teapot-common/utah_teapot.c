/*
 * Utah teapot tessellator for GPU FPS probes.
 *
 * Control-point tables are the classic Newell teapot as shipped with GLUT:
 * Copyright (c) Mark J. Kilgard, 1994, 2001.
 * (c) Copyright 1993, Silicon Graphics, Inc.
 * Permission to use, copy, modify, and distribute this software for any
 * purpose and without fee is hereby granted, provided that the above
 * copyright notice appear in all copies.
 */

#include "utah_teapot.h"

#include <math.h>

#define TEAPOT_GRID 6
#define TEAPOT_PATCHES 32
#define TEAPOT_MAX_VERTS (TEAPOT_PATCHES * TEAPOT_GRID * TEAPOT_GRID * 2 * 3)

static const int patchdata[][16] = {
    {102, 103, 104, 105, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27},
    {24, 25, 26, 27, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40},
    {96, 96, 96, 96, 97, 98, 99, 100, 101, 101, 101, 101, 0, 1, 2, 3},
    {0, 1, 2, 3, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117},
    {118, 118, 118, 118, 124, 122, 119, 121, 123, 126, 125, 120, 40, 39, 38, 37},
    {41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56},
    {53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 28, 65, 66, 67},
    {68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83},
    {80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95},
};

static const float cpdata[][3] = {
    {0.2f, 0.f, 2.7f}, {0.2f, -0.112f, 2.7f}, {0.112f, -0.2f, 2.7f},
    {0.f, -0.2f, 2.7f}, {1.3375f, 0.f, 2.53125f}, {1.3375f, -0.749f, 2.53125f},
    {0.749f, -1.3375f, 2.53125f}, {0.f, -1.3375f, 2.53125f},
    {1.4375f, 0.f, 2.53125f}, {1.4375f, -0.805f, 2.53125f},
    {0.805f, -1.4375f, 2.53125f}, {0.f, -1.4375f, 2.53125f},
    {1.5f, 0.f, 2.4f}, {1.5f, -0.84f, 2.4f}, {0.84f, -1.5f, 2.4f},
    {0.f, -1.5f, 2.4f}, {1.75f, 0.f, 1.875f}, {1.75f, -0.98f, 1.875f},
    {0.98f, -1.75f, 1.875f}, {0.f, -1.75f, 1.875f}, {2.f, 0.f, 1.35f},
    {2.f, -1.12f, 1.35f}, {1.12f, -2.f, 1.35f}, {0.f, -2.f, 1.35f},
    {2.f, 0.f, 0.9f}, {2.f, -1.12f, 0.9f}, {1.12f, -2.f, 0.9f},
    {0.f, -2.f, 0.9f}, {-2.f, 0.f, 0.9f}, {2.f, 0.f, 0.45f},
    {2.f, -1.12f, 0.45f}, {1.12f, -2.f, 0.45f}, {0.f, -2.f, 0.45f},
    {1.5f, 0.f, 0.225f}, {1.5f, -0.84f, 0.225f}, {0.84f, -1.5f, 0.225f},
    {0.f, -1.5f, 0.225f}, {1.5f, 0.f, 0.15f}, {1.5f, -0.84f, 0.15f},
    {0.84f, -1.5f, 0.15f}, {0.f, -1.5f, 0.15f}, {-1.6f, 0.f, 2.025f},
    {-1.6f, -0.3f, 2.025f}, {-1.5f, -0.3f, 2.25f}, {-1.5f, 0.f, 2.25f},
    {-2.3f, 0.f, 2.025f}, {-2.3f, -0.3f, 2.025f}, {-2.5f, -0.3f, 2.25f},
    {-2.5f, 0.f, 2.25f}, {-2.7f, 0.f, 2.025f}, {-2.7f, -0.3f, 2.025f},
    {-3.f, -0.3f, 2.25f}, {-3.f, 0.f, 2.25f}, {-2.7f, 0.f, 1.8f},
    {-2.7f, -0.3f, 1.8f}, {-3.f, -0.3f, 1.8f}, {-3.f, 0.f, 1.8f},
    {-2.7f, 0.f, 1.575f}, {-2.7f, -0.3f, 1.575f}, {-3.f, -0.3f, 1.35f},
    {-3.f, 0.f, 1.35f}, {-2.5f, 0.f, 1.125f}, {-2.5f, -0.3f, 1.125f},
    {-2.65f, -0.3f, 0.9375f}, {-2.65f, 0.f, 0.9375f}, {-2.f, -0.3f, 0.9f},
    {-1.9f, -0.3f, 0.6f}, {-1.9f, 0.f, 0.6f}, {1.7f, 0.f, 1.425f},
    {1.7f, -0.66f, 1.425f}, {1.7f, -0.66f, 0.6f}, {1.7f, 0.f, 0.6f},
    {2.6f, 0.f, 1.425f}, {2.6f, -0.66f, 1.425f}, {3.1f, -0.66f, 0.825f},
    {3.1f, 0.f, 0.825f}, {2.3f, 0.f, 2.1f}, {2.3f, -0.25f, 2.1f},
    {2.4f, -0.25f, 2.025f}, {2.4f, 0.f, 2.025f}, {2.7f, 0.f, 2.4f},
    {2.7f, -0.25f, 2.4f}, {3.3f, -0.25f, 2.4f}, {3.3f, 0.f, 2.4f},
    {2.8f, 0.f, 2.475f}, {2.8f, -0.25f, 2.475f}, {3.525f, -0.25f, 2.49375f},
    {3.525f, 0.f, 2.49375f}, {2.9f, 0.f, 2.475f}, {2.9f, -0.15f, 2.475f},
    {3.45f, -0.15f, 2.5125f}, {3.45f, 0.f, 2.5125f}, {2.8f, 0.f, 2.4f},
    {2.8f, -0.15f, 2.4f}, {3.2f, -0.15f, 2.4f}, {3.2f, 0.f, 2.4f},
    {0.f, 0.f, 3.15f}, {0.8f, 0.f, 3.15f}, {0.8f, -0.45f, 3.15f},
    {0.45f, -0.8f, 3.15f}, {0.f, -0.8f, 3.15f}, {0.f, 0.f, 2.85f},
    {1.4f, 0.f, 2.4f}, {1.4f, -0.784f, 2.4f}, {0.784f, -1.4f, 2.4f},
    {0.f, -1.4f, 2.4f}, {0.4f, 0.f, 2.55f}, {0.4f, -0.224f, 2.55f},
    {0.224f, -0.4f, 2.55f}, {0.f, -0.4f, 2.55f}, {1.3f, 0.f, 2.55f},
    {1.3f, -0.728f, 2.55f}, {0.728f, -1.3f, 2.55f}, {0.f, -1.3f, 2.55f},
    {1.3f, 0.f, 2.4f}, {1.3f, -0.728f, 2.4f}, {0.728f, -1.3f, 2.4f},
    {0.f, -1.3f, 2.4f}, {0.f, 0.f, 0.f}, {1.425f, -0.798f, 0.f},
    {1.5f, 0.f, 0.075f}, {1.425f, 0.f, 0.f}, {0.798f, -1.425f, 0.f},
    {0.f, -1.5f, 0.075f}, {0.f, -1.425f, 0.f}, {1.5f, -0.84f, 0.075f},
    {0.84f, -1.5f, 0.075f},
};

static float g_pos[TEAPOT_MAX_VERTS * 3];
static float g_nrm[TEAPOT_MAX_VERTS * 3];
static float g_clip[TEAPOT_MAX_VERTS * 3];
static float g_rgb[TEAPOT_MAX_VERTS * 3];
static int g_nverts;
static int g_ready;

static float bernstein(int i, float t)
{
    float u = 1.f - t;

    switch (i) {
    case 0:
        return u * u * u;
    case 1:
        return 3.f * u * u * t;
    case 2:
        return 3.f * u * t * t;
    default:
        return t * t * t;
    }
}

static float dbernstein(int i, float t)
{
    float u = 1.f - t;

    switch (i) {
    case 0:
        return -3.f * u * u;
    case 1:
        return 3.f * u * u - 6.f * u * t;
    case 2:
        return 6.f * u * t - 3.f * t * t;
    default:
        return 3.f * t * t;
    }
}

static void eval_patch(const float net[4][4][3], float u, float v, float p[3],
                       float n[3])
{
    float du[3] = {0.f, 0.f, 0.f};
    float dv[3] = {0.f, 0.f, 0.f};
    int i, j, k;
    float len;

    p[0] = p[1] = p[2] = 0.f;
    for (i = 0; i < 4; i++) {
        float bi = bernstein(i, v);
        float dbi = dbernstein(i, v);
        for (j = 0; j < 4; j++) {
            float bj = bernstein(j, u);
            float dbj = dbernstein(j, u);
            for (k = 0; k < 3; k++) {
                float c = net[i][j][k];
                p[k] += bi * bj * c;
                du[k] += bi * dbj * c;
                dv[k] += dbi * bj * c;
            }
        }
    }
    n[0] = du[1] * dv[2] - du[2] * dv[1];
    n[1] = du[2] * dv[0] - du[0] * dv[2];
    n[2] = du[0] * dv[1] - du[1] * dv[0];
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-8f) {
        n[0] = 0.f;
        n[1] = 1.f;
        n[2] = 0.f;
    } else {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    }
}

/* GLUT: Rx(270) * scale(0.5) * translate(0,0,-1.5) → Y-up, centered. */
static void to_yup(float p[3], float n[3])
{
    float x = p[0] * 0.5f;
    float y = (p[2] - 1.5f) * 0.5f;
    float z = -p[1] * 0.5f;
    float nx = n[0];
    float ny = n[2];
    float nz = -n[1];
    float len = sqrtf(nx * nx + ny * ny + nz * nz);

    p[0] = x;
    p[1] = y;
    p[2] = z;
    if (len > 1e-8f) {
        nx /= len;
        ny /= len;
        nz /= len;
    }
    n[0] = nx;
    n[1] = ny;
    n[2] = nz;
}

static void fill_net(float net[4][4][3], int patch, int kind)
{
    int j, k, l;

    for (j = 0; j < 4; j++) {
        for (k = 0; k < 4; k++) {
            int src = k;
            int idx;
            float v[3];

            if (kind == 1 || kind == 2)
                src = 3 - k;
            idx = patchdata[patch][j * 4 + src];
            for (l = 0; l < 3; l++)
                v[l] = cpdata[idx][l];
            if (kind == 1)
                v[1] = -v[1];
            if (kind == 2)
                v[0] = -v[0];
            if (kind == 3) {
                v[0] = -v[0];
                v[1] = -v[1];
            }
            net[j][k][0] = v[0];
            net[j][k][1] = v[1];
            net[j][k][2] = v[2];
        }
    }
}

static void emit_vert(const float p[3], const float n[3])
{
    int i = g_nverts;

    if (i >= TEAPOT_MAX_VERTS)
        return;
    g_pos[i * 3 + 0] = p[0];
    g_pos[i * 3 + 1] = p[1];
    g_pos[i * 3 + 2] = p[2];
    g_nrm[i * 3 + 0] = n[0];
    g_nrm[i * 3 + 1] = n[1];
    g_nrm[i * 3 + 2] = n[2];
    g_nverts++;
}

static void tessellate_net(const float net[4][4][3])
{
    int iu, iv;
    float grid = (float)TEAPOT_GRID;

    for (iv = 0; iv < TEAPOT_GRID; iv++) {
        for (iu = 0; iu < TEAPOT_GRID; iu++) {
            float p00[3], p10[3], p01[3], p11[3];
            float n00[3], n10[3], n01[3], n11[3];
            float u0 = (float)iu / grid;
            float v0 = (float)iv / grid;
            float u1 = (float)(iu + 1) / grid;
            float v1 = (float)(iv + 1) / grid;

            eval_patch(net, u0, v0, p00, n00);
            eval_patch(net, u1, v0, p10, n10);
            eval_patch(net, u0, v1, p01, n01);
            eval_patch(net, u1, v1, p11, n11);
            to_yup(p00, n00);
            to_yup(p10, n10);
            to_yup(p01, n01);
            to_yup(p11, n11);
            emit_vert(p00, n00);
            emit_vert(p10, n10);
            emit_vert(p01, n01);
            emit_vert(p10, n10);
            emit_vert(p11, n11);
            emit_vert(p01, n01);
        }
    }
}

int teapot_ensure(void)
{
    int i;

    if (g_ready)
        return g_nverts;
    g_nverts = 0;
    for (i = 0; i < 10; i++) {
        float net[4][4][3];
        int kinds = (i < 6) ? 4 : 2;
        int k;

        for (k = 0; k < kinds; k++) {
            fill_net(net, i, k);
            tessellate_net(net);
        }
    }
    g_ready = g_nverts > 0;
    return g_ready ? g_nverts : -1;
}

int teapot_nverts(void)
{
    return g_nverts;
}

const float *teapot_clip(void)
{
    return g_clip;
}

const float *teapot_rgb(void)
{
    return g_rgb;
}

void teapot_update(float seconds, float aspect, float r, float g, float b)
{
    float ang = seconds * 0.85f;
    float ca = cosf(ang);
    float sa = sinf(ang);
    float tilt = -0.42f;
    float ct = cosf(tilt);
    float st = sinf(tilt);
    float f = 1.f / tanf(0.40f);
    float nearp = 1.f;
    float farp = 20.f;
    float lx = 0.35f, ly = 0.78f, lz = 0.52f;
    float llen = sqrtf(lx * lx + ly * ly + lz * lz);
    int i;

    if (aspect < 0.1f)
        aspect = 0.1f;
    lx /= llen;
    ly /= llen;
    lz /= llen;
    for (i = 0; i < g_nverts; i++) {
        float x = g_pos[i * 3 + 0];
        float y = g_pos[i * 3 + 1];
        float z = g_pos[i * 3 + 2];
        float nx = g_nrm[i * 3 + 0];
        float ny = g_nrm[i * 3 + 1];
        float nz = g_nrm[i * 3 + 2];
        float x1 = x * ca + z * sa;
        float z1 = -x * sa + z * ca;
        float y2 = y * ct - z1 * st;
        float z2 = y * st + z1 * ct;
        float nx1 = nx * ca + nz * sa;
        float nz1 = -nx * sa + nz * ca;
        float ny2 = ny * ct - nz1 * st;
        float nz2 = ny * st + nz1 * ct;
        float w;
        float zc;
        float ndotl;
        float shade;

        z2 -= 5.2f;
        w = -z2;
        if (w < 0.08f)
            w = 0.08f;
        zc = ((farp + nearp) / (nearp - farp)) * z2 +
             (2.f * farp * nearp) / (nearp - farp);
        g_clip[i * 3 + 0] = (f / aspect) * x1 / w;
        g_clip[i * 3 + 1] = f * y2 / w;
        g_clip[i * 3 + 2] = zc / w;
        ndotl = nx1 * lx + ny2 * ly + nz2 * lz;
        if (ndotl < 0.f)
            ndotl = -ndotl;
        shade = 0.20f + 0.80f * ndotl;
        g_rgb[i * 3 + 0] = r * shade;
        g_rgb[i * 3 + 1] = g * shade;
        g_rgb[i * 3 + 2] = b * shade;
    }
}
