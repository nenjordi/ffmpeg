/*
 * Copyright (C) 2004 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (C) 2008 David Conrad
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avcodec.h"
#include "dsputil.h"
#include "dwt.h"

static inline int mirror(int v, int m){
    if     (v<0) return -v;
    else if(v>m) return 2*m-v;
    else         return v;
}

static inline int extend(int v, int m){
    if     (v<0) return 0;
    else if(v>m) return m;
    else         return v;
}

static inline int extend_odd(int v, int m){
    if     (v<1) return 1;
    else if(v>m) return m;
    else         return v;
}

static av_always_inline
void interleave(IDWTELEM *dst, IDWTELEM *src0, IDWTELEM *src1, int width,
                int add, int shift){
    int i;
    for (i = 0; i < width>>1; i++) {
        dst[2*i  ] = (src0[i] + add) >> shift;
        dst[2*i+1] = (src1[i] + add) >> shift;
    }
}

static void horizontal_compose_dirac53i(IDWTELEM *b, int w){
    IDWTELEM temp[w];
    const int w2= w >> 1;
    int x;

    temp[0] = COMPOSE_53iL0(b[w2], b[0], b[w2]);
    for (x = 0; x < w2-1; x++) {
        temp[x+1 ] = COMPOSE_53iL0     (b[x+w2], b[x+1 ], b[x+w2+1]);
        temp[x+w2] = COMPOSE_DIRAC53iH0(temp[x], b[x+w2], temp[x+1]);
    }
    temp[w-1] = COMPOSE_DIRAC53iH0(temp[w2-1], b[w-1], temp[w2-1]);

    interleave(b, temp, temp+w2, w, 1, 1);
}

static void horizontal_compose_dd97i(IDWTELEM *b, int w){
    IDWTELEM temp[w];
    const int w2 = w >> 1;
    int x;

    temp[0] = COMPOSE_53iL0(b[w2], b[0], b[w2]);
    for (x = 0; x < w2-1; x++)
        temp[x+1] = COMPOSE_53iL0(b[x+w2], b[x+1], b[x+w2+1]);

    temp[w2] = COMPOSE_DD97iH0(temp[0], temp[0], b[w2], temp[1], temp[2]);
    for (x = 0; x < w2-2; x++)
        temp[x+w2+1] = COMPOSE_DD97iH0(temp[x], temp[x+1], b[x+w2+1], temp[x+2], temp[x+3]);

    temp[w-2] = COMPOSE_DD97iH0(temp[w2-3], temp[w2-2], b[w-2], temp[w2-1], temp[w2-1]);
    temp[w-1] = COMPOSE_DD97iH0(temp[w2-2], temp[w2-1], b[w-1], temp[w2-1], temp[w2-1]);

    interleave(b, temp, temp+w2, w, 1, 1);
}

static void horizontal_compose_dd137i(IDWTELEM *b, int w){
    IDWTELEM temp[w];
    const int w2 = w >> 1;
    int x;

    temp[0] = COMPOSE_DD137iL0(b[w2], b[w2], b[0], b[w2  ], b[w2+1]);
    temp[1] = COMPOSE_DD137iL0(b[w2], b[w2], b[1], b[w2+1], b[w2+2]);
    for (x = 0; x < w2-3; x++)
        temp[x+2] = COMPOSE_DD137iL0(b[x+w2], b[x+w2+1], b[x+2], b[x+w2+2], b[x+w2+3]);
    temp[w2-1] = COMPOSE_DD137iL0(b[w-3], b[w-2], b[w2-1], b[w-1], b[w-1]);

    temp[w2] = COMPOSE_DD97iH0(temp[0], temp[0], b[w2], temp[1], temp[2]);
    for (x = 0; x < w2-2; x++)
        temp[x+w2+1] = COMPOSE_DD97iH0(temp[x], temp[x+1], b[x+w2+1], temp[x+2], temp[x+3]);
    temp[w-2] = COMPOSE_DD97iH0(temp[w2-3], temp[w2-2], b[w-2], temp[w2-1], temp[w2-1]);
    temp[w-1] = COMPOSE_DD97iH0(temp[w2-2], temp[w2-1], b[w-1], temp[w2-1], temp[w2-1]);

    interleave(b, temp, temp+w2, w, 1, 1);
}

static av_always_inline void horizontal_compose_haari(IDWTELEM *b, int w, int shift){
    IDWTELEM temp[w];
    const int w2= w >> 1;
    int x;

    for (x = 0; x < w2; x++) {
        temp[x   ] = COMPOSE_HAARiL0(b[x   ], b[x+w2]);
        temp[x+w2] = COMPOSE_HAARiH0(b[x+w2], temp[x]);
    }

    interleave(b, temp, temp+w2, w, shift, shift);
}

static void horizontal_compose_haar0i(IDWTELEM *b, int w){
    horizontal_compose_haari(b, w, 0);
}

static void horizontal_compose_haar1i(IDWTELEM *b, int w){
    horizontal_compose_haari(b, w, 1);
}

static void horizontal_compose_daub97i(IDWTELEM *b, int w) {
    IDWTELEM temp[w];
    const int w2 = w >> 1;
    int x, b0, b1, b2;

    temp[0] = COMPOSE_DAUB97iL1(b[w2], b[0], b[w2]);
    for (x = 0; x < w2-1; x++) {
        temp[ x+1] = COMPOSE_DAUB97iL1(b[x+w2], b[ x+1], b[x+w2+1]);
        temp[w2+x] = COMPOSE_DAUB97iH1(temp[x], b[w2+x], temp[x+1]);
    }
    temp[w-1] = COMPOSE_DAUB97iH1(temp[w2-1], b[w-1], temp[w2-1]);

    // second stage combined with interleave and shift
    b0 = b2 = COMPOSE_DAUB97iL0(temp[w2], temp[0], temp[w2]);
    b[0] = (b0 + 1) >> 1;
    for (x = 0; x < w2-1; x++) {
        b2 = COMPOSE_DAUB97iL0(temp[x+w2], temp[ x+1], temp[x+w2+1 ]);
        b1 = COMPOSE_DAUB97iH0(        b0, temp[w2+x], b2);
        b[2*x + 1] = (b1 + 1) >> 1;
        b[2*(x+1)] = (b2 + 1) >> 1;
        b0 = b2;
    }
    b[w-1] = (COMPOSE_DAUB97iH0(b2, temp[w-1], b2) + 1) >> 1;
}


static void vertical_compose53iL0(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2,
                                  int width){
    int i;

    for(i=0; i<width; i++){
        b1[i] = COMPOSE_53iL0(b0[i], b1[i], b2[i]);
    }
}

static void vertical_compose_dirac53iH0(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2,
                                        int width){
    int i;

    for(i=0; i<width; i++){
        b1[i] = COMPOSE_DIRAC53iH0(b0[i], b1[i], b2[i]);
    }
}

static void vertical_compose_dd97iH0(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2,
                                  IDWTELEM *b3, IDWTELEM *b4, int width){
    int i;

    for(i=0; i<width; i++){
        b2[i] = COMPOSE_DD97iH0(b0[i], b1[i], b2[i], b3[i], b4[i]);
    }
}

static void vertical_compose_dd137iL0(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2,
                                      IDWTELEM *b3, IDWTELEM *b4, int width){
    int i;

    for(i=0; i<width; i++){
        b2[i] = COMPOSE_DD137iL0(b0[i], b1[i], b2[i], b3[i], b4[i]);
    }
}

static void vertical_compose_haariL0(IDWTELEM *b0, IDWTELEM *b1, int width){
    int i;

    for(i=0; i<width; i++){
        b0[i] = COMPOSE_HAARiL0(b0[i], b1[i]);
    }
}

static void vertical_compose_haariH0(IDWTELEM *b0, IDWTELEM *b1, int width){
    int i;

    for(i=0; i<width; i++){
        b0[i] = COMPOSE_HAARiH0(b0[i], b1[i]);
    }
}

static void vertical_compose_daub97iH0(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2, int width){
    int i;

    for(i=0; i<width; i++){
        b1[i] = COMPOSE_DAUB97iH0(b0[i], b1[i], b2[i]);
    }
}

static void vertical_compose_daub97iH1(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2, int width){
    int i;

    for(i=0; i<width; i++){
        b1[i] = COMPOSE_DAUB97iH1(b0[i], b1[i], b2[i]);
    }
}

static void vertical_compose_daub97iL0(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2, int width){
    int i;

    for(i=0; i<width; i++){
        b1[i] = COMPOSE_DAUB97iL0(b0[i], b1[i], b2[i]);
    }
}

static void vertical_compose_daub97iL1(IDWTELEM *b0, IDWTELEM *b1, IDWTELEM *b2, int width){
    int i;

    for(i=0; i<width; i++){
        b1[i] = COMPOSE_DAUB97iL1(b0[i], b1[i], b2[i]);
    }
}


static void spatial_compose_dd97i_dy(DWTContext *d, int level, int width, int height, int stride)
{
    void (*vertical_compose_l0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_l0;
    void (*vertical_compose_h0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, IDWTELEM *, IDWTELEM *, int)
        = d->vertical_compose_h0;
    DWTCompose *cs = d->cs + level;

    int i, y = cs->y;
    IDWTELEM *b[8];
    for (i = 0; i < 6; i++)
        b[i] = cs->b[i];
    b[6] = d->buffer + extend(y+5, height-2)*stride;
    b[7] = d->buffer + mirror(y+6, height-1)*stride;

        if(y+5<(unsigned)height) vertical_compose_l0(      b[5], b[6], b[7],       width);
        if(y+1<(unsigned)height) vertical_compose_h0(b[0], b[2], b[3], b[4], b[6], width);

        if(y-1<(unsigned)height) d->horizontal_compose(b[0], width);
        if(y+0<(unsigned)height) d->horizontal_compose(b[1], width);

    for (i = 0; i < 6; i++)
        cs->b[i] = b[i+2];
    cs->y += 2;
}

static void spatial_compose_dirac53i_dy(DWTContext *d, int level, int width, int height, int stride)
{
    void (*vertical_compose_l0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_l0;
    void (*vertical_compose_h0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_h0;
    DWTCompose *cs = d->cs + level;

    int y= cs->y;
    IDWTELEM *b[4] = { cs->b[0], cs->b[1] };
    b[2] = d->buffer + mirror(y+1, height-1)*stride;
    b[3] = d->buffer + mirror(y+2, height-1)*stride;

        if(y+1<(unsigned)height) vertical_compose_l0(b[1], b[2], b[3], width);
        if(y+0<(unsigned)height) vertical_compose_h0(b[0], b[1], b[2], width);

        if(y-1<(unsigned)height) d->horizontal_compose(b[0], width);
        if(y+0<(unsigned)height) d->horizontal_compose(b[1], width);

    cs->b[0] = b[2];
    cs->b[1] = b[3];
    cs->y += 2;
}


static void spatial_compose_dd137i_dy(DWTContext *d, int level, int width, int height, int stride)
{
    void (*vertical_compose_l0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, IDWTELEM *, IDWTELEM *, int)
        = d->vertical_compose_l0;
    void (*vertical_compose_h0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, IDWTELEM *, IDWTELEM *, int)
        = d->vertical_compose_h0;
    DWTCompose *cs = d->cs + level;

    int i, y = cs->y;
    IDWTELEM *b[10];
    for (i = 0; i < 8; i++)
        b[i] = cs->b[i];
    b[8] = d->buffer + extend    (y+7, height-2)*stride;
    b[9] = d->buffer + extend_odd(y+8, height-1)*stride;

        if(y+5<(unsigned)height) vertical_compose_l0(b[3], b[5], b[6], b[7], b[9], width);
        if(y+1<(unsigned)height) vertical_compose_h0(b[0], b[2], b[3], b[4], b[6], width);

        if(y-1<(unsigned)height) d->horizontal_compose(b[0], width);
        if(y+0<(unsigned)height) d->horizontal_compose(b[1], width);

    for (i = 0; i < 8; i++)
        cs->b[i] = b[i+2];
    cs->y += 2;
}

static void spatial_compose_haari_dy(DWTContext *d, int level, int width, int height, int stride)
{
    void (*vertical_compose_l0)(IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_l0;
    void (*vertical_compose_h0)(IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_h0;

    int y = d->cs[level].y;
    IDWTELEM *b0 = d->buffer + (y-1)*stride;
    IDWTELEM *b1 = d->buffer + (y  )*stride;

        if(y-1<(unsigned)height) vertical_compose_l0(b0, b1, width);
        if(y+0<(unsigned)height) vertical_compose_h0(b1, b0, width);

        if(y-1<(unsigned)height) d->horizontal_compose(b0, width);
        if(y+0<(unsigned)height) d->horizontal_compose(b1, width);

    d->cs[level].y += 2;
}

static void spatial_compose_daub97i_dy(DWTContext *d, int level, int width, int height, int stride)
{
    void (*vertical_compose_l0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_l0;
    void (*vertical_compose_h0)(IDWTELEM *, IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_h0;
    void (*vertical_compose_l1)(IDWTELEM *, IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_l1;
    void (*vertical_compose_h1)(IDWTELEM *, IDWTELEM *, IDWTELEM *, int) = d->vertical_compose_h1;
    DWTCompose *cs = d->cs + level;

    int i, y = cs->y;
    IDWTELEM *b[6];
    for (i = 0; i < 4; i++)
        b[i] = cs->b[i];
    b[4] = d->buffer + mirror(y+3, height-1)*stride;
    b[5] = d->buffer + mirror(y+4, height-1)*stride;

        if(y+3<(unsigned)height) vertical_compose_l1(b[3], b[4], b[5], width);
        if(y+2<(unsigned)height) vertical_compose_h1(b[2], b[3], b[4], width);
        if(y+1<(unsigned)height) vertical_compose_l0(b[1], b[2], b[3], width);
        if(y+0<(unsigned)height) vertical_compose_h0(b[0], b[1], b[2], width);

        if(y-1<(unsigned)height) d->horizontal_compose(b[0], width);
        if(y+0<(unsigned)height) d->horizontal_compose(b[1], width);

    for (i = 0; i < 4; i++)
        cs->b[i] = b[i+2];
    cs->y += 2;
}


static void spatial_compose97i_init(DWTCompose *cs, IDWTELEM *buffer, int height, int stride){
    cs->b[0] = buffer + mirror(-3-1, height-1)*stride;
    cs->b[1] = buffer + mirror(-3  , height-1)*stride;
    cs->b[2] = buffer + mirror(-3+1, height-1)*stride;
    cs->b[3] = buffer + mirror(-3+2, height-1)*stride;
    cs->y = -3;
}

static void spatial_compose53i_init(DWTCompose *cs, IDWTELEM *buffer,
                                    int height, int stride){
    cs->b[0] = buffer + mirror(-1-1, height-1)*stride;
    cs->b[1] = buffer + mirror(-1  , height-1)*stride;
    cs->y = -1;
}

static void spatial_compose_dd97i_init(DWTCompose *cs, IDWTELEM *buffer,
                                       int height, int stride){
    cs->b[0] = buffer + extend(-5-1, height-2)*stride;
    cs->b[1] = buffer + mirror(-5  , height-1)*stride;
    cs->b[2] = buffer + extend(-5+1, height-2)*stride;
    cs->b[3] = buffer + mirror(-5+2, height-1)*stride;
    cs->b[4] = buffer + extend(-5+3, height-2)*stride;
    cs->b[5] = buffer + mirror(-5+4, height-1)*stride;
    cs->y = -5;
}

static void spatial_compose_dd137i_init(DWTCompose *cs, IDWTELEM *buffer,
                                        int height, int stride){
    cs->b[0] = buffer + extend    (-5-1, height-2)*stride;
    cs->b[1] = buffer + extend_odd(-5  , height-1)*stride;
    cs->b[2] = buffer + extend    (-5+1, height-2)*stride;
    cs->b[3] = buffer + extend_odd(-5+2, height-1)*stride;
    cs->b[4] = buffer + extend    (-5+3, height-2)*stride;
    cs->b[5] = buffer + extend_odd(-5+4, height-1)*stride;
    cs->b[6] = buffer + extend    (-5+5, height-2)*stride;
    cs->b[7] = buffer + extend_odd(-5+6, height-1)*stride;
    cs->y = -5;
}

void ff_spatial_idwt_init_mmx(DWTContext *d, enum dwt_type type);

int ff_spatial_idwt_init2(DWTContext *d, IDWTELEM *buffer, int width, int height,
                          int stride, enum dwt_type type, int decomposition_count)
{
    int level;

    d->buffer = buffer;
    d->width = width;
    d->height = height;
    d->stride = stride;
    d->decomposition_count = decomposition_count;

    for(level=decomposition_count-1; level>=0; level--){
        int hl = height >> level;
        int stride_l = stride << level;

        switch(type){
        case DWT_DIRAC_DD9_7:
            spatial_compose_dd97i_init(d->cs+level, buffer, hl, stride_l);
            break;
        case DWT_DIRAC_LEGALL5_3:
            spatial_compose53i_init(d->cs+level, buffer, hl, stride_l);
            break;
        case DWT_DIRAC_DD13_7:
            spatial_compose_dd137i_init(d->cs+level, buffer, hl, stride_l);
            break;
        case DWT_DIRAC_HAAR0:
        case DWT_DIRAC_HAAR1:
            d->cs[level].y = 1;
            break;
        case DWT_DIRAC_DAUB9_7:
            spatial_compose97i_init(d->cs+level, buffer, hl, stride_l);
            break;
        }
    }

    switch (type) {
    case DWT_DIRAC_DD9_7:
        d->spatial_compose = spatial_compose_dd97i_dy;
        d->vertical_compose_l0 = vertical_compose53iL0;
        d->vertical_compose_h0 = vertical_compose_dd97iH0;
        d->horizontal_compose = horizontal_compose_dd97i;
        d->support = 7;
        break;
    case DWT_DIRAC_LEGALL5_3:
        d->spatial_compose = spatial_compose_dirac53i_dy;
        d->vertical_compose_l0 = vertical_compose53iL0;
        d->vertical_compose_h0 = vertical_compose_dirac53iH0;
        d->horizontal_compose = horizontal_compose_dirac53i;
        d->support = 3;
        break;
    case DWT_DIRAC_DD13_7:
        d->spatial_compose = spatial_compose_dd137i_dy;
        d->vertical_compose_l0 = vertical_compose_dd137iL0;
        d->vertical_compose_h0 = vertical_compose_dd97iH0;
        d->horizontal_compose = horizontal_compose_dd137i;
        d->support = 7;
        break;
    case DWT_DIRAC_HAAR0:
    case DWT_DIRAC_HAAR1:
        d->spatial_compose = spatial_compose_haari_dy;
        d->vertical_compose_l0 = vertical_compose_haariL0;
        d->vertical_compose_h0 = vertical_compose_haariH0;
        if (type == DWT_DIRAC_HAAR0)
            d->horizontal_compose = horizontal_compose_haar0i;
        else
            d->horizontal_compose = horizontal_compose_haar1i;
        d->support = 1;
        break;
    case DWT_DIRAC_DAUB9_7:
        d->spatial_compose = spatial_compose_daub97i_dy;
        d->vertical_compose_l0 = vertical_compose_daub97iL0;
        d->vertical_compose_h0 = vertical_compose_daub97iH0;
        d->vertical_compose_l1 = vertical_compose_daub97iL1;
        d->vertical_compose_h1 = vertical_compose_daub97iH1;
        d->horizontal_compose = horizontal_compose_daub97i;
        d->support = 5;
        break;
    default:
        av_log(NULL, AV_LOG_ERROR, "Unknown wavelet type %d\n", type);
        return -1;
    }

    if (HAVE_MMX) ff_spatial_idwt_init_mmx(d, type);

    return 0;
}

void ff_spatial_idwt_slice2(DWTContext *d, int y)
{
    int level, support = d->support;

    for (level = d->decomposition_count-1; level >= 0; level--) {
        int wl = d->width  >> level;
        int hl = d->height >> level;
        int stride_l = d->stride << level;

        while (d->cs[level].y <= FFMIN((y>>level)+support, hl))
            d->spatial_compose(d, level, wl, hl, stride_l);
    }
}

int ff_spatial_idwt2(IDWTELEM *buffer, int width, int height,
                     int stride, enum dwt_type type, int decomposition_count)
{
    DWTContext d;
    int y;

    if (ff_spatial_idwt_init2(&d, buffer, width, height, stride, type, decomposition_count))
        return -1;

    for (y = 0; y < d.height; y += 4)
        ff_spatial_idwt_slice2(&d, y);

    return 0;
}
