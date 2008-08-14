/*
 * AAC encoder
 * Copyright (C) 2008 Konstantin Shishkov
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
 * @file aacenc.c
 * AAC encoder
 */

/***********************************
 *              TODOs:
 * psy model selection with some option
 * change greedy codebook search into something more optimal, like Viterbi algorithm
 * determine run lengths along with codebook
 ***********************************/

#include "avcodec.h"
#include "bitstream.h"
#include "dsputil.h"
#include "mpeg4audio.h"

#include "aacpsy.h"
#include "aac.h"
#include "aactab.h"

static const uint8_t swb_size_1024_96[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8,
    12, 12, 12, 12, 12, 16, 16, 24, 28, 36, 44,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

static const uint8_t swb_size_1024_64[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8,
    12, 12, 12, 16, 16, 16, 20, 24, 24, 28, 36,
    40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40
};

static const uint8_t swb_size_1024_48[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8,
    12, 12, 12, 12, 16, 16, 20, 20, 24, 24, 28, 28,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    96
};

static const uint8_t swb_size_1024_32[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8,
    12, 12, 12, 12, 16, 16, 20, 20, 24, 24, 28, 28,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32
};

static const uint8_t swb_size_1024_24[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    12, 12, 12, 12, 16, 16, 16, 20, 20, 24, 24, 28, 28,
    32, 36, 36, 40, 44, 48, 52, 52, 64, 64, 64, 64, 64
};

static const uint8_t swb_size_1024_16[] = {
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 20, 20, 20, 24, 24, 28, 28,
    32, 36, 40, 40, 44, 48, 52, 56, 60, 64, 64, 64
};

static const uint8_t swb_size_1024_8[] = {
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    16, 16, 16, 16, 16, 16, 16, 20, 20, 20, 20, 24, 24, 24, 28, 28,
    32, 36, 36, 40, 44, 48, 52, 56, 60, 64, 80
};

static const uint8_t *swb_size_1024[] = {
    swb_size_1024_96, swb_size_1024_96, swb_size_1024_64,
    swb_size_1024_48, swb_size_1024_48, swb_size_1024_32,
    swb_size_1024_24, swb_size_1024_24, swb_size_1024_16,
    swb_size_1024_16, swb_size_1024_16, swb_size_1024_8
};

static const uint8_t swb_size_128_96[] = {
    4, 4, 4, 4, 4, 4, 8, 8, 8, 16, 28, 36
};

static const uint8_t swb_size_128_48[] = {
    4, 4, 4, 4, 4, 8, 8, 8, 12, 12, 12, 16, 16, 16
};

static const uint8_t swb_size_128_24[] = {
    4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 12, 12, 16, 16, 20
};

static const uint8_t swb_size_128_16[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 12, 12, 16, 20, 20
};

static const uint8_t swb_size_128_8[] = {
    4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 12, 16, 20, 20
};

static const uint8_t *swb_size_128[] = {
    /* the last entry on the following row is swb_size_128_64 but is a
       duplicate of swb_size_128_96 */
    swb_size_128_96, swb_size_128_96, swb_size_128_96,
    swb_size_128_48, swb_size_128_48, swb_size_128_48,
    swb_size_128_24, swb_size_128_24, swb_size_128_16,
    swb_size_128_16, swb_size_128_16, swb_size_128_8
};

#define CB_UNSIGNED 0x01    ///< coefficients are coded as absolute values
#define CB_PAIRS    0x02    ///< coefficients are grouped into pairs before coding (quads by default)
#define CB_ESCAPE   0x04    ///< codebook allows escapes

/** spectral coefficients codebook information */
static const struct {
    int16_t maxval;         ///< maximum possible value
     int8_t cb_num;         ///< codebook number
    uint8_t flags;          ///< codebook features
} aac_cb_info[] = {
    {    0, -1, CB_UNSIGNED }, // zero codebook
    {    1,  0, 0 },
    {    1,  1, 0 },
    {    2,  2, CB_UNSIGNED },
    {    2,  3, CB_UNSIGNED },
    {    4,  4, CB_PAIRS },
    {    4,  5, CB_PAIRS },
    {    7,  6, CB_PAIRS | CB_UNSIGNED },
    {    7,  7, CB_PAIRS | CB_UNSIGNED },
    {   12,  8, CB_PAIRS | CB_UNSIGNED },
    {   12,  9, CB_PAIRS | CB_UNSIGNED },
    { 8191, 10, CB_PAIRS | CB_UNSIGNED | CB_ESCAPE },
    {   -1, -1, 0 }, // reserved
    {   -1, -1, 0 }, // perceptual noise substitution
    {   -1, -1, 0 }, // intensity out-of-phase
    {   -1, -1, 0 }, // intensity in-phase
};

/** default channel configurations */
static const uint8_t aac_chan_configs[6][5] = {
 {1, ID_SCE},                         // 1 channel  - single channel element
 {1, ID_CPE},                         // 2 channels - channel pair
 {2, ID_SCE, ID_CPE},                 // 3 channels - center + stereo
 {3, ID_SCE, ID_CPE, ID_SCE},         // 4 channels - front center + stereo + back center
 {3, ID_SCE, ID_CPE, ID_CPE},         // 5 channels - front center + stereo + back stereo
 {4, ID_SCE, ID_CPE, ID_CPE, ID_LFE}, // 6 channels - front center + stereo + back stereo + LFE
};

/**
 * AAC encoder context
 */
typedef struct {
    PutBitContext pb;
    MDCTContext mdct1024;                        ///< long (1024 samples) frame transform context
    MDCTContext mdct128;                         ///< short (128 samples) frame transform context
    DSPContext  dsp;
    DECLARE_ALIGNED_16(FFTSample, output[2048]); ///< temporary buffer for MDCT input coefficients
    DECLARE_ALIGNED_16(FFTSample, tmp[1024]);    ///< temporary buffer used by MDCT
    int16_t* samples;                            ///< saved preprocessed input

    int samplerate_index;                        ///< MPEG-4 samplerate index
    const uint8_t *swb_sizes1024;                ///< scalefactor band sizes for long frame
    int swb_num1024;                             ///< number of scalefactor bands for long frame
    const uint8_t *swb_sizes128;                 ///< scalefactor band sizes for short frame
    int swb_num128;                              ///< number of scalefactor bands for short frame

    ChannelElement *cpe;                         ///< channel elements
    AACPsyContext psy;                           ///< psychoacoustic model context
    int last_frame;
} AACEncContext;

/**
 * Make AAC audio config object.
 * @see 1.6.2.1 "Syntax - AudioSpecificConfig"
 */
static void put_audio_specific_config(AVCodecContext *avctx)
{
    PutBitContext pb;
    AACEncContext *s = avctx->priv_data;

    init_put_bits(&pb, avctx->extradata, avctx->extradata_size*8);
    put_bits(&pb, 5, 2); //object type - AAC-LC
    put_bits(&pb, 4, s->samplerate_index); //sample rate index
    put_bits(&pb, 4, avctx->channels);
    //GASpecificConfig
    put_bits(&pb, 1, 0); //frame length - 1024 samples
    put_bits(&pb, 1, 0); //does not depend on core coder
    put_bits(&pb, 1, 0); //is not extension
    flush_put_bits(&pb);
}

static av_cold int aac_encode_init(AVCodecContext *avctx)
{
    AACEncContext *s = avctx->priv_data;
    int i;

    avctx->frame_size = 1024;

    for(i = 0; i < 16; i++)
        if(avctx->sample_rate == ff_mpeg4audio_sample_rates[i])
            break;
    if(i == 16){
        av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate %d\n", avctx->sample_rate);
        return -1;
    }
    if(avctx->channels > 6){
        av_log(avctx, AV_LOG_ERROR, "Unsupported number of channels: %d\n", avctx->channels);
        return -1;
    }
    s->samplerate_index = i;
    s->swb_sizes1024 = swb_size_1024[i];
    s->swb_num1024   = ff_aac_num_swb_1024[i];
    s->swb_sizes128  = swb_size_128[i];
    s->swb_num128    = ff_aac_num_swb_128[i];

    dsputil_init(&s->dsp, avctx);
    ff_mdct_init(&s->mdct1024, 11, 0);
    ff_mdct_init(&s->mdct128,   8, 0);
    // window init
    ff_kbd_window_init(ff_aac_kbd_long_1024, 4.0, 1024);
    ff_kbd_window_init(ff_aac_kbd_short_128, 6.0, 128);
    ff_sine_window_init(ff_aac_sine_long_1024, 1024);
    ff_sine_window_init(ff_aac_sine_short_128, 128);

    s->samples = av_malloc(2 * 1024 * avctx->channels * sizeof(s->samples[0]));
    s->cpe = av_mallocz(sizeof(ChannelElement) * aac_chan_configs[avctx->channels-1][0]);
    if(ff_aac_psy_init(&s->psy, avctx, AAC_PSY_3GPP, aac_chan_configs[avctx->channels-1][0], 0, s->swb_sizes1024, s->swb_num1024, s->swb_sizes128, s->swb_num128) < 0){
        av_log(avctx, AV_LOG_ERROR, "Cannot initialize selected model.\n");
        return -1;
    }
    avctx->extradata = av_malloc(2);
    avctx->extradata_size = 2;
    put_audio_specific_config(avctx);
    return 0;
}

/**
 * Encode ics_info element.
 * @see Table 4.6 (syntax of ics_info)
 */
static void put_ics_info(AVCodecContext *avctx, IndividualChannelStream *info)
{
    AACEncContext *s = avctx->priv_data;
    int i;

    put_bits(&s->pb, 1, 0);                // ics_reserved bit
    put_bits(&s->pb, 2, info->window_sequence[0]);
    put_bits(&s->pb, 1, info->use_kb_window[0]);
    if(info->window_sequence[0] != EIGHT_SHORT_SEQUENCE){
        put_bits(&s->pb, 6, info->max_sfb);
        put_bits(&s->pb, 1, 0);            // no prediction
    }else{
        put_bits(&s->pb, 4, info->max_sfb);
        for(i = 1; i < info->num_windows; i++)
            put_bits(&s->pb, 1, info->group_len[i]);
    }
}

/**
 * Write some auxiliary information about the created AAC file.
 */
static void put_bitstream_info(AVCodecContext *avctx, AACEncContext *s, const char *name)
{
    int i, namelen, padbits;

    namelen = strlen(name) + 2;
    put_bits(&s->pb, 3, ID_FIL);
    put_bits(&s->pb, 4, FFMIN(namelen, 15));
    if(namelen >= 15)
        put_bits(&s->pb, 8, namelen - 16);
    put_bits(&s->pb, 4, 0); //extension type - filler
    padbits = 8 - (put_bits_count(&s->pb) & 7);
    align_put_bits(&s->pb);
    for(i = 0; i < namelen - 2; i++)
        put_bits(&s->pb, 8, name[i]);
    put_bits(&s->pb, 12 - padbits, 0);
}

static av_cold int aac_encode_end(AVCodecContext *avctx)
{
    AACEncContext *s = avctx->priv_data;

    ff_mdct_end(&s->mdct1024);
    ff_mdct_end(&s->mdct128);
    ff_aac_psy_end(&s->psy);
    av_freep(&s->samples);
    av_freep(&s->cpe);
    return 0;
}

AVCodec aac_encoder = {
    "aac",
    CODEC_TYPE_AUDIO,
    CODEC_ID_AAC,
    sizeof(AACEncContext),
    aac_encode_init,
    aac_encode_frame,
    aac_encode_end,
    .capabilities = CODEC_CAP_SMALL_LAST_FRAME | CODEC_CAP_DELAY,
    .sample_fmts = (enum SampleFormat[]){SAMPLE_FMT_S16,SAMPLE_FMT_NONE},
    .long_name = NULL_IF_CONFIG_SMALL("Advanced Audio Coding"),
};
