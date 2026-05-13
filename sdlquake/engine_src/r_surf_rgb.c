/*
r_surf_rgb.c -- coloured-light surface-cache block writers.

These are RGB siblings of R_DrawSurfaceBlock8_mip* in r_surf.c. Each writer
reads three corner light values per channel from blocklights_rgb (in the
same 8.8 fixed range as mono blocklights[]), bilinearly interpolates per
texel, computes basepal[texel] * light_RGB, quantises to 6 bits per channel,
and looks up the nearest palette index in rgbtable[].

The lightR_l/r and lightR_lstep/rstep pairs mirror the mono writers'
lightleft/lightright pairs, replicated per channel. Inner-loop variable
naming (bx for column, by for row, r6/g6/b6 for quantised channels) avoids
shadowing the column counter.
*/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#define RGB_LIGHT_INTEGER(L) ((L) >> 8)
#define RGB_SHIFT 8

void R_DrawSurfaceBlock8_mip0_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 4;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 4;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 4;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 4;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 4;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 4;

        for (by = 0; by < 16; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 4;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 4;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 4;

            lightR = lightR_r;
            lightG = lightG_r;
            lightB = lightB_r;

            for (bx = 15; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}

void R_DrawSurfaceBlock8_mip1_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 3;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 3;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 3;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 3;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 3;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 3;

        for (by = 0; by < 8; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 3;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 3;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 3;

            lightR = lightR_r; lightG = lightG_r; lightB = lightB_r;

            for (bx = 7; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}

void R_DrawSurfaceBlock8_mip2_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 2;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 2;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 2;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 2;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 2;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 2;

        for (by = 0; by < 4; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 2;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 2;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 2;

            lightR = lightR_r; lightG = lightG_r; lightB = lightB_r;

            for (bx = 3; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}

void R_DrawSurfaceBlock8_mip3_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 1;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 1;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 1;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 1;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 1;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 1;

        for (by = 0; by < 2; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 1;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 1;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 1;

            lightR = lightR_r; lightG = lightG_r; lightB = lightB_r;

            for (bx = 1; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}
