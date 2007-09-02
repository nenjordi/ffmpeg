/* -*-  indent-tabs-mode:nil; c-basic-offset:4;  -*- */
/*
 * Copyright (C) 2007 Marco Gerards <marco@gnu.org>
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

/**
 * @file dirac.c
 * Dirac Decoder
 * @author Marco Gerards <marco@gnu.org>
 */

//#define DEBUG 1

#include "avcodec.h"
#include "dsputil.h"
#include "bitstream.h"
#include "bytestream.h"
#include "golomb.h"
#include "dirac_arith.h"
#include "dirac_wavelet.h"
#include "mpeg12data.h"

typedef enum {
    TRANSFER_FUNC_TV,
    TRANSFER_FUNC_EXTENDED_GAMUT,
    TRANSFER_FUNC_LINEAR,
    TRANSFER_FUNC_DCI_GAMMA
} transfer_func_t;

#define DIRAC_SIGN(x) ((x) > 0 ? 2 : ((x) < 0 ? 1 : 0))
#define DIRAC_PARSE_INFO_PREFIX 0x42424344

struct source_parameters
{
    /* Interlacing.  */
    char interlaced;                     ///< flag for interlacing
    char top_field_first;
    char sequential_fields;

    AVRational frame_rate;             ///< frame rate

    AVRational aspect_ratio;           ///< aspect ratio

    /* Clean area.  */
    uint16_t clean_width;
    uint16_t clean_height;
    uint16_t clean_left_offset;
    uint16_t clean_right_offset;

    /* Luma and chroma offsets.  */
    uint16_t luma_offset;
    uint16_t luma_excursion;
    uint16_t chroma_offset;
    uint16_t chroma_excursion;

    uint16_t color_spec;
    uint16_t color_primaries; /* XXX: ??? */

    float k_r;
    float k_b; /* XXX: ??? */

    transfer_func_t transfer_function;
};

struct sequence_parameters
{
    /* Information about the frames.  */
    unsigned int luma_width;                    ///< the luma component width
    unsigned int luma_height;                   ///< the luma component height
    /** Choma format: 0: 4:4:4, 1: 4:2:2, 2: 4:2:0 */
    unsigned int chroma_format;
    unsigned char video_depth;                  ///< depth in bits

    /* Calculated:  */
    unsigned int chroma_width;                  ///< the chroma component width
    unsigned int chroma_height;                 ///< the chroma component height
};

struct decoding_parameters
{
    uint8_t wavelet_depth;                 ///< depth of the IDWT
    uint8_t wavelet_idx_intra;             ///< wavelet transform for intra frames
    uint8_t wavelet_idx_inter;             ///< wavelet transform for inter frames

    uint8_t luma_xbsep;
    uint8_t luma_xblen;
    uint8_t luma_ybsep;
    uint8_t luma_yblen;

    uint8_t mv_precision;

    int16_t picture_weight_ref1;
    int16_t picture_weight_ref2;
    unsigned int picture_weight_precision;

    /* Codeblocks h*v.  */
    int intra_hlevel_012, intra_vlevel_012;
    int intra_hlevel_other, intra_vlevel_other;
    int inter_hlevel_01, inter_vlevel_01;
    int inter_hlevel_2, inter_vlevel_2;
    int inter_hlevel_other, inter_vlevel_other;

    int slice_width;
    int slide_height;
    int slice_bits;

    /* Calculated.  */
    uint8_t chroma_xbsep;
    uint8_t chroma_xblen;
    uint8_t chroma_ybsep;
    uint8_t chroma_yblen;
};

struct globalmc_parameters {
    unsigned int b[2];                          ///< b vector
    unsigned int A[2][2];                       ///< A matrix
    int c[2];                                   ///< c vector
    unsigned int zrs_exp;
    unsigned int perspective_exp;
};

/* Defaults for sequence parameters.  */
static const struct sequence_parameters sequence_parameters_defaults[13] =
{
    /* Width   Height   Chroma format   Depth  */
    {  640,    480,     2,              8  },
    {  176,    120,     2,              8  },
    {  176,    144,     2,              8  },
    {  352,    240,     2,              8  },
    {  352,    288,     2,              8  },
    {  704,    480,     2,              8  },
    {  704,    576,     2,              8  },

    {  720,    480,     2,              8  },
    {  720,    576,     2,              8  },
    {  1280,   720,     2,              8  },
    {  1920,   1080,    2,              8  },
    {  2048,   1556,    0,              16 },
    {  4096,   3112,    0,              16 },
};

/* Defaults for source parameters.  */
static const struct source_parameters source_parameters_defaults[13] =
{
    { 0, 1, 0, {30, 1},        {1, 1},   640,  480,  0, 0, 0,  255,   128,   254,   0, 0, 0.2126, 0.0722, TRANSFER_FUNC_TV },
    { 0, 1, 0, {15000, 1001},  {10, 11}, 176,  120,  0, 0, 0,  255,   128,   254,   1, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },
    { 0, 1, 0, {25, 2},        {12, 11}, 176,  144,  0, 0, 0,  255,   128,   254,   2, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },
    { 0, 1, 0, {15000, 1001},  {10, 11}, 352,  240,  0, 0, 0,  255,   128,   254,   1, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },
    { 0, 1, 0, {25, 2},        {12, 11}, 352,  288,  0, 0, 0,  255,   128,   254,   2, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },
    { 0, 1, 0, {15000, 1001},  {10, 11}, 704,  480,  0, 0, 0,  255,   128,   254,   1, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },
    { 0, 1, 0, {25, 2},        {12, 11}, 704,  576,  0, 0, 0,  255,   128,   254,   2, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },

    { 0, 1, 0, {24000, 1001},  {10, 11}, 720,  480,  0, 0, 16, 235,   128,   224,   1, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },
    { 0, 1, 0, {35, 1},        {12, 11}, 720,  576,  0, 0, 16, 235,   128,   224,   2, 0, 0.299,  0.144,  TRANSFER_FUNC_TV },
    { 0, 1, 0, {24, 1},        {1, 1},   1280, 720,  0, 0, 16, 235,   128,   224,   0, 0, 0.2126, 0.0722, TRANSFER_FUNC_TV },
    { 0, 1, 0, {24, 1},        {1, 1},   1920, 1080, 0, 0, 16, 235,   128,   224,   0, 0, 0.2126, 0.0722, TRANSFER_FUNC_TV },
    { 0, 1, 0, {24, 1},        {1, 1},   2048, 1536, 0, 0, 0,  65535, 32768, 65534, 3, 0, 0.25,   0.25,   TRANSFER_FUNC_LINEAR },
    { 0, 1, 0, {24, 1},        {1, 1},   4096, 3072, 0, 0, 0,  65535, 32768, 65534, 3, 0, 0.25,   0.25,   TRANSFER_FUNC_LINEAR },
};

/* Defaults for decoding parameters.  */
static const struct decoding_parameters decoding_parameters_defaults[13] =
{
    { 4, 0, 1, 8,  12, 8,  12, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 32, 32, 512  },
    { 4, 0, 1, 4,   8, 4,   8, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 16, 16, 512  },
    { 4, 0, 1, 4,   8, 4,   8, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 16, 16, 512  },
    { 4, 0, 1, 8,  12, 8,  12, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 32, 32, 512  },
    { 4, 0, 1, 8,  12, 8,  12, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 32, 32, 512  },
    { 4, 0, 1, 8,  12, 8,  12, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 32, 32, 512  },
    { 4, 0, 1, 8,  12, 8,  12, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 32, 32, 512  },

    { 4, 0, 1, 8,  12, 8,  12, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 32, 32, 512  },
    { 4, 0, 1, 8,  12, 8,  12, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 32, 32, 512  },
    { 4, 0, 1, 12, 16, 12, 16, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 48, 48, 768  },
    { 4, 0, 1, 16, 24, 16, 24, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 48, 48, 1024 },
    { 4, 6, 1, 16, 24, 16, 24, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 48, 48, 1024 },
    { 4, 6, 0, 16, 24, 16, 24, 2, 1, 1, 1, 1, 1, 4, 3, 1, 1, 8, 6, 12, 8, 48, 48, 1024 }
};

static const AVRational preset_aspect_ratios[3] =
{
    {1, 1}, {10, 11}, {12, 11}
};

static const uint8_t preset_luma_offset[3] = { 0, 16, 64 };
static const uint16_t preset_luma_excursion[3] = { 255, 235, 876 };
static const uint16_t preset_chroma_offset[3] = { 128, 128, 512 };
static const uint16_t preset_chroma_excursion[3] = { 255, 224, 896 };

static const uint8_t preset_primaries[4] = { 0, 1, 2, 3 };
static const uint8_t preset_matrix[4] = {0, 1, 1, 2 };
static const transfer_func_t preset_transfer_func[4] =
{
    TRANSFER_FUNC_TV, TRANSFER_FUNC_TV, TRANSFER_FUNC_TV, TRANSFER_FUNC_DCI_GAMMA
};
static const float preset_kr[3] = { 0.2126, 0.299, 0 /* XXX */ };
static const float preset_kb[3] = {0.0722, 0.114, 0 /* XXX */ };

/* Weights for qpel/eighth pel interpolation.  */
typedef uint8_t weights_t[4];

/* Quarter pixel interpolation.  */
static const weights_t qpel_weights[4] = {
    {  4,  0,  0,  0 }, /* rx=0, ry=0 */
    {  2,  0,  2,  0 }, /* rx=0, ry=1 */
    {  2,  2,  0,  0 }, /* rx=1, ry=0 */
    {  1,  1,  1,  1 }, /* rx=1, ry=1 */
};

static const weights_t eighthpel_weights[16] = {
    { 16,  0,  0,  0 }, /* rx=0, ry=0 */
    { 12,  0,  4,  0 }, /* rx=0, ry=1 */
    {  8,  0,  8,  0 }, /* rx=0, ry=2 */
    {  4,  0, 12,  0 }, /* rx=0, ry=3 */
    { 12,  4,  0,  0 }, /* rx=1, ry=0 */
    {  9,  3,  3,  1 }, /* rx=1, ry=1 */
    {  6,  2,  6,  2 }, /* rx=1, ry=2 */
    {  3,  1,  9,  3 }, /* rx=1, ry=3 */
    {  8,  8,  0,  0 }, /* rx=2, ry=0 */
    {  6,  6,  2,  2 }, /* rx=2, ry=1 */
    {  4,  4,  4,  4 }, /* rx=2, ry=2 */
    {  2,  2,  6,  6 }, /* rx=2, ry=3 */
    {  4, 12,  0,  0 }, /* rx=3, ry=0 */
    {  3,  9,  1,  3 }, /* rx=3, ry=1 */
    {  2,  6,  2,  6 }, /* rx=3, ry=2 */
    {  1,  3,  3,  9 }, /* rx=3, ry=3 */
};

typedef int16_t vect_t[2];

#define DIRAC_REF_MASK_REF1   1
#define DIRAC_REF_MASK_REF2   2
#define DIRAC_REF_MASK_GLOBAL 4

struct dirac_blockmotion {
    uint8_t use_ref;
    vect_t vect[2];
    int16_t dc[3];
};

/* XXX */
#define REFFRAME_CNT 20

struct reference_frame {
    AVFrame frame;
    uint8_t *halfpel[3];
};

typedef struct DiracContext {
    unsigned int profile;
    unsigned int level;

    AVCodecContext *avctx;
    GetBitContext gb;

    PutBitContext pb;
    int next_parse_code;
    char *encodebuf;
    int prev_size;

    AVFrame picture;

    uint32_t picnum;
    int refcnt;
    struct reference_frame refframes[REFFRAME_CNT]; /* XXX */

    int retirecnt;
    uint32_t retireframe[REFFRAME_CNT];

    struct source_parameters source;
    struct sequence_parameters sequence;
    struct decoding_parameters decoding;

    struct decoding_parameters frame_decoding;

    unsigned int codeblocksh[7]; /* XXX: 7 levels.  */
    unsigned int codeblocksv[7]; /* XXX: 7 levels.  */

    int padded_luma_width;    ///< padded luma width
    int padded_luma_height;   ///< padded luma height
    int padded_chroma_width;  ///< padded chroma width
    int padded_chroma_height; ///< padded chroma height

    int chroma_hshift;        ///< horizontal bits to shift for choma
    int chroma_vshift;        ///< vertical bits to shift for choma

    int blwidth;              ///< number of blocks (horizontally)
    int blheight;             ///< number of blocks (vertically)
    int sbwidth;              ///< number of superblocks (horizontally)
    int sbheight;             ///< number of superblocks (vertically)

    int zero_res;             ///< zero residue flag

    int refs;                 ///< number of reference pictures
    int globalmc_flag;        ///< use global motion compensation flag
    /** global motion compensation parameters */
    struct globalmc_parameters globalmc;
    uint32_t ref[2];          ///< reference pictures
    int16_t *spatialwt;

    uint8_t *refdata[2];
    int refwidth;
    int refheight;

    unsigned int wavelet_idx;

    /* Current component.  */
    int padded_width;         ///< padded width of the current component
    int padded_height;        ///< padded height of the current component
    int width;
    int height;
    int xbsep;
    int ybsep;
    int xblen;
    int yblen;
    int xoffset;
    int yoffset;
    int total_wt_bits;
    int current_blwidth;
    int current_blheight;

    int *sbsplit;
    struct dirac_blockmotion *blmotion;

    /** State of arithmetic decoding.  */
    struct dirac_arith_state arith;
} DiracContext;

static int decode_init(AVCodecContext *avctx){
    av_log_set_level(AV_LOG_DEBUG);
    return 0;
}

static int decode_end(AVCodecContext *avctx)
{
    // DiracContext *s = avctx->priv_data;

    return 0;
}

static int encode_init(AVCodecContext *avctx){
    DiracContext *s = avctx->priv_data;
    av_log_set_level(AV_LOG_DEBUG);

    /* XXX: Choose a better size somehow.  */
    s->encodebuf = av_malloc(1 << 20);

    if (!s->encodebuf) {
        av_log(s->avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }

    return 0;
}

static int encode_end(AVCodecContext *avctx)
{
    DiracContext *s = avctx->priv_data;

    av_free(s->encodebuf);

    return 0;
}


typedef enum {
    pc_access_unit_header = 0x00,
    pc_eos                = 0x10,
    pc_aux_data           = 0x20,
    pc_padding            = 0x60,
    pc_intra_ref          = 0x0c
} parse_code_t;

typedef enum {
    subband_ll = 0,
    subband_hl = 1,
    subband_lh = 2,
    subband_hh = 3
} subband_t;

/**
 * Dump the sequence parameters.  DEBUG needs to be defined.
 */
static void dump_sequence_parameters(AVCodecContext *avctx) {
    DiracContext *s = avctx->priv_data;
    struct sequence_parameters *seq = &s->sequence;
    const char *chroma_format_str[] = { "4:4:4", "4:2:2", "4:2:0" };

    dprintf(avctx, "-----------------------------------------------------\n");
    dprintf(avctx, "        Dumping the sequence parameters:\n");
    dprintf(avctx, "-----------------------------------------------------\n");


    dprintf(avctx, "Luma size=%dx%d\n",
            seq->luma_width, seq->luma_height);
    dprintf(avctx, "Chroma size=%dx%d, format: %s\n",
            seq->chroma_width, seq->chroma_height,
            chroma_format_str[seq->chroma_format]);
    dprintf(avctx, "Video depth: %d bpp\n", seq->video_depth);

    dprintf(avctx, "-----------------------------------------------------\n");

}

/**
 * Dump the source parameters.  DEBUG needs to be defined.
 */
static void dump_source_parameters(AVCodecContext *avctx) {
    DiracContext *s = avctx->priv_data;
    struct source_parameters *source = &s->source;

    dprintf(avctx, "-----------------------------------------------------\n");
    dprintf(avctx, "        Dumping source parameters:\n");
    dprintf(avctx, "-----------------------------------------------------\n");

    if (! source->interlaced)
        dprintf(avctx, "No interlacing\n");
    else
        dprintf(avctx, "Interlacing: top fields first=%d\n, seq. fields=%d\n",
                source->top_field_first, source->sequential_fields);

    dprintf(avctx, "Frame rate: %d/%d = %f\n",
            source->frame_rate.num, source->frame_rate.den,
            (double) source->frame_rate.num / source->frame_rate.den);
    dprintf(avctx, "Aspect ratio: %d/%d = %f\n",
            source->aspect_ratio.num, source->aspect_ratio.den,
            (double) source->aspect_ratio.num / source->aspect_ratio.den);

    dprintf(avctx, "Clean space: loff=%d, roff=%d, size=%dx%d\n",
            source->clean_left_offset, source->clean_right_offset,
            source->clean_width, source->clean_height);

    dprintf(avctx, "Luma offset=%d, Luma excursion=%d\n",
            source->luma_offset, source->luma_excursion);
    dprintf(avctx, "Croma offset=%d, Chroma excursion=%d\n",
            source->chroma_offset, source->chroma_excursion);

    /* XXX: This list is incomplete, add the other members.  */

    dprintf(avctx, "-----------------------------------------------------\n");
}


/**
 * Parse the sequence parameters in the access unit header
 */
static void parse_sequence_parameters(DiracContext *s) {
    GetBitContext *gb = &s->gb;

    /* Override the luma dimensions.  */
    if (get_bits1(gb)) {
        s->sequence.luma_width  = svq3_get_ue_golomb(gb);
        s->sequence.luma_height = svq3_get_ue_golomb(gb);
    }

    /* Override the chroma format.  */
    if (get_bits1(gb))
        s->sequence.chroma_format = svq3_get_ue_golomb(gb);

    /* Calculate the chroma dimensions.  */
    s->chroma_hshift = s->sequence.chroma_format > 0;
    s->chroma_vshift = s->sequence.chroma_format > 1;
    s->sequence.chroma_width  = s->sequence.luma_width  >> s->chroma_hshift;
    s->sequence.chroma_height = s->sequence.luma_height >> s->chroma_vshift;

    /* Override the video depth.  */
    if (get_bits1(gb))
        s->sequence.video_depth = svq3_get_ue_golomb(gb);
}

/**
 * Parse the source parameters in the access unit header
 */
static int parse_source_parameters(DiracContext *s) {
    GetBitContext *gb = &s->gb;

    /* Access Unit Source parameters.  */
    if (get_bits1(gb)) {
        /* Interlace.  */
        s->source.interlaced = get_bits1(gb);

        if (s->source.interlaced) {
            if (get_bits1(gb))
                s->source.top_field_first = get_bits1(gb);

            if (get_bits1(gb))
                s->source.sequential_fields = get_bits1(gb);
        }
    }

    /* Framerate.  */
    if (get_bits1(gb)) {
        unsigned int idx = svq3_get_ue_golomb(gb);

        if (idx > 8)
            return -1;

        if (! idx) {
            s->source.frame_rate.num = svq3_get_ue_golomb(gb);
            s->source.frame_rate.den = svq3_get_ue_golomb(gb);
        } else {
            /* Use a pre-set framerate.  */
            s->source.frame_rate = ff_frame_rate_tab[idx];
        }
    }

    /* Override aspect ratio.  */
    if (get_bits1(gb)) {
        unsigned int idx = svq3_get_ue_golomb(gb);

        if (idx > 3)
            return -1;

        if (! idx) {
            s->source.aspect_ratio.num = svq3_get_ue_golomb(gb);
            s->source.aspect_ratio.den = svq3_get_ue_golomb(gb);
        } else {
            /* Use a pre-set aspect ratio.  */
            s->source.aspect_ratio = preset_aspect_ratios[idx - 1];
        }
    }

    /* Override clean area.  */
    if (get_bits1(gb)) {
        s->source.clean_width        = svq3_get_ue_golomb(gb);
        s->source.clean_height       = svq3_get_ue_golomb(gb);
        s->source.clean_left_offset  = svq3_get_ue_golomb(gb);
        s->source.clean_right_offset = svq3_get_ue_golomb(gb);
    }

    /* Override signal range.  */
    if (get_bits1(gb)) {
        unsigned int idx = svq3_get_ue_golomb(gb);

        if (idx > 3)
            return -1;

        if (! idx) {
            s->source.luma_offset      = svq3_get_ue_golomb(gb);
            s->source.luma_excursion   = svq3_get_ue_golomb(gb);
            s->source.chroma_offset    = svq3_get_ue_golomb(gb);
            s->source.chroma_excursion = svq3_get_ue_golomb(gb);
        } else {
            /* Use a pre-set signal range.  */
            s->source.luma_offset = preset_luma_offset[idx - 1];
            s->source.luma_excursion = preset_luma_excursion[idx - 1];
            s->source.chroma_offset = preset_chroma_offset[idx - 1];
            s->source.chroma_excursion = preset_chroma_excursion[idx - 1];
        }
    }

    /* Color spec.  */
    if (get_bits1(gb)) {
        unsigned int idx = svq3_get_ue_golomb(gb);

        if (idx > 3)
            return -1;

        s->source.color_primaries = preset_primaries[idx];
        s->source.k_r = preset_kr[preset_matrix[idx]];
        s->source.k_b = preset_kb[preset_matrix[idx]];
        s->source.transfer_function = preset_transfer_func[idx];

        /* XXX: color_spec?  */

        if (! idx) {
            /* Color primaries.  */
            if (get_bits1(gb)) {
                unsigned int primaries_idx = svq3_get_ue_golomb(gb);

                if (primaries_idx > 3)
                    return -1;

                s->source.color_primaries = preset_primaries[primaries_idx];
            }

            /* Override matrix.  */
            if (get_bits1(gb)) {
                unsigned int matrix_idx = svq3_get_ue_golomb(gb);

                if (matrix_idx > 3)
                    return -1;

                s->source.k_r = preset_kr[preset_matrix[matrix_idx]];
                s->source.k_b = preset_kb[preset_matrix[matrix_idx]];
            }

            /* Transfer function.  */
            if (get_bits1(gb)) {
                unsigned int tf_idx = svq3_get_ue_golomb(gb);

                if (tf_idx > 3)
                    return -1;

                s->source.transfer_function = preset_transfer_func[tf_idx];
            }
        } else {
            /* XXX: Use the index.  */
        }
    }

    return 0;
}

/**
 * Parse the access unit header
 */
static int parse_access_unit_header(DiracContext *s) {
    GetBitContext *gb = &s->gb;
    unsigned int version_major;
    unsigned int version_minor;
    unsigned int video_format;

    /* Parse parameters.  */
    version_major = svq3_get_ue_golomb(gb);
    version_minor = svq3_get_ue_golomb(gb);
    /* XXX: Don't check the version yet, existing encoders do not yet
       set this to a sane value (0.6 at the moment).  */

    /* XXX: Not yet documented in the spec.  This is actually the main
       thing that is missing.  */
    s->profile = svq3_get_ue_golomb(gb);
    s->level = svq3_get_ue_golomb(gb);
    dprintf(s->avctx, "Access unit header: Version %d.%d\n",
            version_major, version_minor);
    dprintf(s->avctx, "Profile: %d, Level: %d\n", s->profile, s->level);

    video_format = svq3_get_ue_golomb(gb);
    dprintf(s->avctx, "Video format: %d\n", video_format);

    if (video_format > 12)
        return -1;

    /* Fill in defaults for the sequence parameters.  */
    s->sequence = sequence_parameters_defaults[video_format];

    /* Override the defaults.  */
    parse_sequence_parameters(s);

    /* Fill in defaults for the source parameters.  */
    s->source = source_parameters_defaults[video_format];

    /* Override the defaults.  */
    if (parse_source_parameters(s))
        return -1;

    /* Fill in defaults for the decoding parameters.  */
    s->decoding = decoding_parameters_defaults[video_format];

    return 0;
}

static struct dirac_arith_context_set context_set_split =
    {
        .follow = { ARITH_CONTEXT_SB_F1, ARITH_CONTEXT_SB_F2,
                    ARITH_CONTEXT_SB_F2, ARITH_CONTEXT_SB_F2,
                    ARITH_CONTEXT_SB_F2, ARITH_CONTEXT_SB_F2 },
        .data = ARITH_CONTEXT_SB_DATA
    };

static struct dirac_arith_context_set context_set_mv =
    {
        .follow = { ARITH_CONTEXT_VECTOR_F1, ARITH_CONTEXT_VECTOR_F2,
                    ARITH_CONTEXT_VECTOR_F3, ARITH_CONTEXT_VECTOR_F4,
                    ARITH_CONTEXT_VECTOR_F5, ARITH_CONTEXT_VECTOR_F5 },
        .data = ARITH_CONTEXT_VECTOR_DATA,
        .sign = ARITH_CONTEXT_VECTOR_SIGN
    };
static struct dirac_arith_context_set context_set_dc =
    {
        .follow = { ARITH_CONTEXT_DC_F1, ARITH_CONTEXT_DC_F2,
                    ARITH_CONTEXT_DC_F2, ARITH_CONTEXT_DC_F2,
                    ARITH_CONTEXT_DC_F2, ARITH_CONTEXT_DC_F2 },
        .data = ARITH_CONTEXT_DC_DATA,
        .sign = ARITH_CONTEXT_DC_SIGN
    };

static struct dirac_arith_context_set context_sets_waveletcoeff[12] = {
    {
        /* Parent = 0, Zero neighbourhood, sign predict 0 */
        .follow = { ARITH_CONTEXT_ZPZN_F1, ARITH_CONTEXT_ZP_F2,
                    ARITH_CONTEXT_ZP_F3, ARITH_CONTEXT_ZP_F4,
                    ARITH_CONTEXT_ZP_F5, ARITH_CONTEXT_ZP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_ZERO,
    }, {
        /* Parent = 0, Zero neighbourhood, sign predict < 0 */
        .follow = { ARITH_CONTEXT_ZPZN_F1, ARITH_CONTEXT_ZP_F2,
                    ARITH_CONTEXT_ZP_F3, ARITH_CONTEXT_ZP_F4,
                    ARITH_CONTEXT_ZP_F5, ARITH_CONTEXT_ZP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_NEG
    }, {
        /* Parent = 0, Zero neighbourhood, sign predict > 0 */
        .follow = { ARITH_CONTEXT_ZPZN_F1, ARITH_CONTEXT_ZP_F2,
                    ARITH_CONTEXT_ZP_F3, ARITH_CONTEXT_ZP_F4,
                    ARITH_CONTEXT_ZP_F5, ARITH_CONTEXT_ZP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_POS
    },

    {
        /* Parent = 0, No Zero neighbourhood, sign predict  0 */
        .follow = { ARITH_CONTEXT_ZPNN_F1, ARITH_CONTEXT_ZP_F2,
                    ARITH_CONTEXT_ZP_F3, ARITH_CONTEXT_ZP_F4,
                    ARITH_CONTEXT_ZP_F5, ARITH_CONTEXT_ZP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_ZERO
    }, {
        /* Parent = 0, No Zero neighbourhood, sign predict < 0 */
        .follow = { ARITH_CONTEXT_ZPNN_F1, ARITH_CONTEXT_ZP_F2,
                    ARITH_CONTEXT_ZP_F3, ARITH_CONTEXT_ZP_F4,
                    ARITH_CONTEXT_ZP_F5, ARITH_CONTEXT_ZP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_NEG
    }, {
        /* Parent = 0, No Zero neighbourhood, sign predict > 0 */
        .follow = { ARITH_CONTEXT_ZPNN_F1, ARITH_CONTEXT_ZP_F2,
                    ARITH_CONTEXT_ZP_F3, ARITH_CONTEXT_ZP_F4,
                    ARITH_CONTEXT_ZP_F5, ARITH_CONTEXT_ZP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_POS
    },

    {
        /* Parent != 0, Zero neighbourhood, sign predict 0 */
        .follow = { ARITH_CONTEXT_NPZN_F1, ARITH_CONTEXT_NP_F2,
                    ARITH_CONTEXT_NP_F3, ARITH_CONTEXT_NP_F4,
                    ARITH_CONTEXT_NP_F5, ARITH_CONTEXT_NP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_ZERO
    }, {
        /* Parent != 0, Zero neighbourhood, sign predict < 0 */
        .follow = { ARITH_CONTEXT_NPZN_F1, ARITH_CONTEXT_NP_F2,
                    ARITH_CONTEXT_NP_F3, ARITH_CONTEXT_NP_F4,
                    ARITH_CONTEXT_NP_F5, ARITH_CONTEXT_NP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_NEG
    }, {
        /* Parent != 0, Zero neighbourhood, sign predict > 0 */
        .follow = { ARITH_CONTEXT_NPZN_F1, ARITH_CONTEXT_NP_F2,
                    ARITH_CONTEXT_NP_F3, ARITH_CONTEXT_NP_F4,
                    ARITH_CONTEXT_NP_F5, ARITH_CONTEXT_NP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_POS
    },


    {
        /* Parent != 0, No Zero neighbourhood, sign predict 0 */
        .follow = { ARITH_CONTEXT_NPNN_F1, ARITH_CONTEXT_NP_F2,
                    ARITH_CONTEXT_NP_F3, ARITH_CONTEXT_NP_F4,
                    ARITH_CONTEXT_NP_F5, ARITH_CONTEXT_NP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_ZERO
    }, {
        /* Parent != 0, No Zero neighbourhood, sign predict < 0 */
        .follow = { ARITH_CONTEXT_NPNN_F1, ARITH_CONTEXT_NP_F2,
                    ARITH_CONTEXT_NP_F3, ARITH_CONTEXT_NP_F4,
                    ARITH_CONTEXT_NP_F5, ARITH_CONTEXT_NP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_NEG
    }, {
        /* Parent != 0, No Zero neighbourhood, sign predict > 0 */
        .follow = { ARITH_CONTEXT_NPNN_F1, ARITH_CONTEXT_NP_F2,
                    ARITH_CONTEXT_NP_F3, ARITH_CONTEXT_NP_F4,
                    ARITH_CONTEXT_NP_F5, ARITH_CONTEXT_NP_F6 },
        .data = ARITH_CONTEXT_COEFF_DATA,
        .sign = ARITH_CONTEXT_SIGN_POS
    }
};

/**
 * Calculate the width of a subband on a given level
 *
 * @param level subband level
 * @return subband width
 */
static int inline subband_width(DiracContext *s, int level) {
    if (level == 0)
        return s->padded_width >> s->frame_decoding.wavelet_depth;
    return s->padded_width >> (s->frame_decoding.wavelet_depth - level + 1);
}

/**
 * Calculate the height of a subband on a given level
 *
 * @param level subband level
 * @return height of the subband
 */
static int inline subband_height(DiracContext *s, int level) {
    if (level == 0)
        return s->padded_height >> s->frame_decoding.wavelet_depth;
    return s->padded_height >> (s->frame_decoding.wavelet_depth - level + 1);
}

static int inline coeff_quant_factor(int idx) {
    uint64_t base;
    idx = FFMAX(idx, 0);
    base = 1 << (idx / 4);
    switch(idx & 3) {
    case 0:
        return base << 2;
    case 1:
        return (503829 * base + 52958) / 105917;
    case 2:
        return (665857 * base + 58854) / 117708;
    case 3:
        return (440253 * base + 32722) / 65444;
    }
    return 0; /* XXX: should never be reached */
}

static int inline coeff_quant_offset(DiracContext *s, int idx) {
    if (idx == 0)
        return 1;

    if (s->refs == 0) {
        if (idx == 1)
            return 2;
        else
            return (coeff_quant_factor(idx) + 1) >> 1;
    }

    return (coeff_quant_factor(idx) * 3 + 4) / 8;
}

/**
 * Dequantize a coefficient
 *
 * @param coeff coefficient to dequantize
 * @param qoffset quantizer offset
 * @param qfactor quantizer factor
 * @return dequantized coefficient
 */
static int inline coeff_dequant(int coeff,
                                int qoffset, int qfactor) {
    if (! coeff)
        return 0;

    coeff *= qfactor;

    coeff += qoffset;
    coeff >>= 2;

    return coeff;
}

/**
 * Calculate the horizontal position of a coefficient given a level,
 * orientation and horizontal position within the subband.
 *
 * @param level subband level
 * @param orientation orientation of the subband within the level
 * @param x position within the subband
 * @return horizontal position within the coefficient array
 */
static int inline coeff_posx(DiracContext *s, int level,
                             subband_t orientation, int x) {
    if (orientation == subband_hl || orientation == subband_hh)
        return subband_width(s, level) + x;

    return x;
}

/**
 * Calculate the vertical position of a coefficient given a level,
 * orientation and vertical position within the subband.
 *
 * @param level subband level
 * @param orientation orientation of the subband within the level
 * @param y position within the subband
 * @return vertical position within the coefficient array
 */
static int inline coeff_posy(DiracContext *s, int level,
                             subband_t orientation, int y) {
    if (orientation == subband_lh || orientation == subband_hh)
        return subband_height(s, level) + y;

    return y;
}

/**
 * Returns if the pixel has a zero neighbourhood (the coefficient at
 * the left, top and left top of this coefficient are all zero)
 *
 * @param data current coefficient
 * @param v vertical position of the coefficient
 * @param h horizontal position of the coefficient
 * @return 1 if zero neighbourhood, otherwise 0
 */
static int zero_neighbourhood(DiracContext *s, int16_t *data, int v, int h) {
    /* Check if there is a zero to the left and top left of this
       coefficient.  */
    if (v > 0 && (data[-s->padded_width]
                  || ( h > 0 && data[-s->padded_width - 1])))
        return 0;
    else if (h > 0 && data[- 1])
        return 0;

    return 1;
}

/**
 * Determine the most efficient context to use for arithmetic decoding
 * of this coefficient (given by a position in a subband).
 *
 * @param current coefficient
 * @param v vertical position of the coefficient
 * @param h horizontal position of the coefficient
 * @return prediction for the sign: -1 when negative, 1 when positive, 0 when 0
 */
static int sign_predict(DiracContext *s, int16_t *data,
                        subband_t orientation, int v, int h) {
    if (orientation == subband_hl && v > 0)
        return DIRAC_SIGN(data[-s->padded_width]);
    else if (orientation == subband_lh && h > 0)
        return DIRAC_SIGN(data[-1]);
    else
        return 0;
}

/**
 * Unpack a single coefficient
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the subband
 * @param v vertical position of the to be decoded coefficient in the subband
 * @param h horizontal position of the to be decoded coefficient in the subband
 * @param qoffset quantizer offset
 * @param qfact quantizer factor
 */
static void coeff_unpack(DiracContext *s, int16_t *data, int level,
                         subband_t orientation, int v, int h,
                         int qoffset, int qfactor) {
    int parent = 0;
    int nhood;
    int idx;
    int coeff;
    int read_sign;
    struct dirac_arith_context_set *context;
    int16_t *coeffp;
    int vdata, hdata;

    vdata = coeff_posy(s, level, orientation, v);
    hdata = coeff_posx(s, level, orientation, h);

    coeffp = &data[hdata + vdata * s->padded_width];

    /* The value of the pixel belonging to the lower level.  */
    if (level >= 2) {
        int x = coeff_posx(s, level - 1, orientation, h >> 1);
        int y = coeff_posy(s, level - 1, orientation, v >> 1);
        parent = data[s->padded_width * y + x] != 0;
    }

    /* Determine if the pixel has only zeros in its neighbourhood.  */
    nhood = zero_neighbourhood(s, coeffp, v, h);

    /* Calculate an index into context_sets_waveletcoeff.  */
    idx = parent * 6 + (!nhood) * 3;
    idx += sign_predict(s, coeffp, orientation, v, h);

    context = &context_sets_waveletcoeff[idx];

    coeff = dirac_arith_read_uint(&s->arith, context);

    read_sign = coeff;
    coeff = coeff_dequant(coeff, qoffset, qfactor);
    if (read_sign) {
        if (dirac_arith_get_bit(&s->arith, context->sign))
            coeff = -coeff;
    }

    *coeffp = coeff;
}

/**
 * Decode a codeblock
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the current subband
 * @param x position of the codeblock within the subband in units of codeblocks
 * @param y position of the codeblock within the subband in units of codeblocks
 * @param quant quantizer offset
 * @param quant quantizer factor
 */
static void codeblock(DiracContext *s, int16_t *data, int level,
                      subband_t orientation, int x, int y,
                      int qoffset, int qfactor) {
    int blockcnt_one = (s->codeblocksh[level] + s->codeblocksv[level]) == 2;
    int left, right, top, bottom;
    int v, h;

    left   = (subband_width(s, level)  *  x     ) / s->codeblocksh[level];
    right  = (subband_width(s, level)  * (x + 1)) / s->codeblocksh[level];
    top    = (subband_height(s, level) *  y     ) / s->codeblocksv[level];
    bottom = (subband_height(s, level) * (y + 1)) / s->codeblocksv[level];

    if (!blockcnt_one) {
        /* Determine if this codeblock is a zero block.  */
        if (dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_ZERO_BLOCK))
            return;
    }

    for (v = top; v < bottom; v++)
        for (h = left; h < right; h++)
            coeff_unpack(s, data, level, orientation, v, h,
                         qoffset, qfactor);
}

static inline int intra_dc_coeff_prediction(DiracContext *s, int16_t *coeff,
                                            int x, int y) {
    int pred;
    if (x > 0 && y > 0) {
        pred = (coeff[-1]
                + coeff[-s->padded_width]
                + coeff[-s->padded_width - 1]);
        if (pred > 0)
            pred = (pred + 1) / 3;
        else /* XXX: For now just do what the reference
                implementation does.  Check this.  */
            pred = -((-pred)+1)/3;
    } else if (x > 0) {
        /* Just use the coefficient left of this one.  */
                pred = coeff[-1];
    } else if (y > 0)
        pred = coeff[-s->padded_width];
    else
        pred = 0;

    return pred;
}

/**
 * Intra DC Prediction
 *
 * @param data coefficients
 */
static void intra_dc_prediction(DiracContext *s, int16_t *data) {
    int x, y;
    int16_t *line = data;

    for (y = 0; y < subband_height(s, 0); y++) {
        for (x = 0; x < subband_width(s, 0); x++) {
            line[x] += intra_dc_coeff_prediction(s, &line[x], x, y);
        }
        line += s->padded_width;
    }
}

/**
 * Decode a subband
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the subband
 */
static int subband(DiracContext *s, int16_t *data, int level,
                   subband_t orientation) {
    GetBitContext *gb = &s->gb;
    unsigned int length;
    unsigned int quant, qoffset, qfactor;
    int x, y;

    length = svq3_get_ue_golomb(gb);
    if (! length) {
        align_get_bits(gb);
    } else {
        quant = svq3_get_ue_golomb(gb);
        qfactor = coeff_quant_factor(quant);
        qoffset = coeff_quant_offset(s, quant) + 2;

        dirac_arith_init(&s->arith, gb, length);

        for (y = 0; y < s->codeblocksv[level]; y++)
            for (x = 0; x < s->codeblocksh[level]; x++)
                codeblock(s, data, level, orientation, x, y,
                          qoffset, qfactor);
        dirac_arith_flush(&s->arith);
    }

    return 0;
}

/**
 * Decode the DC subband
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the subband
 */
static int subband_dc(DiracContext *s, int16_t *data) {
    GetBitContext *gb = &s->gb;
    unsigned int length;
    unsigned int quant, qoffset, qfactor;
    int width, height;
    int x, y;

    width  = subband_width(s, 0);
    height = subband_height(s, 0);

    length = svq3_get_ue_golomb(gb);
    if (! length) {
        align_get_bits(gb);
    } else {
        quant = svq3_get_ue_golomb(gb);
        qfactor = coeff_quant_factor(quant);
        qoffset = coeff_quant_offset(s, quant) + 2;

        dirac_arith_init(&s->arith, gb, length);

        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                coeff_unpack(s, data, 0, subband_ll, y, x,
                         qoffset, qfactor);

        dirac_arith_flush(&s->arith);
    }

    if (s->refs == 0)
        intra_dc_prediction(s, data);

    return 0;
}


struct block_params {
    int xblen;
    int yblen;
    int xbsep;
    int ybsep;
};

static const struct block_params block_param_defaults[] = {
    {  8,  8,  4,  4 },
    { 12, 12,  8,  8 },
    { 16, 16, 12, 12 },
    { 24, 24, 16, 16 }
};

/**
 * Unpack the motion compensation parameters
 */
static int dirac_unpack_prediction_parameters(DiracContext *s) {
    GetBitContext *gb = &s->gb;

    /* Override block parameters.  */
    if (get_bits1(gb)) {
        unsigned int idx = svq3_get_ue_golomb(gb);

        if (idx > 3)
            return -1;

        if (idx == 0) {
            s->frame_decoding.luma_xblen = svq3_get_ue_golomb(gb);
            s->frame_decoding.luma_yblen = svq3_get_ue_golomb(gb);
            s->frame_decoding.luma_xbsep = svq3_get_ue_golomb(gb);
            s->frame_decoding.luma_ybsep = svq3_get_ue_golomb(gb);
        } else {
            s->frame_decoding.luma_xblen = block_param_defaults[idx].xblen;
            s->frame_decoding.luma_yblen = block_param_defaults[idx].yblen;
            s->frame_decoding.luma_xbsep = block_param_defaults[idx].xbsep;
            s->frame_decoding.luma_ybsep = block_param_defaults[idx].ybsep;
        }
    }

    /* Setup the blen and bsep parameters for the chroma
       component.  */
    s->frame_decoding.chroma_xblen = (s->frame_decoding.luma_xblen
                                      >> s->chroma_hshift);
    s->frame_decoding.chroma_yblen = (s->frame_decoding.luma_yblen
                                      >> s->chroma_vshift);
    s->frame_decoding.chroma_xbsep = (s->frame_decoding.luma_xbsep
                                      >> s->chroma_hshift);
    s->frame_decoding.chroma_ybsep = (s->frame_decoding.luma_ybsep
                                      >> s->chroma_vshift);

    /* Override motion vector precision.  */
    if (get_bits1(gb))
        s->frame_decoding.mv_precision = svq3_get_ue_golomb(gb);

    /* Read the global motion compensation parameters.  */
    s->globalmc_flag = get_bits1(gb);
    if (s->globalmc_flag) {
        int ref;
        for (ref = 0; ref < s->refs; ref++) {
            memset(&s->globalmc, 0, sizeof(s->globalmc));

            /* Pan/til parameters.  */
            if (get_bits1(gb)) {
                s->globalmc.b[0] = dirac_get_se_golomb(gb);
                s->globalmc.b[1] = dirac_get_se_golomb(gb);
            }

            /* Rotation/shear parameters.  */
            if (get_bits1(gb)) {
                s->globalmc.zrs_exp = svq3_get_ue_golomb(gb);
                s->globalmc.A[0][0] = dirac_get_se_golomb(gb);
                s->globalmc.A[0][1] = dirac_get_se_golomb(gb);
                s->globalmc.A[1][0] = dirac_get_se_golomb(gb);
                s->globalmc.A[1][1] = dirac_get_se_golomb(gb);
            }

            /* Perspective parameters.  */
            if (get_bits1(gb)) {
                s->globalmc.perspective_exp = svq3_get_ue_golomb(gb);
                s->globalmc.c[0]            = dirac_get_se_golomb(gb);
                s->globalmc.c[1]            = dirac_get_se_golomb(gb);
            }
        }
    }

    /* Picture prediction mode.  Not used yet in the specification.  */
    if (get_bits1(gb)) {
        /* Just ignore it, it should and will be zero.  */
        svq3_get_ue_golomb(gb);
    }

    /* XXX: For now set the weights here, I can't find this in the
       specification.  */
    s->frame_decoding.picture_weight_ref1 = 1;
    if (s->refs == 2) {
        s->frame_decoding.picture_weight_precision = 1;
        s->frame_decoding.picture_weight_ref2      = 1;
    } else {
        s->frame_decoding.picture_weight_precision = 0;
        s->frame_decoding.picture_weight_ref2      = 0;
    }

    /* Override reference picture weights.  */
    if (get_bits1(gb)) {
        s->frame_decoding.picture_weight_precision = svq3_get_ue_golomb(gb);
        s->frame_decoding.picture_weight_ref1 = dirac_get_se_golomb(gb);
        if (s->refs == 2)
            s->frame_decoding.picture_weight_ref2 = dirac_get_se_golomb(gb);
    }

    return 0;
}

static const int avgsplit[7] = { 0, 0, 1, 1, 1, 2, 2 };

static inline int split_prediction(DiracContext *s, int x, int y) {
    if (x == 0 && y == 0)
        return 0;
    else if (y == 0)
        return s->sbsplit[ y      * s->sbwidth + x - 1];
    else if (x == 0)
        return s->sbsplit[(y - 1) * s->sbwidth + x    ];

    return avgsplit[s->sbsplit[(y - 1) * s->sbwidth + x    ]
                  + s->sbsplit[ y      * s->sbwidth + x - 1]
                  + s->sbsplit[(y - 1) * s->sbwidth + x - 1]];
}

/**
 * Mode prediction
 *
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 * @param ref reference frame
 */
static inline int mode_prediction(DiracContext *s,
                                  int x, int y, int refmask, int refshift) {
    int cnt;

    if (x == 0 && y == 0)
        return 0;
    else if (y == 0)
        return ((s->blmotion[ y      * s->blwidth + x - 1].use_ref & refmask)
                >> refshift);
    else if (x == 0)
        return ((s->blmotion[(y - 1) * s->blwidth + x    ].use_ref & refmask)
                >> refshift);

    /* Return the majority.  */
    cnt = (s->blmotion[ y      * s->blwidth + x - 1].use_ref & refmask)
        + (s->blmotion[(y - 1) * s->blwidth + x    ].use_ref & refmask)
        + (s->blmotion[(y - 1) * s->blwidth + x - 1].use_ref & refmask);
    cnt >>= refshift;

    return cnt >> 1;
}

/**
 * Blockmode prediction
 *
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 */
static void blockmode_prediction(DiracContext *s, int x, int y) {
    int res = dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_PMODE_REF1);

    res ^= mode_prediction(s, x, y, DIRAC_REF_MASK_REF1, 0);
    s->blmotion[y * s->blwidth + x].use_ref |= res;
    if (s->refs == 2) {
        res = dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_PMODE_REF2);
        res ^= mode_prediction(s, x, y, DIRAC_REF_MASK_REF2, 1);
        s->blmotion[y * s->blwidth + x].use_ref |= res << 1;
    }
}

/**
 * Prediction for global motion compensation
 *
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 */
static void blockglob_prediction(DiracContext *s, int x, int y) {
    /* Global motion compensation is not used at all.  */
    if (!s->globalmc_flag)
        return;

    /* Global motion compensation is not used for this block.  */
    if (s->blmotion[y * s->blwidth + x].use_ref & 3) {
        int res = dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_GLOBAL_BLOCK);
        res ^= mode_prediction(s, x, y, DIRAC_REF_MASK_GLOBAL, 2);
        s->blmotion[y * s->blwidth + x].use_ref |= res << 2;
    }
}

/**
 * copy the block data to other MC blocks
 *
 * @param step superblock step size, so the number of MC blocks to copy
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 */
static void propagate_block_data(DiracContext *s, int step,
                                 int x, int y) {
    int i, j;

    /* XXX: For now this is rather inefficient, because everything is
       copied.  This function is called quite often.  */
    for (j = y; j < y + step; j++)
        for (i = x; i < x + step; i++)
            s->blmotion[j * s->blwidth + i] = s->blmotion[y * s->blwidth + x];
}

/**
 * Predict the motion vector
 *
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 * @param ref reference frame
 * @param dir direction horizontal=0, vertical=1
 */
static int motion_vector_prediction(DiracContext *s, int x, int y,
                                    int ref, int dir) {
    int cnt = 0;
    int left = 0, top = 0, lefttop = 0;
    const int refmask = ref + 1;
    const int mask = refmask | DIRAC_REF_MASK_GLOBAL;
    struct dirac_blockmotion *block = &s->blmotion[y * s->blwidth + x];

    if (x > 0) {
        /* Test if the block to the left has a motion vector for this
           reference frame.  */
        if ((block[-1].use_ref & mask) == refmask) {
            left = block[-1].vect[ref][dir];
            cnt++;
        }

        /* This is the only reference, return it.  */
        if (y == 0)
            return left;
    }

    if (y > 0) {
        /* Test if the block above the current one has a motion vector
           for this reference frame.  */
        if ((block[-s->blwidth].use_ref & mask) == refmask) {
            top = block[-s->blwidth].vect[ref][dir];
            cnt++;
        }

        /* This is the only reference, return it.  */
        if (x == 0)
            return top;
        else if (x > 0) {
            /* Test if the block above the current one has a motion vector
               for this reference frame.  */
            if ((block[-s->blwidth - 1].use_ref & mask) == refmask) {
                lefttop = block[-s->blwidth - 1].vect[ref][dir];
                cnt++;
            }
        }
    }

    /* No references for the prediction.  */
    if (cnt == 0)
        return 0;

    if (cnt == 1)
        return left + top + lefttop;

    /* Return the median of two motion vectors.  */
    if (cnt == 2)
        return (left + top + lefttop + 1) >> 1;

    /* Return the median of three motion vectors.  */
    return mid_pred(left, top, lefttop);
}

static int block_dc_prediction(DiracContext *s,
                               int x, int y, int comp) {
    int total = 0;
    int cnt = 0;

    if (x > 0) {
        if (!(s->blmotion[y * s->blwidth + x - 1].use_ref & 3)) {
            total += s->blmotion[y * s->blwidth + x - 1].dc[comp];
            cnt++;
        }
    }

    if (y > 0) {
        if (!(s->blmotion[(y - 1) * s->blwidth + x].use_ref & 3)) {
            total += s->blmotion[(y - 1) * s->blwidth + x].dc[comp];
            cnt++;
        }
    }

    if (x > 0 && y > 0) {
        if (!(s->blmotion[(y - 1) * s->blwidth + x - 1].use_ref & 3)) {
            total += s->blmotion[(y - 1) * s->blwidth + x - 1].dc[comp];
            cnt++;
        }
    }

    if (cnt == 0)
        return 1 << (s->sequence.video_depth - 1);

    /* Return the average of all DC values that were counted.  */
    return (total + (cnt >> 1)) / cnt;
}

static void unpack_block_dc(DiracContext *s, int x, int y, int comp) {
    int res;

    if (s->blmotion[y * s->blwidth + x].use_ref & 3) {
        s->blmotion[y * s->blwidth + x].dc[comp] = 0;
        return;
    }

    res = dirac_arith_read_int(&s->arith, &context_set_dc);
    res += block_dc_prediction(s, x, y, comp);

    s->blmotion[y * s->blwidth + x].dc[comp] = res;
}

/**
 * Unpack a single motion vector
 *
 * @param ref reference frame
 * @param dir direction horizontal=0, vertical=1
 */
static void dirac_unpack_motion_vector(DiracContext *s,
                                       int ref, int dir,
                                       int x, int y) {
    int res;
    const int refmask = (ref + 1) | DIRAC_REF_MASK_GLOBAL;

    /* First determine if for this block in the specific reference
       frame a motion vector is required.  */
    if ((s->blmotion[y * s->blwidth + x].use_ref & refmask) != ref + 1)
        return;

    res = dirac_arith_read_int(&s->arith, &context_set_mv);
    res += motion_vector_prediction(s, x, y, ref, dir);
    s->blmotion[y * s->blwidth + x].vect[ref][dir] = res;
}

/**
 * Unpack motion vectors
 *
 * @param ref reference frame
 * @param dir direction horizontal=0, vertical=1
 */
static void dirac_unpack_motion_vectors(DiracContext *s,
                                        int ref, int dir) {
    GetBitContext *gb = &s->gb;
    unsigned int length;
    int x, y;

    length = svq3_get_ue_golomb(gb);
    dirac_arith_init(&s->arith, gb, length);
    for (y = 0; y < s->sbheight; y++)
        for (x = 0; x < s->sbwidth; x++) {
                        int q, p;
            int blkcnt = 1 << s->sbsplit[y * s->sbwidth + x];
            int step = 4 >> s->sbsplit[y * s->sbwidth + x];

            for (q = 0; q < blkcnt; q++)
                for (p = 0; p < blkcnt; p++) {
                    dirac_unpack_motion_vector(s, ref, dir,
                                         4 * x + p * step,
                                         4 * y + q * step);
                    propagate_block_data(s, step,
                                         4 * x + p * step,
                                         4 * y + q * step);
                }
        }
    dirac_arith_flush(&s->arith);
}

/**
 * Unpack the motion compensation parameters
 */
static int dirac_unpack_prediction_data(DiracContext *s) {
    GetBitContext *gb = &s->gb;
    int i;
    unsigned int length;
    int comp;
    int x, y;

#define DIVRNDUP(a, b) ((a + b - 1) / b)

    s->sbwidth  = DIVRNDUP(s->sequence.luma_width,
                           (s->frame_decoding.luma_xbsep << 2));
    s->sbheight = DIVRNDUP(s->sequence.luma_height,
                           (s->frame_decoding.luma_ybsep << 2));
    s->blwidth  = s->sbwidth  << 2;
    s->blheight = s->sbheight << 2;

    s->sbsplit  = av_mallocz(s->sbwidth * s->sbheight * sizeof(int));
    if (!s->sbsplit) {
        av_log(s->avctx, AV_LOG_ERROR, "avcodec_check_dimensions() failed\n");
        return -1;
    }

    s->blmotion = av_mallocz(s->blwidth * s->blheight * sizeof(*s->blmotion));
    if (!s->blmotion) {
        av_freep(&s->sbsplit);
        av_log(s->avctx, AV_LOG_ERROR, "avcodec_check_dimensions() failed\n");
        return -1;
    }

    /* Superblock splitmodes.  */
    length = svq3_get_ue_golomb(gb);
    dirac_arith_init(&s->arith, gb, length);
    for (y = 0; y < s->sbheight; y++)
        for (x = 0; x < s->sbwidth; x++) {
            int res = dirac_arith_read_uint(&s->arith, &context_set_split);
            s->sbsplit[y * s->sbwidth + x] = (res +
                                              split_prediction(s, x, y));
            s->sbsplit[y * s->sbwidth + x] %= 3;
        }
    dirac_arith_flush(&s->arith);

    /* Prediction modes.  */
    length = svq3_get_ue_golomb(gb);
    dirac_arith_init(&s->arith, gb, length);
    for (y = 0; y < s->sbheight; y++)
        for (x = 0; x < s->sbwidth; x++) {
            int q, p;
            int blkcnt = 1 << s->sbsplit[y * s->sbwidth + x];
            int step   = 4 >> s->sbsplit[y * s->sbwidth + x];

            for (q = 0; q < blkcnt; q++)
                for (p = 0; p < blkcnt; p++) {
                    blockmode_prediction(s,
                                         4 * x + p * step,
                                         4 * y + q * step);
                    blockglob_prediction(s,
                                         4 * x + p * step,
                                         4 * y + q * step);
                    propagate_block_data(s, step,
                                         4 * x + p * step,
                                         4 * y + q * step);
                }
        }
    dirac_arith_flush(&s->arith);

    /* Unpack the motion vectors.  */
    for (i = 0; i < s->refs; i++) {
        dirac_unpack_motion_vectors(s, i, 0);
        dirac_unpack_motion_vectors(s, i, 1);
    }

    /* Unpack the DC values for all the three components (YUV).  */
    for (comp = 0; comp < 3; comp++) {
        /* Unpack the DC values.  */
        length = svq3_get_ue_golomb(gb);
        dirac_arith_init(&s->arith, gb, length);
        for (y = 0; y < s->sbheight; y++)
            for (x = 0; x < s->sbwidth; x++) {
                int q, p;
                int blkcnt = 1 << s->sbsplit[y * s->sbwidth + x];
                int step   = 4 >> s->sbsplit[y * s->sbwidth + x];

                for (q = 0; q < blkcnt; q++)
                    for (p = 0; p < blkcnt; p++) {
                        unpack_block_dc(s,
                                        4 * x + p * step,
                                        4 * y + q * step,
                                        comp);
                        propagate_block_data(s, step,
                                             4 * x + p * step,
                                             4 * y + q * step);
                    }
            }
        dirac_arith_flush(&s->arith);
    }

    return 0;
}

/**
 * Decode a single component
 *
 * @param coeffs coefficients for this component
 */
static void decode_component(DiracContext *s, int16_t *coeffs) {
    GetBitContext *gb = &s->gb;
    int level;
    subband_t orientation;

    /* Align for coefficient bitstream.  */
    align_get_bits(gb);

    /* Unpack LL, level 0.  */
    subband_dc(s, coeffs);

    /* Unpack all other subbands at all levels.  */
    for (level = 1; level <= s->frame_decoding.wavelet_depth; level++) {
        for (orientation = 1; orientation <= subband_hh; orientation++)
            subband(s, coeffs, level, orientation);
    }
 }

/**
 * IDWT
 *
 * @param coeffs coefficients to transform
 * @return returns 0 on succes, otherwise -1
 */
int dirac_idwt(DiracContext *s, int16_t *coeffs, int16_t *synth) {
    int level;
    int width, height;

    for (level = 1; level <= s->frame_decoding.wavelet_depth; level++) {
        width  = subband_width(s, level);
        height = subband_height(s, level);

        switch(s->wavelet_idx) {
        case 0:
            dprintf(s->avctx, "Deslauriers-Debuc (9,5) IDWT\n");
            dirac_subband_idwt_95(s->avctx, width, height, s->padded_width, coeffs, synth, level);
            break;
        case 1:
            dprintf(s->avctx, "LeGall (5,3) IDWT\n");
            dirac_subband_idwt_53(s->avctx, width, height, s->padded_width, coeffs, synth, level);
            break;
        default:
            av_log(s->avctx, AV_LOG_INFO, "unknown IDWT index: %d\n",
                   s->wavelet_idx);
        }
    }

    return 0;
}

/**
 * DWT
 *
 * @param coeffs coefficients to transform
 * @return returns 0 on succes, otherwise -1
 */
int dirac_dwt(DiracContext *s, int16_t *coeffs) {
    int level;
    int width, height;

    /* XXX: make depth configurable.  */
    for (level = s->frame_decoding.wavelet_depth; level >= 1; level--) {
        width  = subband_width(s, level);
        height = subband_height(s, level);
        dirac_subband_dwt_53(s->avctx, width, height, s->padded_width, coeffs, level);
    }

    return 0;
}


/**
 * Search a frame in the buffer of reference frames
 *
 * @param  frameno  frame number in display order
 * @return index of the reference frame in the reference frame buffer
 */
static int reference_frame_idx(DiracContext *s, int frameno) {
    int i;

    for (i = 0; i < s->refcnt; i++) {
        AVFrame *f = &s->refframes[i].frame;
        if (f->display_picture_number == frameno)
            return i;
    }

    return -1;
}

/**
 * Interpolate a frame
 *
 * @param refframe frame to grab the upconverted pixel from
 * @param width    frame width
 * @param height   frame height
 * @param pixels   buffer to write the interpolated pixels to
 * @param comp     component
 */
static inline void interpolate_frame_halfpel(AVFrame *refframe,
                                             int width, int height,
                                             uint8_t *pixels, int comp,
                                             int xpad, int ypad) {
    uint8_t *lineout;
    uint8_t *refdata;
    uint8_t *linein;
    int outwidth = width * 2 + xpad * 4;
    int doutwidth = 2 * outwidth;
    int x, y;
    const int t[5] = { 167, -56, 25, -11, 3 };
    uint8_t *pixelsdata = pixels + ypad * doutwidth + 2 * xpad;

START_TIMER

    refdata    = refframe->data[comp];

    linein  = refdata;
    lineout = pixelsdata;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            lineout[x * 2] = linein[x];
        }

        linein  += refframe->linesize[comp];
        lineout += doutwidth;
    }

    /* Copy top even lines.  */
    linein  = pixels + ypad * doutwidth;
    lineout = pixels;
    for (y = 0; y < ypad * 2; y += 2) {
        memcpy(lineout, linein, outwidth);
        lineout += doutwidth;
    }

    /* Copy bottom even lines.  */
    linein  = pixels + (ypad + height - 1) * doutwidth;
    lineout = linein + outwidth;
    for (y = 0; y < ypad * 2; y += 2) {
        memcpy(lineout, linein, outwidth);
        lineout += doutwidth;
    }

    /* Interpolation (vectically).  */
    linein  = pixelsdata;
    lineout = pixelsdata + outwidth;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width * 2; x += 2) {
            int val = 128;
            uint8_t *li1 = linein;
            uint8_t *li2 = linein + doutwidth;

            val += t[0] * (li1[x] + li2[x]);
            li1 -= doutwidth;
            li2 += doutwidth;

            val += t[1] * (li1[x] + li2[x]);
            li1 -= doutwidth;
            li2 += doutwidth;

            val += t[2] * (li1[x] + li2[x]);
            li1 -= doutwidth;
            li2 += doutwidth;

            val += t[3] * (li1[x] + li2[x]);
            li1 -= doutwidth;
            li2 += doutwidth;

            val += t[4] * (li1[x] + li2[x]);

            val >>= 8;

            lineout[x] = av_clip_uint8(val);
        }

        linein += outwidth * 2;

        /* Skip one line, we are interpolating to odd lines.  */
        lineout    += outwidth * 2;
    }

    /* Add padding on the left and right sides of the frame.  */
    lineout = pixels + 2 * ypad * outwidth;
    for (y = 0; y < height * 2; y++) {
        memset(lineout, lineout[2 * xpad], 2 * xpad);
        memset(&lineout[2 * width + xpad * 2],
               lineout[2 * width + xpad * 2 - 2], 2 * xpad);
        lineout += outwidth;
    }

    /* Interpolation (horizontally).  */
    lineout = pixelsdata + 1;
    linein  = pixelsdata;
    for (y = 0; y < height * 2; y++) {
        for (x = 0; x < width * 2; x += 2) {
            uint8_t *li1 = &linein[x];
            uint8_t *li2 = &linein[x + 2];
            int val = 128;

            val += t[0] * (*li1 + *li2);
            li1 -= 2;
            li2 += 2;
            val += t[1] * (*li1 + *li2);
            li1 -= 2;
            li2 += 2;
            val += t[2] * (*li1 + *li2);
            li1 -= 2;
            li2 += 2;
            val += t[3] * (*li1 + *li2);
            li1 -= 2;
            li2 += 2;
            val += t[4] * (*li1 + *li2);

            val >>= 8;
            lineout[x] = av_clip_uint8(val);
        }
        lineout += outwidth;
        linein  += outwidth;
    }

    /* Add padding to right side of the frame.  */
    lineout = pixels + 2 * ypad * outwidth;
    for (y = 0; y < height * 2; y++) {
        memset(&lineout[2 * width + xpad * 2],
               lineout[2 * width + xpad * 2 - 1], 2 * xpad);
        lineout += outwidth;
    }

    /* Copy top lines.  */
    linein  = pixels + ypad * doutwidth;
    lineout = pixels;
    for (y = 0; y < ypad * 2; y++) {
        memcpy(lineout, linein, outwidth);
        lineout += outwidth;
    }

    /* Copy bottom lines.  */
    linein  = pixels + (ypad + height - 1) * doutwidth;
    lineout = linein + outwidth;
    for (y = 0; y < ypad * 2; y++) {
        memcpy(lineout, linein, outwidth);
        lineout += outwidth;
    }

STOP_TIMER("halfpel");
}

/**
 * Calculate WH or WV of the spatial weighting matrix
 *
 * @param i       block position
 * @param x       current pixel
 * @param bsep    block spacing
 * @param blen    block length
 * @param offset  xoffset/yoffset
 * @param blocks  number of blocks
 */
static inline int spatial_wt(int i, int x, int bsep, int blen,
                             int offset, int blocks) {
    int pos = x - (i * bsep - offset);
    int max;

    max = 2 * (blen - bsep);
    if (i == 0 && pos < (blen >> 1))
        return max;
    else if (i == blocks - 1 && pos >= (blen >> 1))
        return max;
    else
        return av_clip(blen - FFABS(2*pos - (blen - 1)), 0, max);
}

/**
 * Motion Compensation with two reference frames
 *
 * @param coeffs     coefficients to add the DC to
 * @param i          horizontal position of the MC block
 * @param j          vertical position of the MC block
 * @param xstart     top left coordinate of the MC block
 * @param ystop      top left coordinate of the MC block
 * @param xstop      bottom right coordinate of the MC block
 * @param ystop      bottom right coordinate of the MC block
 * @param ref1       first reference frame
 * @param ref2       second reference frame
 * @param currblock  MC block to use
 * @param comp       component
 * @param border     0 if this is not a border MC block, otherwise 1
 */
static void motion_comp_block2refs(DiracContext *s, int16_t *coeffs,
                                   int i, int j, int xstart, int xstop,
                                   int ystart, int ystop, uint8_t *ref1,
                                   uint8_t *ref2,
                                   struct dirac_blockmotion *currblock,
                                   int comp, int border) {
    int x, y;
    int xs, ys;
    int16_t *line;
    uint8_t *refline1;
    uint8_t *refline2;
    int vect1[2];
    int vect2[2];
    int refxstart1, refystart1;
    int refxstart2, refystart2;
    uint16_t *spatialwt;
    /* Subhalfpixel in qpel/eighthpel interpolated frame.  */
    int rx1, ry1, rx2, ry2;
    const uint8_t *w1 = NULL;
    const uint8_t *w2 = NULL;
    int xfix1 = 0, xfix2 = 0;


START_TIMER

    vect1[0] = currblock->vect[0][0];
    vect1[1] = currblock->vect[0][1];
    vect2[0] = currblock->vect[1][0];
    vect2[1] = currblock->vect[1][1];

    xs = FFMAX(xstart, 0);
    ys = FFMAX(ystart, 0);

    if (comp != 0) {
        vect1[0] >>= s->chroma_hshift;
        vect2[0] >>= s->chroma_hshift;
        vect1[1] >>= s->chroma_vshift;
        vect2[1] >>= s->chroma_vshift;
    }

    switch(s->frame_decoding.mv_precision) {
    case 0:
        refxstart1 = (xs + vect1[0]) << 1;
        refystart1 = (ys + vect1[1]) << 1;
        refxstart2 = (xs + vect2[0]) << 1;
        refystart2 = (ys + vect2[1]) << 1;
        break;
    case 1:
        refxstart1   = (xs << 1) + vect1[0];
        refystart1   = (ys << 1) + vect1[1];
        refxstart2   = (xs << 1) + vect2[0];
        refystart2   = (ys << 1) + vect2[1];
        break;
    case 2:
        refxstart1   = ((xs << 2) + vect1[0]) >> 1;
        refystart1   = ((ys << 2) + vect1[1]) >> 1;
        refxstart2   = ((xs << 2) + vect2[0]) >> 1;
        refystart2   = ((ys << 2) + vect2[1]) >> 1;
        rx1 = vect1[0] & 1;
        ry1 = vect1[1] & 1;
        rx2 = vect2[0] & 1;
        ry2 = vect2[1] & 1;
        w1 = qpel_weights[(rx1 << 1) | ry1];
        w2 = qpel_weights[(rx2 << 1) | ry2];
        break;
    case 3:
        refxstart1   = ((xs << 3) + vect1[0]) >> 2;
        refystart1   = ((ys << 3) + vect1[1]) >> 2;
        refxstart2   = ((xs << 3) + vect2[0]) >> 2;
        refystart2   = ((ys << 3) + vect2[1]) >> 2;
        rx1 = vect1[0] & 3;
        ry1 = vect1[1] & 3;
        rx2 = vect2[0] & 3;
        ry2 = vect2[0] & 3;
        w1 = eighthpel_weights[(rx1 << 2) | ry1];
        w2 = eighthpel_weights[(rx2 << 2) | ry2];
        break;
    default:
        /* XXX */
        return;
    }

    spatialwt = &s->spatialwt[s->xblen * (ys - ystart)];

    /* Make sure the vector doesn't point to a block outside the
       padded frame.  */
    refystart1 = av_clip(refystart1, -s->yblen, s->height * 2 - 1);
    refystart2 = av_clip(refystart2, -s->yblen, s->height * 2 - 1);
    if (refxstart1 < -s->xblen)
        xfix1 = -s->xblen - refxstart1;
    else if (refxstart1 >= (s->width - 1) * 2)
        xfix1 = (s->width - 1) * 2 - refxstart1;
    if (refxstart2 < -s->xblen * 2)
        xfix2 = -s->xblen * 2 - refxstart2;
    else if (refxstart2 >= (s->width - 1) * 2)
        xfix2 = (s->width - 1) * 2 - refxstart2;

    line = &coeffs[s->width * ys];
    refline1 = &ref1[refystart1 * s->refwidth];
    refline2 = &ref2[refystart2 * s->refwidth];
    for (y = ys; y < ystop; y++) {
        int bx = xs - xstart;
        for (x = xs; x < xstop; x++) {
            int val1;
            int val2;
            int val;

            if (s->frame_decoding.mv_precision == 0) {
                /* No interpolation.  */
                val1 = refline1[(x + vect1[0]) << 1];
                val2 = refline2[(x + vect2[0]) << 1];
            } else if (s->frame_decoding.mv_precision == 1) {
                /* Halfpel interpolation.  */
                val1 = refline1[(x << 1) + vect1[0]];
                val2 = refline2[(x << 1) + vect2[0]];
            } else {
                /* Position in halfpel interpolated frame.  */
                int hx1, hx2;

                if (s->frame_decoding.mv_precision == 2) {
                    /* Do qpel interpolation.  */
                    hx1 = ((x << 2) + vect1[0]) >> 1;
                    hx2 = ((x << 2) + vect2[0]) >> 1;
                    val1 = 2;
                    val2 = 2;
                } else {
                    /* Do eighthpel interpolation.  */
                    hx1 = ((x << 3) + vect1[0]) >> 2;
                    hx2 = ((x << 3) + vect2[0]) >> 2;
                    val1 = 4;
                    val2 = 4;
                }

                /* Fix the x position on the halfpel interpolated
                   frame so it points to a MC block within the padded
                   region.  */
                hx1 += xfix1;
                hx2 += xfix2;

                val1 += w1[0] * refline1[hx1                  ];
                val1 += w1[1] * refline1[hx1               + 1];
                val1 += w1[2] * refline1[hx1 + s->refwidth    ];
                val1 += w1[3] * refline1[hx1 + s->refwidth + 1];
                val1 >>= s->frame_decoding.mv_precision;

                val2 += w2[0] * refline2[hx2                  ];
                val2 += w2[1] * refline2[hx2               + 1];
                val2 += w2[2] * refline2[hx2 + s->refwidth    ];
                val2 += w2[3] * refline2[hx2 + s->refwidth + 1];
                val2 >>= s->frame_decoding.mv_precision;
            }

            val1 *= s->frame_decoding.picture_weight_ref1;
            val2 *= s->frame_decoding.picture_weight_ref2;
            val = val1 + val2;
            if (border) {
                val *= spatialwt[bx];
            } else {
                val = (val
                       * spatial_wt(i, x, s->xbsep, s->xblen,
                                    s->xoffset, s->current_blwidth)
                       * spatial_wt(j, y, s->ybsep, s->yblen,
                                    s->yoffset, s->current_blheight));
            }

            line[x] += val;
            bx++;
        }
        refline1 += s->refwidth << 1;
        refline2 += s->refwidth << 1;
        line += s->width;
        spatialwt += s->xblen;
    }

STOP_TIMER("two_refframes");
}

/**
 * Motion Compensation with one reference frame
 *
 * @param coeffs     coefficients to add the DC to
 * @param i          horizontal position of the MC block
 * @param j          vertical position of the MC block
 * @param xstart     top left coordinate of the MC block
 * @param ystop      top left coordinate of the MC block
 * @param xstop      bottom right coordinate of the MC block
 * @param ystop      bottom right coordinate of the MC block
 * @param refframe   reference frame
 * @param ref        0=first refframe 1=second refframe
 * @param currblock  MC block to use
 * @param comp       component
 * @param border     0 if this is not a border MC block, otherwise 1
 */
static void motion_comp_block1ref(DiracContext *s, int16_t *coeffs,
                                  int i, int j, int xstart, int xstop,
                                  int ystart, int ystop, uint8_t *refframe,
                                  int ref,
                                  struct dirac_blockmotion *currblock,
                                  int comp, int border) {
    int x, y;
    int xs, ys;
    int16_t *line;
    uint8_t  *refline;
    int vect[2];
    int refxstart, refystart;
    uint16_t *spatialwt;
    /* Subhalfpixel in qpel/eighthpel interpolated frame.  */
    int rx, ry;
    const uint8_t *w = NULL;
    int xfix = 0;

START_TIMER

    vect[0] = currblock->vect[ref][0];
    vect[1] = currblock->vect[ref][1];

    xs = FFMAX(xstart, 0);
    ys = FFMAX(ystart, 0);

    if (comp != 0) {
        vect[0] >>= s->chroma_hshift;
        vect[1] >>= s->chroma_vshift;
    }

    switch(s->frame_decoding.mv_precision) {
    case 0:
        refxstart = (xs + vect[0]) << 1;
        refystart = (ys + vect[1]) << 1;
        break;
    case 1:
        refxstart   = (xs << 1) + vect[0];
        refystart   = (ys << 1) + vect[1];
        break;
    case 2:
        refxstart   = ((xs << 2) + vect[0]) >> 1;
        refystart   = ((ys << 2) + vect[1]) >> 1;
        rx = vect[0] & 1;
        ry = vect[1] & 1;
        w = qpel_weights[(rx << 1) | ry];
        break;
    case 3:
        refxstart   = ((xs << 3) + vect[0]) >> 2;
        refystart   = ((ys << 3) + vect[1]) >> 2;
        rx = vect[0] & 3;
        ry = vect[1] & 3;
        w = eighthpel_weights[(rx << 2) | ry];
        break;
    default:
        /* XXX */
        return;
    }

    /* Make sure the vector doesn't point to a block outside the
       padded frame.  */
    refystart = av_clip(refystart, -s->yblen * 2, s->height * 2 - 1);
    if (refxstart < -s->xblen * 2)
        xfix = -s->xblen - refxstart;
    else if (refxstart >= (s->width - 1) * 2)
        xfix = (s->width - 1) * 2 - refxstart;

    spatialwt = &s->spatialwt[s->xblen * (ys - ystart)];

    line = &coeffs[s->width * ys];
    refline = &refframe[refystart * s->refwidth];
    for (y = ys; y < ystop; y++) {
        int bx = xs - xstart;
        for (x = xs; x < xstop; x++) {
            int val;

            if (s->frame_decoding.mv_precision == 0) {
                /* No interpolation.  */
                val = refline[(x + vect[0]) << 1];
            } else if (s->frame_decoding.mv_precision == 1) {
                /* Halfpel interpolation.  */
                val = refline[(x << 1) + vect[0]];
            } else {
                /* Position in halfpel interpolated frame.  */
                int hx;

                if (s->frame_decoding.mv_precision == 2) {
                    /* Do qpel interpolation.  */
                    hx = ((x << 2) + vect[0]) >> 1;
                    val = 2;
                } else {
                    /* Do eighthpel interpolation.  */
                    hx = ((x << 3) + vect[0]) >> 2;
                    val = 4;
                }

                /* Fix the x position on the halfpel interpolated
                   frame so it points to a MC block within the padded
                   region.  */
                hx += xfix;

                val += w[0] * refline[hx                  ];
                val += w[1] * refline[hx               + 1];
                val += w[2] * refline[hx + s->refwidth    ];
                val += w[3] * refline[hx + s->refwidth + 1];
                val >>= s->frame_decoding.mv_precision;
            }

            val *= s->frame_decoding.picture_weight_ref1
                 + s->frame_decoding.picture_weight_ref2;

            if (border) {
                val *= spatialwt[bx];
            } else {
                val = (val
                       * spatial_wt(i, x, s->xbsep, s->xblen,
                                    s->xoffset, s->current_blwidth)
                       * spatial_wt(j, y, s->ybsep, s->yblen,
                                    s->yoffset, s->current_blheight));
            }

            line[x] += val;
            bx++;
        }
        line += s->width;
        refline += s->refwidth << 1;
        spatialwt += s->xblen;
    }

STOP_TIMER("single_refframe");
}

/**
 * Motion Compensation DC values (no reference frame)
 *
 * @param coeffs coefficients to add the DC to
 * @param i      horizontal position of the MC block
 * @param j      vertical position of the MC block
 * @param xstart top left coordinate of the MC block
 * @param ystop  top left coordinate of the MC block
 * @param xstop  bottom right coordinate of the MC block
 * @param ystop  bottom right coordinate of the MC block
 * @param dcval  DC value to apply to all coefficients in the MC block
 * @param border 0 if this is not a border MC block, otherwise 1
 */
static inline void motion_comp_dc_block(DiracContext *s,
                                        int16_t *coeffs, int i, int j,
                                        int xstart, int xstop, int ystart,
                                        int ystop, int dcval, int border) {
    int x, y;
    int xs, ys;
    int16_t *line;
    uint16_t *spatialwt;

    ys = FFMAX(ystart, 0);
    xs = FFMAX(xstart, 0);

    dcval <<= s->frame_decoding.picture_weight_precision;

    spatialwt = &s->spatialwt[s->xblen * (ys - ystart)];
    line = &coeffs[s->width * ys];
    for (y = ys; y < ystop; y++) {
        int bx = xs - xstart;
        for (x = xs; x < xstop; x++) {
            int val;

            if (border) {
                val = dcval * spatialwt[bx];
            } else {
                val = dcval
                    * spatial_wt(i, x, s->xbsep, s->xblen,
                                 s->xoffset, s->current_blwidth)
                    * spatial_wt(j, y, s->ybsep, s->yblen,
                                 s->yoffset, s->current_blheight);
            }

            line[x] += val;
            bx++;
        }
        line += s->width;
        spatialwt += s->xblen;
    }
}

/**
 * Motion compensation
 *
 * @param coeffs coefficients to which the MC results will be added
 * @param comp component
 * @return returns 0 on succes, otherwise -1
 */
static int dirac_motion_compensation(DiracContext *s, int16_t *coeffs,
                                     int comp) {
    int i, j;
    int x, y;
    int refidx[2] = { 0 };
    int cacheframe[2] = {1, 1};
    AVFrame *ref[2] = { 0 };
    struct dirac_blockmotion *currblock;
    int16_t *mcpic;
    int16_t *mcline;
    int16_t *coeffline;
    int xstart, ystart;
    int xstop, ystop;
    int hbits, vbits;
    int total_wt_bits;

    if (comp == 0) {
        s->width  = s->sequence.luma_width;
        s->height = s->sequence.luma_height;
        s->xblen  = s->frame_decoding.luma_xblen;
        s->yblen  = s->frame_decoding.luma_yblen;
        s->xbsep  = s->frame_decoding.luma_xbsep;
        s->ybsep  = s->frame_decoding.luma_ybsep;
    } else {
        s->width  = s->sequence.chroma_width;
        s->height = s->sequence.chroma_height;
        s->xblen  = s->frame_decoding.chroma_xblen;
        s->yblen  = s->frame_decoding.chroma_yblen;
        s->xbsep  = s->frame_decoding.chroma_xbsep;
        s->ybsep  = s->frame_decoding.chroma_ybsep;
    }

    s->xoffset = (s->xblen - s->xbsep) / 2;
    s->yoffset = (s->yblen - s->ybsep) / 2;
    hbits      = av_log2(s->xoffset) + 2;
    vbits      = av_log2(s->yoffset) + 2;

    total_wt_bits = hbits + vbits
                       + s->frame_decoding.picture_weight_precision;

    s->refwidth = (s->width + 2 * s->xblen) << 1;
    s->refheight = (s->height + 2 * s->yblen) << 1;

    s->spatialwt = av_malloc(s->xblen * s->yblen * sizeof(int16_t));
    if (!s->spatialwt) {
        av_log(s->avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }

    /* Set up the spatial weighting matrix.  */
    for (x = 0; x < s->xblen; x++) {
        for (y = 0; y < s->yblen; y++) {
            int wh, wv;
            const int xmax = 2 * (s->xblen - s->xbsep);
            const int ymax = 2 * (s->yblen - s->ybsep);

            wh = av_clip(s->xblen - FFABS(2*x - (s->xblen - 1)), 0, xmax);
            wv = av_clip(s->yblen - FFABS(2*y - (s->yblen - 1)), 0, ymax);
            s->spatialwt[x + y * s->xblen] = wh * wv;
        }
    }

    if (avcodec_check_dimensions(s->avctx, s->refwidth, s->refheight)) {
        av_log(s->avctx, AV_LOG_ERROR, "avcodec_check_dimensions() failed\n");
        return -1;
    }

    for (i = 0; i < s->refs; i++) {
        refidx[i] = reference_frame_idx(s, s->ref[i]);
        ref[i] = &s->refframes[refidx[i]].frame;

        if (s->refframes[refidx[i]].halfpel[comp] == NULL) {
            s->refdata[i] = av_malloc(s->refwidth * s->refheight);
            if (!s->refdata[i]) {
                if (i == 1)
                    av_free(s->refdata[0]);
                av_log(s->avctx, AV_LOG_ERROR, "av_malloc() failed\n");
                return -1;
            }
            interpolate_frame_halfpel(ref[i], s->width, s->height,
                                      s->refdata[i], comp, s->xblen, s->yblen);
        } else {
            s->refdata[i] = s->refframes[refidx[i]].halfpel[comp];
            cacheframe[i] = 2;
        }
    }

    if (avcodec_check_dimensions(s->avctx, s->width, s->height)) {
        for (i = 0; i < s->refs; i++)
            av_free(s->refdata[i]);

        av_log(s->avctx, AV_LOG_ERROR, "avcodec_check_dimensions() failed\n");
        return -1;
    }

    mcpic = av_malloc(s->width * s->height * sizeof(int16_t));
    if (!mcpic) {
        for (i = 0; i < s->refs; i++)
            av_free(s->refdata[i]);

        av_log(s->avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }
    memset(mcpic, 0, s->width * s->height * sizeof(int16_t));

    {
        START_TIMER;

        s->current_blwidth  = (s->width  - s->xoffset) / s->xbsep + 1;
        s->current_blheight = (s->height - s->yoffset) / s->ybsep + 1;

        currblock = s->blmotion;
        for (j = 0; j < s->current_blheight; j++) {
            for (i = 0; i < s->current_blwidth; i++) {
                struct dirac_blockmotion *block = &currblock[i];
                int border;
                int padding;

                /* XXX: These calculations do not match those in the
                   Dirac specification, but are correct.  */
                xstart  = i * s->xbsep - s->xoffset;
                ystart  = j * s->ybsep - s->yoffset;
                xstop   = FFMIN(xstart + s->xblen, s->width);
                ystop   = FFMIN(ystart + s->yblen, s->height);

                border = (i > 0 && j > 0
                          && i < s->current_blwidth - 1
                          && j < s->current_blheight - 1);

                padding = 2 * ((s->xblen * 2 + s->width) * 2 * s->yblen
                               + s->xblen);

                /* Intra */
                if ((block->use_ref & 3) == 0)
                    motion_comp_dc_block(s, mcpic, i, j,
                                         xstart, xstop, ystart, ystop,
                                         block->dc[comp], border);
                /* Reference frame 1 only.  */
                else if ((block->use_ref & 3) == DIRAC_REF_MASK_REF1)
                    motion_comp_block1ref(s, mcpic, i, j,
                                          xstart, xstop, ystart,
                                          ystop,s->refdata[0] + padding, 0, block, comp,
                                          border);
                /* Reference frame 2 only.  */
                else if ((block->use_ref & 3) == DIRAC_REF_MASK_REF2)
                    motion_comp_block1ref(s, mcpic, i, j,
                                          xstart, xstop, ystart, ystop,
                                          s->refdata[1] + padding, 1, block, comp,
                                          border);
                /* Both reference frames.  */
                else
                    motion_comp_block2refs(s, mcpic, i, j,
                                           xstart, xstop, ystart, ystop,
                                           s->refdata[0] + padding, s->refdata[1] + padding,
                                           block, comp, border);
            }
            currblock += s->blwidth;
        }

        coeffline = coeffs;
        mcline    = mcpic;
        /* XXX: It might be more efficient (for cache) to merge this
           with the loop above somehow.  */
        for (y = 0; y < s->height; y++) {
            for (x = 0; x < s->width; x++) {
                int16_t coeff = mcline[x] + (1 << (total_wt_bits - 1));
                coeffline[x] += coeff >> total_wt_bits;
            }
            coeffline += s->padded_width;
            mcline    += s->width;
        }

        av_free(mcpic);

        STOP_TIMER("motioncomp");
    }

    av_freep(&s->spatialwt);

    for (i = 0; i < s->retirecnt; i++) {
        if (cacheframe[0] == 1 && i == refidx[0])
            cacheframe[0] = 0;
        if (cacheframe[1] == 1 && i == refidx[1])
            cacheframe[1] = 0;
    }

    for (i = 0; i < s->refs; i++) {
        if (cacheframe[i])
            s->refframes[refidx[i]].halfpel[comp] = s->refdata[i];
        else
            av_freep(&s->refdata[i]);
    }

    return 0;
}

/**
 * Decode a frame.
 *
 * @return 0 when successful, otherwise -1 is returned
 */
static int dirac_decode_frame(DiracContext *s) {
    AVCodecContext *avctx = s->avctx;
    int16_t *coeffs;
    int16_t *line;
    int comp;
    int x,y;
    int16_t *synth;

START_TIMER

    if (avcodec_check_dimensions(s->avctx, s->padded_luma_width,
                                 s->padded_luma_height)) {
        av_log(s->avctx, AV_LOG_ERROR, "avcodec_check_dimensions() failed\n");
        return -1;
    }

    coeffs = av_malloc(s->padded_luma_width
                       * s->padded_luma_height
                       * sizeof(int16_t));
    if (! coeffs) {
        av_log(s->avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }

    /* Allocate memory for the IDWT to work in.  */
    if (avcodec_check_dimensions(avctx, s->padded_luma_width,
                                 s->padded_luma_height)) {
        av_log(avctx, AV_LOG_ERROR, "avcodec_check_dimensions() failed\n");
        return -1;
    }
    synth = av_malloc(s->padded_luma_width * s->padded_luma_height
                      * sizeof(int16_t));
    if (!synth) {
        av_log(avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }

    for (comp = 0; comp < 3; comp++) {
        uint8_t *frame = s->picture.data[comp];
        int width, height;

        if (comp == 0) {
            width            = s->sequence.luma_width;
            height           = s->sequence.luma_height;
            s->padded_width  = s->padded_luma_width;
            s->padded_height = s->padded_luma_height;
        } else {
            width            = s->sequence.chroma_width;
            height           = s->sequence.chroma_height;
            s->padded_width  = s->padded_chroma_width;
            s->padded_height = s->padded_chroma_height;
        }

        memset(coeffs, 0,
               s->padded_width * s->padded_height * sizeof(int16_t));

        if (!s->zero_res)
            decode_component(s, coeffs);

        dirac_idwt(s, coeffs, synth);

        if (s->refs) {
            if (dirac_motion_compensation(s, coeffs, comp)) {
                av_freep(&s->sbsplit);
                av_freep(&s->blmotion);

                return -1;
            }
        }

        /* Copy the decoded coefficients into the frame.  */
        line = coeffs;
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++)
                frame[x]= av_clip_uint8(line[x]);

            line  += s->padded_width;
            frame += s->picture.linesize[comp];
        }
    }

    if (s->refs) {
        av_freep(&s->sbsplit);
        av_freep(&s->blmotion);
    }
    av_free(coeffs);
    av_free(synth);

STOP_TIMER("dirac_frame_decode");

    return 0;
}

/**
 * Parse a frame and setup DiracContext to decode it
 *
 * @return 0 when successful, otherwise -1 is returned
 */
static int parse_frame(DiracContext *s) {
    unsigned int retire;
    int i;
    GetBitContext *gb = &s->gb;

    /* Setup decoding parameter defaults for this frame.  */
    s->frame_decoding = s->decoding;

    s->picture.pict_type = FF_I_TYPE;
    s->picture.key_frame = 1;

    s->picnum = get_bits_long(gb, 32);

    for (i = 0; i < s->refs; i++)
        s->ref[i] = dirac_get_se_golomb(gb) + s->picnum;

    /* Retire the reference frames that are not used anymore.  */
    retire = svq3_get_ue_golomb(gb);
    s->retirecnt = retire;
    for (i = 0; i < retire; i++) {
        uint32_t retire_num;

        retire_num = dirac_get_se_golomb(gb) + s->picnum;
        s->retireframe[i] = retire_num;
    }

    if (s->refs) {
        align_get_bits(gb);
        if (dirac_unpack_prediction_parameters(s))
            return -1;
        align_get_bits(gb);
        if (dirac_unpack_prediction_data(s))
            return -1;
    }

    align_get_bits(gb);

    /* Wavelet transform data.  */
    if (s->refs == 0)
        s->zero_res = 0;
    else
        s->zero_res = get_bits1(gb);

    if (!s->zero_res) {
        /* Override wavelet transform parameters.  */
        if (get_bits1(gb)) {
            dprintf(s->avctx, "Non default filter\n");
            s->wavelet_idx = svq3_get_ue_golomb(gb);
        } else {
            dprintf(s->avctx, "Default filter\n");
            if (s->refs == 0)
                s->wavelet_idx = s->frame_decoding.wavelet_idx_intra;
            else
                s->wavelet_idx = s->frame_decoding.wavelet_idx_inter;
        }

        if (s->wavelet_idx > 7)
            return -1;

        /* Overrid wavelet depth.  */
        if (get_bits1(gb)) {
            dprintf(s->avctx, "Non default depth\n");
            s->frame_decoding.wavelet_depth = svq3_get_ue_golomb(gb);
        }
        dprintf(s->avctx, "Depth: %d\n", s->frame_decoding.wavelet_depth);

        /* Spatial partitioning.  */
        if (get_bits1(gb)) {
            unsigned int idx;

            dprintf(s->avctx, "Spatial partitioning\n");

            /* Override the default partitioning.  */
            if (get_bits1(gb)) {
                for (i = 0; i <= s->frame_decoding.wavelet_depth; i++) {
                    s->codeblocksh[i] = svq3_get_ue_golomb(gb);
                    s->codeblocksv[i] = svq3_get_ue_golomb(gb);
                }

                dprintf(s->avctx, "Non-default partitioning\n");

            } else {
                /* Set defaults for the codeblocks.  */
                for (i = 0; i <= s->frame_decoding.wavelet_depth; i++) {
                    if (s->refs == 0) {
                        s->codeblocksh[i] = i <= 2 ? 1 : 4;
                        s->codeblocksv[i] = i <= 2 ? 1 : 3;
                    } else {
                        if (i <= 1) {
                            s->codeblocksh[i] = 1;
                            s->codeblocksv[i] = 1;
                        } else if (i == 2) {
                            s->codeblocksh[i] = 8;
                            s->codeblocksv[i] = 6;
                        } else {
                            s->codeblocksh[i] = 12;
                            s->codeblocksv[i] = 8;
                        }
                    }
                }
            }

            idx = svq3_get_ue_golomb(gb);
            dprintf(s->avctx, "Codeblock mode idx: %d\n", idx);
            /* XXX: Here 0, so single quant.  */
        }
    }

#define CALC_PADDING(size, depth) \
         (((size + (1 << depth) - 1) >> depth) << depth)

    /* Round up to a multiple of 2^depth.  */
    s->padded_luma_width    = CALC_PADDING(s->sequence.luma_width,
                                           s->frame_decoding.wavelet_depth);
    s->padded_luma_height   = CALC_PADDING(s->sequence.luma_height,
                                           s->frame_decoding.wavelet_depth);
    s->padded_chroma_width  = CALC_PADDING(s->sequence.chroma_width,
                                           s->frame_decoding.wavelet_depth);
    s->padded_chroma_height = CALC_PADDING(s->sequence.chroma_height,
                                           s->frame_decoding.wavelet_depth);

    return 0;
}


static int decode_frame(AVCodecContext *avctx, void *data, int *data_size,
                        uint8_t *buf, int buf_size){
    DiracContext *s = avctx->priv_data;
    AVFrame *picture = data;
    int i;
    int parse_code;

    if (buf_size == 0) {
        int idx = reference_frame_idx(s, avctx->frame_number);
        if (idx == -1) {
            /* The frame was not found.  */
            *data_size = 0;
        } else {
            *data_size = sizeof(AVFrame);
            *picture = s->refframes[idx].frame;
        }
        return 0;
    }

    parse_code = buf[4];

    dprintf(avctx, "Decoding frame: size=%d head=%c%c%c%c parse=%02x\n",
            buf_size, buf[0], buf[1], buf[2], buf[3], buf[4]);

    init_get_bits(&s->gb, &buf[13], (buf_size - 13) * 8);
    s->avctx = avctx;

    if (parse_code ==  pc_access_unit_header) {
        if (parse_access_unit_header(s))
            return -1;

        /* Dump the header.  */
#if 1
        dump_sequence_parameters(avctx);
        dump_source_parameters(avctx);
#endif

        return 0;
    }

    /* If this is not a picture, return.  */
    if ((parse_code & 0x08) != 0x08)
        return 0;

    s->refs = parse_code & 0x03;

    parse_frame(s);

    avctx->pix_fmt = PIX_FMT_YUVJ420P; /* XXX */

    if (avcodec_check_dimensions(avctx, s->sequence.luma_width,
                                 s->sequence.luma_height)) {
        av_log(avctx, AV_LOG_ERROR,
               "avcodec_check_dimensions() failed\n");
        return -1;
    }

    avcodec_set_dimensions(avctx, s->sequence.luma_width,
                           s->sequence.luma_height);

    if (s->picture.data[0] != NULL)
        avctx->release_buffer(avctx, &s->picture);

    s->picture.reference = (parse_code & 0x04) == 0x04;

    if (avctx->get_buffer(avctx, &s->picture) < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        return -1;
    }

#if 1
    for (i = 0; i < s->refcnt; i++)
        dprintf(avctx, "Reference frame #%d\n",
                s->refframes[i].frame.display_picture_number);

    for (i = 0; i < s->refs; i++)
        dprintf(avctx, "Reference frame %d: #%d\n", i, s->ref[i]);
#endif

    if (dirac_decode_frame(s))
        return -1;

    s->picture.display_picture_number = s->picnum;

    if (s->picture.reference
        || s->picture.display_picture_number != avctx->frame_number) {
        if (s->refcnt + 1 == REFFRAME_CNT) {
            av_log(avctx, AV_LOG_ERROR, "reference picture buffer overrun\n");
            return -1;
        }

        s->refframes[s->refcnt].halfpel[0] = 0;
        s->refframes[s->refcnt].halfpel[1] = 0;
        s->refframes[s->refcnt].halfpel[2] = 0;
        s->refframes[s->refcnt++].frame = s->picture;
    }

    /* Retire frames that were reordered and displayed if they are no
       reference frames either.  */
    for (i = 0; i < s->refcnt; i++) {
        AVFrame *f = &s->refframes[i].frame;

        if (f->reference == 0
            && f->display_picture_number < avctx->frame_number) {
            s->retireframe[s->retirecnt++] = f->display_picture_number;
        }
    }

    for (i = 0; i < s->retirecnt; i++) {
        AVFrame *f;
        int idx, j;

        idx = reference_frame_idx(s, s->retireframe[i]);
        if (idx == -1) {
            av_log(avctx, AV_LOG_WARNING, "frame to retire #%d not found\n",
                   s->retireframe[i]);
            continue;
        }

        f = &s->refframes[idx].frame;
        /* Do not retire frames that were not displayed yet.  */
        if (f->display_picture_number >= avctx->frame_number) {
            f->reference = 0;
            continue;
        }

        if (f->data[0] != NULL)
            avctx->release_buffer(avctx, f);

        av_free(s->refframes[idx].halfpel[0]);
        av_free(s->refframes[idx].halfpel[1]);
        av_free(s->refframes[idx].halfpel[2]);

        s->refcnt--;

        for (j = idx; j < idx + s->refcnt; j++) {
            s->refframes[j] = s->refframes[j + 1];
        }
    }

    if (s->picture.display_picture_number > avctx->frame_number) {
        int idx;

        if (!s->picture.reference) {
            /* This picture needs to be shown at a later time.  */

            s->refframes[s->refcnt].halfpel[0] = 0;
            s->refframes[s->refcnt].halfpel[1] = 0;
            s->refframes[s->refcnt].halfpel[2] = 0;
            s->refframes[s->refcnt++].frame = s->picture;
        }

        idx = reference_frame_idx(s, avctx->frame_number);
        if (idx == -1) {
            /* The frame is not yet decoded.  */
            *data_size = 0;
        } else {
            *data_size = sizeof(AVFrame);
            *picture = s->refframes[idx].frame;
        }
    } else {
        /* The right frame at the right time :-) */
        *data_size = sizeof(AVFrame);
        *picture = s->picture;
    }

    if (s->picture.reference
        || s->picture.display_picture_number < avctx->frame_number)
        avcodec_get_frame_defaults(&s->picture);

    return buf_size;
}

static void dirac_encode_parse_info(DiracContext *s, int parsecode) {
    put_bits(&s->pb, 32, DIRAC_PARSE_INFO_PREFIX);
    put_bits(&s->pb, 8,  parsecode);
    /* XXX: These will be filled in after encoding.  */
    put_bits(&s->pb, 32, 0);
    put_bits(&s->pb, 32, 0);
}

static void dirac_encode_sequence_parameters(DiracContext *s) {
    AVCodecContext *avctx = s->avctx;
    struct sequence_parameters *seq = &s->sequence;
    const struct sequence_parameters *seqdef;
    int video_format = 0;

    seqdef = &sequence_parameters_defaults[video_format];

    /* Fill in defaults for the sequence parameters.  */
    s->sequence = *seqdef;

    /* Fill in the sequence parameters using the information set by
       the user. XXX: Only support YUV420P for now.  */
    seq->luma_width    = avctx->width;
    seq->luma_height   = avctx->height;
    seq->chroma_width  = avctx->width  / 2;
    seq->chroma_height = avctx->height / 2;
    seq->video_depth   = 8;
    seq->chroma_format = 2;

    /* Set video format to 0.  In the future a best match is perhaps
       better.  */
    dirac_set_ue_golomb(&s->pb, video_format);


    /* Override image dimensions.  */
    if (seq->luma_width != seqdef->luma_width
        || seq->luma_height != seqdef->luma_height) {
        put_bits(&s->pb, 1, 1);

        dirac_set_ue_golomb(&s->pb, seq->luma_width);
        dirac_set_ue_golomb(&s->pb, seq->luma_height);
    } else {
        put_bits(&s->pb, 1, 0);
    }

    /* Override chroma format.  */
    if (seq->chroma_format != seqdef->chroma_format) {
        put_bits(&s->pb, 1, 1);

        /* XXX: Hardcoded to 4:2:0.  */
        dirac_set_ue_golomb(&s->pb, 2);
    } else {
        put_bits(&s->pb, 1, 0);
    }

    /* Override video depth.  */
    if (seq->video_depth != seqdef->video_depth) {
        put_bits(&s->pb, 1, 1);

        dirac_set_ue_golomb(&s->pb, seq->video_depth);
    } else {
        put_bits(&s->pb, 1, 0);
    }
}

static void dirac_encode_source_parameters(DiracContext *s) {
    AVCodecContext *avctx = s->avctx;
    struct source_parameters *source = &s->source;
    const struct source_parameters *sourcedef;
    int video_format = 0;

    sourcedef = &source_parameters_defaults[video_format];

    /* Fill in defaults for the source parameters.  */
    s->source = *sourcedef;

    /* Fill in the source parameters using the information set by the
       user. XXX: No support for interlacing.  */
    source->interlaced         = 0;
    source->frame_rate.num     = avctx->time_base.den;
    source->frame_rate.den     = avctx->time_base.num;
    source->clean_width        = avctx->width;
    source->clean_height       = avctx->height;

    if (avctx->sample_aspect_ratio.num != 0)
        source->aspect_ratio = avctx->sample_aspect_ratio;

    /* Override interlacing options.  */
    if (source->interlaced != sourcedef->interlaced) {
        put_bits(&s->pb, 1, 1);

        put_bits(&s->pb, 1, source->interlaced);

        /* Override top field first flag.  */
        if (source->top_field_first != sourcedef->top_field_first) {
            put_bits(&s->pb, 1, 1);

            put_bits(&s->pb, 1, source->top_field_first);

        } else {
            put_bits(&s->pb, 1, 0);
        }

        /* Override sequential fields flag.  */
        if (source->sequential_fields != sourcedef->sequential_fields) {
            put_bits(&s->pb, 1, 1);

            put_bits(&s->pb, 1, source->sequential_fields);

        } else {
            put_bits(&s->pb, 1, 0);
        }

    } else {
        put_bits(&s->pb, 1, 0);
    }

    /* Override frame rate.  */
    if (av_cmp_q(source->frame_rate, sourcedef->frame_rate) != 0) {
        put_bits(&s->pb, 1, 1);

        /* XXX: Some default frame rates can be used.  For now just
           set the index to 0 and write the frame rate.  */
        dirac_set_ue_golomb(&s->pb, 0);

        dirac_set_ue_golomb(&s->pb, source->frame_rate.num);
        dirac_set_ue_golomb(&s->pb, source->frame_rate.den);
    } else {
        put_bits(&s->pb, 1, 0);
    }

    /* Override aspect ratio.  */
    if (av_cmp_q(source->aspect_ratio, sourcedef->aspect_ratio) != 0) {
        put_bits(&s->pb, 1, 1);

        /* XXX: Some default aspect ratios can be used.  For now just
           set the index to 0 and write the aspect ratio.  */
        dirac_set_ue_golomb(&s->pb, 0);

        dirac_set_ue_golomb(&s->pb, source->aspect_ratio.num);
        dirac_set_ue_golomb(&s->pb, source->aspect_ratio.den);
    } else {
        put_bits(&s->pb, 1, 0);
    }

    /* Override clean area.  */
    if (source->clean_width != sourcedef->clean_width
        || source->clean_height != sourcedef->clean_height
        || source->clean_left_offset != sourcedef->clean_left_offset
        || source->clean_right_offset != sourcedef->clean_right_offset) {
        put_bits(&s->pb, 1, 1);

        dirac_set_ue_golomb(&s->pb, source->clean_width);
        dirac_set_ue_golomb(&s->pb, source->clean_height);
        dirac_set_ue_golomb(&s->pb, source->clean_left_offset);
        dirac_set_ue_golomb(&s->pb, source->clean_right_offset);
    } else {
        put_bits(&s->pb, 1, 1);
    }

    /* Override signal range.  */
    if (source->luma_offset != sourcedef->luma_offset
        || source->luma_excursion != sourcedef->luma_excursion
        || source->chroma_offset != sourcedef->chroma_offset
        || source->chroma_excursion != sourcedef->chroma_excursion) {
        put_bits(&s->pb, 1, 1);

        /* XXX: Some default signal ranges can be used.  For now just
           set the index to 0 and write the signal range.  */
        dirac_set_ue_golomb(&s->pb, 0);

        dirac_set_ue_golomb(&s->pb, source->luma_offset);
        dirac_set_ue_golomb(&s->pb, source->luma_excursion);
        dirac_set_ue_golomb(&s->pb, source->chroma_offset);
        dirac_set_ue_golomb(&s->pb, source->chroma_excursion);
    } else {
        put_bits(&s->pb, 1, 0);
    }

    /* Override color spec.  */
    /* XXX: For now this won't be overridden at all.  Just set this to
       defaults.  */
    put_bits(&s->pb, 1, 0);
}

static void dirac_encode_access_unit_header(DiracContext *s) {
    /* First write the Access Unit Parse Parameters.  */

    dirac_set_ue_golomb(&s->pb, 0); /* version major */
    dirac_set_ue_golomb(&s->pb, 1); /* version minor */
    dirac_set_ue_golomb(&s->pb, 0); /* profile */
    dirac_set_ue_golomb(&s->pb, 0); /* level */

    dirac_encode_sequence_parameters(s);
    dirac_encode_source_parameters(s);
    /* Fill in defaults for the decoding parameters.  */
    s->decoding = decoding_parameters_defaults[0];
}



static void encode_coeff(DiracContext *s, int16_t *coeffs, int level,
                         int orientation, int x, int y) {
    int parent = 0;
    int nhood;
    int idx;
    int coeff;
    int xpos, ypos;
    struct dirac_arith_context_set *context;
    int16_t *coeffp;

    xpos   = coeff_posx(s, level, orientation, x);
    ypos   = coeff_posy(s, level, orientation, y);

    coeffp = &coeffs[xpos + ypos * s->padded_width];
    coeff  = *coeffp;

    /* The value of the pixel belonging to the lower level.  */
    if (level >= 2) {
        int px = coeff_posx(s, level - 1, orientation, x >> 1);
        int py = coeff_posy(s, level - 1, orientation, y >> 1);
        parent = coeffs[s->padded_width * py + px] != 0;
    }

    /* Determine if the pixel has only zeros in its neighbourhood.  */
    nhood = zero_neighbourhood(s, coeffp, y, x);

    /* Calculate an index into context_sets_waveletcoeff.  */
    idx = parent * 6 + (!nhood) * 3;
    idx += sign_predict(s, coeffp, orientation, y, x);

    context = &context_sets_waveletcoeff[idx];

    /* XXX: Quantization.  */

    /* Write out the coefficient.  */
    dirac_arith_write_int(&s->arith, context, coeff);
}

static void encode_codeblock(DiracContext *s, int16_t *coeffs, int level,
                             int orientation, int xpos, int ypos) {
    int blockcnt_one = (s->codeblocksh[level] + s->codeblocksv[level]) == 2;
    int left, right, top, bottom;
    int x, y;

    left   = (subband_width(s, level)  *  xpos     ) / s->codeblocksh[level];
    right  = (subband_width(s, level)  * (xpos + 1)) / s->codeblocksh[level];
    top    = (subband_height(s, level) *  ypos     ) / s->codeblocksv[level];
    bottom = (subband_height(s, level) * (ypos + 1)) / s->codeblocksv[level];

    if (!blockcnt_one) {
        int zero = 1;
        for (y = top; y < bottom; y++) {
            for (x = left; x < right; x++) {
                if (coeffs[x + y * s->padded_width] != 0) {
                    zero = 0;
                    break;
                }
            }
        }

        /* XXX: Check if this is a zero codeblock.  For now just
           encode like it isn't.  */
        dirac_arith_put_bit(&s->arith, ARITH_CONTEXT_ZERO_BLOCK, zero);

        if (zero)
            return;
    }

    for (y = top; y < bottom; y++)
        for (x = left; x < right; x++)
            encode_coeff(s, coeffs, level, orientation, x, y);
}

static void intra_dc_coding(DiracContext *s, int16_t *coeffs) {
    int x, y;
    int16_t *line = coeffs + (subband_height(s, 0) - 1) * s->padded_width;

    /* Just do the inverse of intra_dc_prediction.  Start at the right
       bottom corner and remove the predicted value from the
       coefficient, the decoder can easily reconstruct this.  */

    for (y = subband_height(s, 0) - 1; y >= 0; y--) {
        for (x = subband_width(s, 0) - 1; x >= 0; x--) {
            line[x] -= intra_dc_coeff_prediction(s, &line[x], x, y);
        }
        line -= s->padded_width;
    }
}

static int encode_subband(DiracContext *s, int level,
                          int orientation, int16_t *coeffs) {
    int xpos, ypos;
    int length;
    char *buf;
    PutBitContext pb;

    /* Encode the data.  */

    init_put_bits(&pb, s->encodebuf, (1 << 20) * 8);
    dirac_arith_coder_init(&s->arith, &pb);

    if (level == 0)
        intra_dc_coding(s, coeffs);

    for (ypos = 0; ypos < s->codeblocksv[level]; ypos++)
        for (xpos = 0; xpos < s->codeblocksh[level]; xpos++)
            encode_codeblock(s, coeffs, level, orientation, xpos, ypos);

    dirac_arith_coder_flush(&s->arith);
    flush_put_bits(&pb);

    /* Write length.  */
    length = put_bits_count(&pb) / 8;

    dirac_set_ue_golomb(&s->pb, length);

    /* Write quantizer index.  XXX: No quantization?  */
    dirac_set_ue_golomb(&s->pb, 0);

    /* Write out encoded data.  */
    align_put_bits(&s->pb);

    /* XXX: Use memmove.  */
    flush_put_bits(&s->pb);
    buf = pbBufPtr(&s->pb);
    memcpy(buf, s->encodebuf, length);
    skip_put_bytes(&s->pb, length);

    return 0;
}

static int dirac_encode_component(DiracContext *s, int comp) {
    int level;
    subband_t subband;
    int16_t *coeffs;
    int x, y;

    align_put_bits(&s->pb);

    if (comp == 0) {
        s->width         = s->sequence.luma_width;
        s->height        = s->sequence.luma_height;
        s->padded_width  = s->padded_luma_width;
        s->padded_height = s->padded_luma_height;
    } else {
        s->width         = s->sequence.chroma_width;
        s->height        = s->sequence.chroma_height;
        s->padded_width  = s->padded_chroma_width;
        s->padded_height = s->padded_chroma_height;
    }

    coeffs = av_mallocz(s->padded_width * s->padded_height * sizeof(int16_t));
    if (! coeffs) {
        av_log(s->avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }

    for (y = 0; y < s->height; y++) {
        for (x = 0; x < s->width; x++) {
            coeffs[y * s->padded_width + x] =
                s->picture.data[comp][y * s->picture.linesize[comp] + x];
        }
        for (x = s->width; x < s->padded_width; x++)
            coeffs[y * s->padded_width + x] =
                s->picture.data[comp][y * s->picture.linesize[comp]
                                      + s->width];
    }
    for (y = s->height; y < s->padded_height; y++) {
        for (x = 0; x < s->padded_width; x++)
            coeffs[y * s->padded_width + x] =
                s->picture.data[comp][s->height * s->picture.linesize[comp] + x];
    }

    dirac_dwt(s, coeffs);

    encode_subband(s, 0, subband_ll, coeffs);
    for (level = 1; level <= 4; level++) {
        for (subband = 1; subband <= subband_hh; subband++) {
            encode_subband(s, level, subband, coeffs);
        }
    }

    av_free(coeffs);

    return 0;
}

static int dirac_encode_frame(DiracContext *s) {
    PutBitContext *pb = &s->pb;
    int comp;
    int i;

    s->frame_decoding = s->decoding;

    /* Round up to a multiple of 2^depth.  */
    s->padded_luma_width    = CALC_PADDING(s->sequence.luma_width,
                                           s->frame_decoding.wavelet_depth);
    s->padded_luma_height   = CALC_PADDING(s->sequence.luma_height,
                                           s->frame_decoding.wavelet_depth);
    s->padded_chroma_width  = CALC_PADDING(s->sequence.chroma_width,
                                           s->frame_decoding.wavelet_depth);
    s->padded_chroma_height = CALC_PADDING(s->sequence.chroma_height,
                                           s->frame_decoding.wavelet_depth);

    /* Set defaults for the codeblocks.  */
    for (i = 0; i <= s->frame_decoding.wavelet_depth; i++) {
        if (s->refs == 0) {
            s->codeblocksh[i] = i <= 2 ? 1 : 4;
            s->codeblocksv[i] = i <= 2 ? 1 : 3;
        } else {
            if (i <= 1) {
                s->codeblocksh[i] = 1;
                s->codeblocksv[i] = 1;
            } else if (i == 2) {
                s->codeblocksh[i] = 8;
                s->codeblocksv[i] = 6;
            } else {
                s->codeblocksh[i] = 12;
                s->codeblocksv[i] = 8;
            }
        }
    }

    /* Write picture header.  */
    put_bits(pb, 32, s->avctx->frame_number - 1);

    /* XXX: Write reference frames.  */

    /* XXX: Write retire pictures list.  */
    dirac_set_ue_golomb(pb, 0);

    align_put_bits(pb);

    /* Wavelet transform parameters.  */

    /* Do not override default filter.  */
    put_bits(pb, 1, 1);

    /* Set the default filter to Deslauriers-Debuc.  */
    dirac_set_ue_golomb(pb, 1);

    /* Do not override the default depth.  */
    put_bits(pb, 1, 0);

    /* Use spatial partitioning.  */
    put_bits(pb, 1, 1);

    /* Do not override spatial partitioning.  */
    put_bits(pb, 1, 0);

    /* Codeblock mode.  */
    dirac_set_ue_golomb(pb, 0);


    /* Write the transform data.  */
    for (comp = 0; comp < 3; comp++) {
        if (dirac_encode_component(s, comp))
            return -1;
    }

    return 0;
}

static int encode_frame(AVCodecContext *avctx, unsigned char *buf,
                        int buf_size, void *data) {
    DiracContext *s = avctx->priv_data;
    AVFrame *picture = data;
    unsigned char *dst = &buf[5];
    int size;

    dprintf(avctx, "Encoding frame %p size=%d\n", buf, buf_size);

    init_put_bits(&s->pb, buf, buf_size);
    s->avctx = avctx;
    s->picture = *picture;

    if (s->next_parse_code == 0) {
        dirac_encode_parse_info(s, pc_access_unit_header);
        dirac_encode_access_unit_header(s);
        s->next_parse_code = 0x08;
    } else if (s->next_parse_code == 0x08) {
        dirac_encode_parse_info(s, 0x08);
        dirac_encode_frame(s);
    }

    flush_put_bits(&s->pb);
    size = put_bits_count(&s->pb) / 8;

    bytestream_put_be32(&dst, size);
    bytestream_put_be32(&dst, s->prev_size);
    s->prev_size = size;

    return size;
}

AVCodec dirac_decoder = {
    "dirac",
    CODEC_TYPE_VIDEO,
    CODEC_ID_DIRAC,
    sizeof(DiracContext),
    decode_init,
    NULL,
    decode_end,
    decode_frame,
    CODEC_CAP_DELAY,
    NULL
};

#ifdef CONFIG_ENCODERS
AVCodec dirac_encoder = {
    "dirac",
    CODEC_TYPE_VIDEO,
    CODEC_ID_DIRAC,
    sizeof(DiracContext),
    encode_init,
    encode_frame,
    encode_end,
    .pix_fmts = (enum PixelFormat[]) {PIX_FMT_YUV420P, -1}
};
#endif
