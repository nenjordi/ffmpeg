/*
 * Chinese AVS video (AVS1-P2, JiZhun profile) decoder.
 * Copyright (c) 2006  Stefan Gehrer <stefan.gehrer@gmx.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define SLICE_MIN_START_CODE    0x00000101
#define SLICE_MAX_START_CODE    0x000001af
#define EXT_START_CODE          0x000001b5
#define USER_START_CODE         0x000001b2
#define SEQ_START_CODE          0x000001b0
#define PIC_I_START_CODE        0x000001b3
#define PIC_PB_START_CODE       0x000001b6

#define A_AVAIL                          1
#define B_AVAIL                          2
#define C_AVAIL                          4
#define D_AVAIL                          8
#define NOT_AVAIL                       -1
#define REF_INTRA                       -2
#define REF_DIR                         -3

#define ESCAPE_CODE                     59

#define FWD0                          0x01
#define FWD1                          0x02
#define BWD0                          0x04
#define BWD1                          0x08
#define SYM0                          0x10
#define SYM1                          0x20

#define MV_BWD_OFFS                     12
#define MV_STRIDE                        4

enum mb_t {
  I_8X8 = 0,
  P_SKIP,
  P_16X16,
  P_16X8,
  P_8X16,
  P_8X8,
  B_SKIP,
  B_DIRECT,
  B_FWD_16X16,
  B_BWD_16X16,
  B_SYM_16X16,
  B_8X8 = 29
};

enum sub_mb_t {
  B_SUB_DIRECT,
  B_SUB_FWD,
  B_SUB_BWD,
  B_SUB_SYM
};

enum intra_luma_t {
  INTRA_L_VERT,
  INTRA_L_HORIZ,
  INTRA_L_LP,
  INTRA_L_DOWN_LEFT,
  INTRA_L_DOWN_RIGHT,
  INTRA_L_LP_LEFT,
  INTRA_L_LP_TOP,
  INTRA_L_DC_128
};

enum intra_chroma_t {
  INTRA_C_LP,
  INTRA_C_HORIZ,
  INTRA_C_VERT,
  INTRA_C_PLANE,
  INTRA_C_LP_LEFT,
  INTRA_C_LP_TOP,
  INTRA_C_DC_128,
};

enum mv_pred_t {
  MV_PRED_MEDIAN,
  MV_PRED_LEFT,
  MV_PRED_TOP,
  MV_PRED_TOPRIGHT,
  MV_PRED_PSKIP,
  MV_PRED_BSKIP
};

enum block_t {
  BLK_16X16,
  BLK_16X8,
  BLK_8X16,
  BLK_8X8
};

enum mv_loc_t {
  MV_FWD_D3 = 0,
  MV_FWD_B2,
  MV_FWD_B3,
  MV_FWD_C2,
  MV_FWD_A1,
  MV_FWD_X0,
  MV_FWD_X1,
  MV_FWD_A3 = 8,
  MV_FWD_X2,
  MV_FWD_X3,
  MV_BWD_D3 = MV_BWD_OFFS,
  MV_BWD_B2,
  MV_BWD_B3,
  MV_BWD_C2,
  MV_BWD_A1,
  MV_BWD_X0,
  MV_BWD_X1,
  MV_BWD_A3 = MV_BWD_OFFS+8,
  MV_BWD_X2,
  MV_BWD_X3
};

static const uint8_t b_partition_flags[14] = {
  0,0,0,0,0,
  FWD0|FWD1,
  BWD0|BWD1,
  FWD0|BWD1,
  BWD0|FWD1,
  FWD0|SYM1,
  BWD0|SYM1,
  SYM0|FWD1,
  SYM0|BWD1,
  SYM0|SYM1
};

static const uint8_t scan3x3[4] = {4,5,7,8};

static const uint8_t mv_scan[4] = {
    MV_FWD_X0,MV_FWD_X1,
    MV_FWD_X2,MV_FWD_X3
};

static const uint8_t cbp_tab[64][2] = {
  {63, 0},{15,15},{31,63},{47,31},{ 0,16},{14,32},{13,47},{11,13},
  { 7,14},{ 5,11},{10,12},{ 8, 5},{12,10},{61, 7},{ 4,48},{55, 3},
  { 1, 2},{ 2, 8},{59, 4},{ 3, 1},{62,61},{ 9,55},{ 6,59},{29,62},
  {45,29},{51,27},{23,23},{39,19},{27,30},{46,28},{53, 9},{30, 6},
  {43,60},{37,21},{60,44},{16,26},{21,51},{28,35},{19,18},{35,20},
  {42,24},{26,53},{44,17},{32,37},{58,39},{24,45},{20,58},{17,43},
  {18,42},{48,46},{22,36},{33,33},{25,34},{49,40},{40,52},{36,49},
  {34,50},{50,56},{52,25},{54,22},{41,54},{56,57},{38,41},{57,38}
};

static const uint8_t chroma_qp[64] = {
  0,  1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  32,33,34,35,36,37,38,39,40,41,42,42,43,43,44,44,
  45,45,46,46,47,47,48,48,48,49,49,49,50,50,50,51
};

static const uint8_t dequant_shift[64] = {
  14,14,14,14,14,14,14,14,
  13,13,13,13,13,13,13,13,
  13,12,12,12,12,12,12,12,
  11,11,11,11,11,11,11,11,
  11,10,10,10,10,10,10,10,
  10, 9, 9, 9, 9, 9, 9, 9,
  9, 8, 8, 8, 8, 8, 8, 8,
  7, 7, 7, 7, 7, 7, 7, 7
};

static const uint16_t dequant_mul[64] = {
  32768,36061,38968,42495,46341,50535,55437,60424,
  32932,35734,38968,42495,46177,50535,55109,59933,
  65535,35734,38968,42577,46341,50617,55027,60097,
  32809,35734,38968,42454,46382,50576,55109,60056,
  65535,35734,38968,42495,46320,50515,55109,60076,
  65535,35744,38968,42495,46341,50535,55099,60087,
  65535,35734,38973,42500,46341,50535,55109,60097,
  32771,35734,38965,42497,46341,50535,55109,60099
};

typedef struct {
    int16_t x;
    int16_t y;
    int16_t dist;
    int16_t ref;
} vector_t;

// marks block as unavailable, i.e. out of picture
//  or not yet decoded
static const vector_t un_mv    = {0,0,1,NOT_AVAIL};

//marks block as "no prediction from this direction"
// e.g. forward motion vector in BWD partition
static const vector_t dir_mv   = {0,0,1,REF_DIR};

//marks block as using intra prediction
static const vector_t intra_mv = {0,0,1,REF_INTRA};

typedef struct residual_vlc_t {
  int8_t rltab[59][3];
  int8_t level_add[26];
  int8_t golomb_order;
  int inc_limit;
  int8_t max_run;
} residual_vlc_t;

static const residual_vlc_t intra_2dvlc[7] = {
  {
    { //level / run / table_inc
      {  1, 0, 1},{ -1, 0, 1},{  1, 1, 1},{ -1, 1, 1},{  1, 2, 1},{ -1, 2, 1},
      {  1, 3, 1},{ -1, 3, 1},{  1, 4, 1},{ -1, 4, 1},{  1, 5, 1},{ -1, 5, 1},
      {  1, 6, 1},{ -1, 6, 1},{  1, 7, 1},{ -1, 7, 1},{  1, 8, 1},{ -1, 8, 1},
      {  1, 9, 1},{ -1, 9, 1},{  1,10, 1},{ -1,10, 1},{  2, 0, 2},{ -2, 0, 2},
      {  1,11, 1},{ -1,11, 1},{  1,12, 1},{ -1,12, 1},{  1,13, 1},{ -1,13, 1},
      {  1,14, 1},{ -1,14, 1},{  2, 1, 2},{ -2, 1, 2},{  1,15, 1},{ -1,15, 1},
      {  1,16, 1},{ -1,16, 1},{  3, 0, 3},{ -3, 0, 3},{  1,17, 1},{ -1,17, 1},
      {  1,18, 1},{ -1,18, 1},{  2, 2, 2},{ -2, 2, 2},{  1,19, 1},{ -1,19, 1},
      {  1,20, 1},{ -1,20, 1},{  2, 3, 2},{ -2, 3, 2},{  1,21, 1},{ -1,21, 1},
      {  2, 4, 2},{ -2, 4, 2},{  1,22, 1},{ -1,22, 1},{  0, 0,-1}
    },
    //level_add
    { 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2, 2, 2, 2, 2,-1,-1,-1},
    2, //golomb_order
    0, //inc_limit
    22, //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  2, 0, 1},{ -2, 0, 1},
      {  1, 2, 0},{ -1, 2, 0},{  0, 0, 0},{  1, 3, 0},{ -1, 3, 0},{  1, 4, 0},
      { -1, 4, 0},{  1, 5, 0},{ -1, 5, 0},{  3, 0, 2},{ -3, 0, 2},{  2, 1, 1},
      { -2, 1, 1},{  1, 6, 0},{ -1, 6, 0},{  1, 7, 0},{ -1, 7, 0},{  1, 8, 0},
      { -1, 8, 0},{  2, 2, 1},{ -2, 2, 1},{  4, 0, 2},{ -4, 0, 2},{  1, 9, 0},
      { -1, 9, 0},{  1,10, 0},{ -1,10, 0},{  2, 3, 1},{ -2, 3, 1},{  3, 1, 2},
      { -3, 1, 2},{  1,11, 0},{ -1,11, 0},{  2, 4, 1},{ -2, 4, 1},{  5, 0, 3},
      { -5, 0, 3},{  1,12, 0},{ -1,12, 0},{  2, 5, 1},{ -2, 5, 1},{  1,13, 0},
      { -1,13, 0},{  2, 6, 1},{ -2, 6, 1},{  2, 7, 1},{ -2, 7, 1},{  3, 2, 2},
      { -3, 2, 2},{  6, 0, 3},{ -6, 0, 3},{  1,14, 0},{ -1,14, 0}
    },
    //level_add
    { 7, 4, 4, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    1, //inc_limit
    14, //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  1, 1, 0},{ -1, 1, 0},
      {  3, 0, 1},{ -3, 0, 1},{  0, 0, 0},{  1, 2, 0},{ -1, 2, 0},{  2, 1, 0},
      { -2, 1, 0},{  4, 0, 1},{ -4, 0, 1},{  1, 3, 0},{ -1, 3, 0},{  5, 0, 2},
      { -5, 0, 2},{  1, 4, 0},{ -1, 4, 0},{  3, 1, 1},{ -3, 1, 1},{  2, 2, 0},
      { -2, 2, 0},{  1, 5, 0},{ -1, 5, 0},{  6, 0, 2},{ -6, 0, 2},{  2, 3, 0},
      { -2, 3, 0},{  1, 6, 0},{ -1, 6, 0},{  4, 1, 1},{ -4, 1, 1},{  7, 0, 2},
      { -7, 0, 2},{  3, 2, 1},{ -3, 2, 1},{  2, 4, 0},{ -2, 4, 0},{  1, 7, 0},
      { -1, 7, 0},{  2, 5, 0},{ -2, 5, 0},{  8, 0, 3},{ -8, 0, 3},{  1, 8, 0},
      { -1, 8, 0},{  5, 1, 2},{ -5, 1, 2},{  3, 3, 1},{ -3, 3, 1},{  2, 6, 0},
      { -2, 6, 0},{  9, 0, 3},{ -9, 0, 3},{  1, 9, 0},{ -1, 9, 0}
    },
    //level_add
    {10, 6, 4, 4, 3, 3, 3, 2, 2, 2,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    2, //inc_limit
    9, //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},{ -3, 0, 0},
      {  1, 1, 0},{ -1, 1, 0},{  0, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 1},
      { -5, 0, 1},{  2, 1, 0},{ -2, 1, 0},{  1, 2, 0},{ -1, 2, 0},{  6, 0, 1},
      { -6, 0, 1},{  3, 1, 0},{ -3, 1, 0},{  7, 0, 1},{ -7, 0, 1},{  1, 3, 0},
      { -1, 3, 0},{  8, 0, 2},{ -8, 0, 2},{  2, 2, 0},{ -2, 2, 0},{  4, 1, 0},
      { -4, 1, 0},{  1, 4, 0},{ -1, 4, 0},{  9, 0, 2},{ -9, 0, 2},{  5, 1, 1},
      { -5, 1, 1},{  2, 3, 0},{ -2, 3, 0},{ 10, 0, 2},{-10, 0, 2},{  3, 2, 0},
      { -3, 2, 0},{  1, 5, 0},{ -1, 5, 0},{ 11, 0, 3},{-11, 0, 3},{  6, 1, 1},
      { -6, 1, 1},{  1, 6, 0},{ -1, 6, 0},{  2, 4, 0},{ -2, 4, 0},{  3, 3, 0},
      { -3, 3, 0},{ 12, 0, 3},{-12, 0, 3},{  4, 2, 0},{ -4, 2, 0}
    },
    //level_add
    {13, 7, 5, 4, 3, 2, 2,-1,-1,-1 -1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    4, //inc_limit
    6, //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},{ -3, 0, 0},
      {  0, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 0},{ -5, 0, 0},{  6, 0, 0},
      { -6, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  7, 0, 0},{ -7, 0, 0},{  8, 0, 1},
      { -8, 0, 1},{  2, 1, 0},{ -2, 1, 0},{  9, 0, 1},{ -9, 0, 1},{ 10, 0, 1},
      {-10, 0, 1},{  1, 2, 0},{ -1, 2, 0},{  3, 1, 0},{ -3, 1, 0},{ 11, 0, 2},
      {-11, 0, 2},{  4, 1, 0},{ -4, 1, 0},{ 12, 0, 2},{-12, 0, 2},{ 13, 0, 2},
      {-13, 0, 2},{  5, 1, 0},{ -5, 1, 0},{  1, 3, 0},{ -1, 3, 0},{  2, 2, 0},
      { -2, 2, 0},{ 14, 0, 2},{-14, 0, 2},{  6, 1, 0},{ -6, 1, 0},{ 15, 0, 2},
      {-15, 0, 2},{ 16, 0, 2},{-16, 0, 2},{  3, 2, 0},{ -3, 2, 0},{  1, 4, 0},
      { -1, 4, 0},{  7, 1, 0},{ -7, 1, 0},{ 17, 0, 2},{-17, 0, 2},
    },
    //level_add
    {18, 8, 4, 2, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    7, //inc_limit
    4, //max_run
  },{
    { //level / run
      {  0, 0, 0},{  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},
      { -3, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 0},{ -5, 0, 0},{  6, 0, 0},
      { -6, 0, 0},{  7, 0, 0},{ -7, 0, 0},{  8, 0, 0},{ -8, 0, 0},{  9, 0, 0},
      { -9, 0, 0},{ 10, 0, 0},{-10, 0, 0},{  1, 1, 0},{ -1, 1, 0},{ 11, 0, 1},
      {-11, 0, 1},{ 12, 0, 1},{-12, 0, 1},{ 13, 0, 1},{-13, 0, 1},{  2, 1, 0},
      { -2, 1, 0},{ 14, 0, 1},{-14, 0, 1},{ 15, 0, 1},{-15, 0, 1},{  3, 1, 0},
      { -3, 1, 0},{ 16, 0, 1},{-16, 0, 1},{  1, 2, 0},{ -1, 2, 0},{ 17, 0, 1},
      {-17, 0, 1},{  4, 1, 0},{ -4, 1, 0},{ 18, 0, 1},{-18, 0, 1},{  5, 1, 0},
      { -5, 1, 0},{ 19, 0, 1},{-19, 0, 1},{ 20, 0, 1},{-20, 0, 1},{  6, 1, 0},
      { -6, 1, 0},{ 21, 0, 1},{-21, 0, 1},{  2, 2, 0},{ -2, 2, 0},
    },
    //level_add
    {22, 7, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    10, //inc_limit
    2, //max_run
  },{
    { //level / run
      {  0, 0, 0},{  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},
      { -3, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 0},{ -5, 0, 0},{  6, 0, 0},
      { -6, 0, 0},{  7, 0, 0},{ -7, 0, 0},{  8, 0, 0},{ -8, 0, 0},{  9, 0, 0},
      { -9, 0, 0},{ 10, 0, 0},{-10, 0, 0},{ 11, 0, 0},{-11, 0, 0},{ 12, 0, 0},
      {-12, 0, 0},{ 13, 0, 0},{-13, 0, 0},{ 14, 0, 0},{-14, 0, 0},{ 15, 0, 0},
      {-15, 0, 0},{ 16, 0, 0},{-16, 0, 0},{  1, 1, 0},{ -1, 1, 0},{ 17, 0, 0},
      {-17, 0, 0},{ 18, 0, 0},{-18, 0, 0},{ 19, 0, 0},{-19, 0, 0},{ 20, 0, 0},
      {-20, 0, 0},{ 21, 0, 0},{-21, 0, 0},{  2, 1, 0},{ -2, 1, 0},{ 22, 0, 0},
      {-22, 0, 0},{ 23, 0, 0},{-23, 0, 0},{ 24, 0, 0},{-24, 0, 0},{ 25, 0, 0},
      {-25, 0, 0},{  3, 1, 0},{ -3, 1, 0},{ 26, 0, 0},{-26, 0, 0}
    },
    //level_add
    {27, 4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    INT_MAX, //inc_limit
    1, //max_run
  }
};

static const residual_vlc_t inter_2dvlc[7] = {
  {
    { //level / run
      {  1, 0, 1},{ -1, 0, 1},{  1, 1, 1},{ -1, 1, 1},{  1, 2, 1},{ -1, 2, 1},
      {  1, 3, 1},{ -1, 3, 1},{  1, 4, 1},{ -1, 4, 1},{  1, 5, 1},{ -1, 5, 1},
      {  1, 6, 1},{ -1, 6, 1},{  1, 7, 1},{ -1, 7, 1},{  1, 8, 1},{ -1, 8, 1},
      {  1, 9, 1},{ -1, 9, 1},{  1,10, 1},{ -1,10, 1},{  1,11, 1},{ -1,11, 1},
      {  1,12, 1},{ -1,12, 1},{  2, 0, 2},{ -2, 0, 2},{  1,13, 1},{ -1,13, 1},
      {  1,14, 1},{ -1,14, 1},{  1,15, 1},{ -1,15, 1},{  1,16, 1},{ -1,16, 1},
      {  1,17, 1},{ -1,17, 1},{  1,18, 1},{ -1,18, 1},{  3, 0, 3},{ -3, 0, 3},
      {  1,19, 1},{ -1,19, 1},{  1,20, 1},{ -1,20, 1},{  2, 1, 2},{ -2, 1, 2},
      {  1,21, 1},{ -1,21, 1},{  1,22, 1},{ -1,22, 1},{  1,23, 1},{ -1,23, 1},
      {  1,24, 1},{ -1,24, 1},{  1,25, 1},{ -1,25, 1},{  0, 0,-1}
    },
    //level_add
    { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    3, //golomb_order
    0, //inc_limit
    25 //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  0, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  1, 2, 0},
      { -1, 2, 0},{  1, 3, 0},{ -1, 3, 0},{  1, 4, 0},{ -1, 4, 0},{  1, 5, 0},
      { -1, 5, 0},{  2, 0, 1},{ -2, 0, 1},{  1, 6, 0},{ -1, 6, 0},{  1, 7, 0},
      { -1, 7, 0},{  1, 8, 0},{ -1, 8, 0},{  1, 9, 0},{ -1, 9, 0},{  2, 1, 1},
      { -2, 1, 1},{  1,10, 0},{ -1,10, 0},{  1,11, 0},{ -1,11, 0},{  3, 0, 2},
      { -3, 0, 2},{  1,12, 0},{ -1,12, 0},{  1,13, 0},{ -1,13, 0},{  2, 2, 1},
      { -2, 2, 1},{  1,14, 0},{ -1,14, 0},{  2, 3, 1},{ -2, 3, 1},{  1,15, 0},
      { -1,15, 0},{  2, 4, 1},{ -2, 4, 1},{  1,16, 0},{ -1,16, 0},{  4, 0, 3},
      { -4, 0, 3},{  2, 5, 1},{ -2, 5, 1},{  1,17, 0},{ -1,17, 0},{  1,18, 0},
      { -1,18, 0},{  2, 6, 1},{ -2, 6, 1},{  3, 1, 2},{ -3, 1, 2},
    },
    //level_add
    { 5, 4, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    1, //inc_limit
    18 //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  0, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  2, 0, 0},
      { -2, 0, 0},{  1, 2, 0},{ -1, 2, 0},{  1, 3, 0},{ -1, 3, 0},{  3, 0, 1},
      { -3, 0, 1},{  2, 1, 0},{ -2, 1, 0},{  1, 4, 0},{ -1, 4, 0},{  1, 5, 0},
      { -1, 5, 0},{  1, 6, 0},{ -1, 6, 0},{  2, 2, 0},{ -2, 2, 0},{  4, 0, 2},
      { -4, 0, 2},{  1, 7, 0},{ -1, 7, 0},{  3, 1, 1},{ -3, 1, 1},{  2, 3, 0},
      { -2, 3, 0},{  1, 8, 0},{ -1, 8, 0},{  1, 9, 0},{ -1, 9, 0},{  5, 0, 2},
      { -5, 0, 2},{  2, 4, 0},{ -2, 4, 0},{  1,10, 0},{ -1,10, 0},{  2, 5, 0},
      { -2, 5, 0},{  1,11, 0},{ -1,11, 0},{  3, 2, 1},{ -3, 2, 1},{  6, 0, 2},
      { -6, 0, 2},{  4, 1, 2},{ -4, 1, 2},{  1,12, 0},{ -1,12, 0},{  2, 6, 0},
      { -2, 6, 0},{  3, 3, 1},{ -3, 3, 1},{  1,13, 0},{ -1,13, 0},
    },
    //level_add
    { 7, 5, 4, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    2, //inc_limit
    13 //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  0, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  1, 1, 0},
      { -1, 1, 0},{  3, 0, 0},{ -3, 0, 0},{  1, 2, 0},{ -1, 2, 0},{  2, 1, 0},
      { -2, 1, 0},{  4, 0, 1},{ -4, 0, 1},{  1, 3, 0},{ -1, 3, 0},{  5, 0, 1},
      { -5, 0, 1},{  1, 4, 0},{ -1, 4, 0},{  3, 1, 0},{ -3, 1, 0},{  2, 2, 0},
      { -2, 2, 0},{  1, 5, 0},{ -1, 5, 0},{  6, 0, 1},{ -6, 0, 1},{  2, 3, 0},
      { -2, 3, 0},{  1, 6, 0},{ -1, 6, 0},{  4, 1, 1},{ -4, 1, 1},{  7, 0, 2},
      { -7, 0, 2},{  3, 2, 0},{ -3, 2, 0},{  1, 7, 0},{ -1, 7, 0},{  2, 4, 0},
      { -2, 4, 0},{  8, 0, 2},{ -8, 0, 2},{  1, 8, 0},{ -1, 8, 0},{  3, 3, 0},
      { -3, 3, 0},{  2, 5, 0},{ -2, 5, 0},{  5, 1, 1},{ -5, 1, 1},{  1, 9, 0},
      { -1, 9, 0},{  9, 0, 2},{ -9, 0, 2},{  4, 2, 1},{ -4, 2, 1},
    },
    //level_add
    {10, 6, 5, 4, 3, 3, 2, 2, 2, 2,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    3, //inc_limit
    9 //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  0, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},
      { -3, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 0},
      { -5, 0, 0},{  2, 1, 0},{ -2, 1, 0},{  1, 2, 0},{ -1, 2, 0},{  6, 0, 0},
      { -6, 0, 0},{  3, 1, 0},{ -3, 1, 0},{  7, 0, 1},{ -7, 0, 1},{  1, 3, 0},
      { -1, 3, 0},{  8, 0, 1},{ -8, 0, 1},{  2, 2, 0},{ -2, 2, 0},{  4, 1, 0},
      { -4, 1, 0},{  1, 4, 0},{ -1, 4, 0},{  9, 0, 1},{ -9, 0, 1},{  5, 1, 0},
      { -5, 1, 0},{  2, 3, 0},{ -2, 3, 0},{  1, 5, 0},{ -1, 5, 0},{ 10, 0, 2},
      {-10, 0, 2},{  3, 2, 0},{ -3, 2, 0},{ 11, 0, 2},{-11, 0, 2},{  1, 6, 0},
      { -1, 6, 0},{  6, 1, 0},{ -6, 1, 0},{  3, 3, 0},{ -3, 3, 0},{  2, 4, 0},
      { -2, 4, 0},{ 12, 0, 2},{-12, 0, 2},{  4, 2, 0},{ -4, 2, 0},
    },
    //level_add
    {13, 7, 5, 4, 3, 2, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    6, //inc_limit
    6 //max_run
  },{
    { //level / run
      {  0, 0, 0},{  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},
      { -3, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 0},{ -5, 0, 0},{  1, 1, 0},
      { -1, 1, 0},{  6, 0, 0},{ -6, 0, 0},{  7, 0, 0},{ -7, 0, 0},{  8, 0, 0},
      { -8, 0, 0},{  2, 1, 0},{ -2, 1, 0},{  9, 0, 0},{ -9, 0, 0},{  1, 2, 0},
      { -1, 2, 0},{ 10, 0, 1},{-10, 0, 1},{  3, 1, 0},{ -3, 1, 0},{ 11, 0, 1},
      {-11, 0, 1},{  4, 1, 0},{ -4, 1, 0},{ 12, 0, 1},{-12, 0, 1},{  1, 3, 0},
      { -1, 3, 0},{  2, 2, 0},{ -2, 2, 0},{ 13, 0, 1},{-13, 0, 1},{  5, 1, 0},
      { -5, 1, 0},{ 14, 0, 1},{-14, 0, 1},{  6, 1, 0},{ -6, 1, 0},{  1, 4, 0},
      { -1, 4, 0},{ 15, 0, 1},{-15, 0, 1},{  3, 2, 0},{ -3, 2, 0},{ 16, 0, 1},
      {-16, 0, 1},{  2, 3, 0},{ -2, 3, 0},{  7, 1, 0},{ -7, 1, 0},
    },
    //level_add
    {17, 8, 4, 3, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    9, //inc_limit
    4 //max_run
  },{
    { //level / run
      {  0, 0, 0},{  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},
      { -3, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 0},{ -5, 0, 0},{  6, 0, 0},
      { -6, 0, 0},{  7, 0, 0},{ -7, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  8, 0, 0},
      { -8, 0, 0},{  9, 0, 0},{ -9, 0, 0},{ 10, 0, 0},{-10, 0, 0},{ 11, 0, 0},
      {-11, 0, 0},{ 12, 0, 0},{-12, 0, 0},{  2, 1, 0},{ -2, 1, 0},{ 13, 0, 0},
      {-13, 0, 0},{  1, 2, 0},{ -1, 2, 0},{ 14, 0, 0},{-14, 0, 0},{ 15, 0, 0},
      {-15, 0, 0},{  3, 1, 0},{ -3, 1, 0},{ 16, 0, 0},{-16, 0, 0},{ 17, 0, 0},
      {-17, 0, 0},{ 18, 0, 0},{-18, 0, 0},{  4, 1, 0},{ -4, 1, 0},{ 19, 0, 0},
      {-19, 0, 0},{ 20, 0, 0},{-20, 0, 0},{  2, 2, 0},{ -2, 2, 0},{  1, 3, 0},
      { -1, 3, 0},{  5, 1, 0},{ -5, 1, 0},{ 21, 0, 0},{-21, 0, 0},
    },
    //level_add
    {22, 6, 3, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    2, //golomb_order
    INT_MAX, //inc_limit
    3 //max_run
  }
};

static const residual_vlc_t chroma_2dvlc[5] = {
  {
    { //level / run
      {  1, 0, 1},{ -1, 0, 1},{  1, 1, 1},{ -1, 1, 1},{  1, 2, 1},{ -1, 2, 1},
      {  1, 3, 1},{ -1, 3, 1},{  1, 4, 1},{ -1, 4, 1},{  1, 5, 1},{ -1, 5, 1},
      {  1, 6, 1},{ -1, 6, 1},{  2, 0, 2},{ -2, 0, 2},{  1, 7, 1},{ -1, 7, 1},
      {  1, 8, 1},{ -1, 8, 1},{  1, 9, 1},{ -1, 9, 1},{  1,10, 1},{ -1,10, 1},
      {  1,11, 1},{ -1,11, 1},{  1,12, 1},{ -1,12, 1},{  1,13, 1},{ -1,13, 1},
      {  1,14, 1},{ -1,14, 1},{  3, 0, 3},{ -3, 0, 3},{  1,15, 1},{ -1,15, 1},
      {  1,16, 1},{ -1,16, 1},{  1,17, 1},{ -1,17, 1},{  1,18, 1},{ -1,18, 1},
      {  1,19, 1},{ -1,19, 1},{  1,20, 1},{ -1,20, 1},{  1,21, 1},{ -1,21, 1},
      {  2, 1, 2},{ -2, 1, 2},{  1,22, 1},{ -1,22, 1},{  1,23, 1},{ -1,23, 1},
      {  1,24, 1},{ -1,24, 1},{  4, 0, 3},{ -4, 0, 3},{  0, 0,-1}
    },
    //level_add
    { 5, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2, 2, 2, 2, 2, 2, 2,-1},
    2, //golomb_order
    0, //inc_limit
    24, //max_run
  },{
    { //level / run
      {  0, 0, 0},{  1, 0, 0},{ -1, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  2, 0, 1},
      { -2, 0, 1},{  1, 2, 0},{ -1, 2, 0},{  1, 3, 0},{ -1, 3, 0},{  1, 4, 0},
      { -1, 4, 0},{  1, 5, 0},{ -1, 5, 0},{  3, 0, 2},{ -3, 0, 2},{  1, 6, 0},
      { -1, 6, 0},{  1, 7, 0},{ -1, 7, 0},{  2, 1, 1},{ -2, 1, 1},{  1, 8, 0},
      { -1, 8, 0},{  1, 9, 0},{ -1, 9, 0},{  1,10, 0},{ -1,10, 0},{  4, 0, 2},
      { -4, 0, 2},{  1,11, 0},{ -1,11, 0},{  1,12, 0},{ -1,12, 0},{  1,13, 0},
      { -1,13, 0},{  2, 2, 1},{ -2, 2, 1},{  1,14, 0},{ -1,14, 0},{  2, 3, 1},
      { -2, 3, 1},{  5, 0, 3},{ -5, 0, 3},{  3, 1, 2},{ -3, 1, 2},{  1,15, 0},
      { -1,15, 0},{  1,16, 0},{ -1,16, 0},{  1,17, 0},{ -1,17, 0},{  2, 4, 1},
      { -2, 4, 1},{  1,18, 0},{ -1,18, 0},{  1,19, 0},{ -1,19, 0},
    },
    //level_add
    { 6, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2, 2,-1,-1,-1,-1,-1,-1},
    0, //golomb_order
    1, //inc_limit
    19, //max_run
  },{
    { //level / run
      {  1, 0, 0},{ -1, 0, 0},{  0, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  1, 1, 0},
      { -1, 1, 0},{  3, 0, 1},{ -3, 0, 1},{  1, 2, 0},{ -1, 2, 0},{  4, 0, 1},
      { -4, 0, 1},{  2, 1, 0},{ -2, 1, 0},{  1, 3, 0},{ -1, 3, 0},{  5, 0, 2},
      { -5, 0, 2},{  1, 4, 0},{ -1, 4, 0},{  3, 1, 1},{ -3, 1, 1},{  2, 2, 0},
      { -2, 2, 0},{  1, 5, 0},{ -1, 5, 0},{  6, 0, 2},{ -6, 0, 2},{  1, 6, 0},
      { -1, 6, 0},{  2, 3, 0},{ -2, 3, 0},{  7, 0, 2},{ -7, 0, 2},{  1, 7, 0},
      { -1, 7, 0},{  4, 1, 1},{ -4, 1, 1},{  1, 8, 0},{ -1, 8, 0},{  3, 2, 1},
      { -3, 2, 1},{  2, 4, 0},{ -2, 4, 0},{  2, 5, 0},{ -2, 5, 0},{  8, 0, 2},
      { -8, 0, 2},{  1, 9, 0},{ -1, 9, 0},{  1,10, 0},{ -1,10, 0},{  9, 0, 2},
      { -9, 0, 2},{  5, 1, 2},{ -5, 1, 2},{  3, 3, 1},{ -3, 3, 1},
    },
    //level_add
    {10, 6, 4, 4, 3, 3, 2, 2, 2, 2, 2,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    1, //golomb_order
    2, //inc_limit
    10, //max_run
  },{
    { //level / run
      {  0, 0, 0},{  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},
      { -3, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  1, 1, 0},{ -1, 1, 0},{  5, 0, 1},
      { -5, 0, 1},{  2, 1, 0},{ -2, 1, 0},{  6, 0, 1},{ -6, 0, 1},{  1, 2, 0},
      { -1, 2, 0},{  7, 0, 1},{ -7, 0, 1},{  3, 1, 0},{ -3, 1, 0},{  8, 0, 1},
      { -8, 0, 1},{  1, 3, 0},{ -1, 3, 0},{  2, 2, 0},{ -2, 2, 0},{  9, 0, 1},
      { -9, 0, 1},{  4, 1, 0},{ -4, 1, 0},{  1, 4, 0},{ -1, 4, 0},{ 10, 0, 1},
      {-10, 0, 1},{  3, 2, 0},{ -3, 2, 0},{  5, 1, 1},{ -5, 1, 1},{  2, 3, 0},
      { -2, 3, 0},{ 11, 0, 1},{-11, 0, 1},{  1, 5, 0},{ -1, 5, 0},{ 12, 0, 1},
      {-12, 0, 1},{  1, 6, 0},{ -1, 6, 0},{  6, 1, 1},{ -6, 1, 1},{ 13, 0, 1},
      {-13, 0, 1},{  2, 4, 0},{ -2, 4, 0},{  1, 7, 0},{ -1, 7, 0},
    },
    //level_add
    {14, 7, 4, 3, 3, 2, 2, 2,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    1, //golomb_order
    4, //inc_limit
    7, //max_run
  },{
    { //level / run
      {  0, 0, 0},{  1, 0, 0},{ -1, 0, 0},{  2, 0, 0},{ -2, 0, 0},{  3, 0, 0},
      { -3, 0, 0},{  4, 0, 0},{ -4, 0, 0},{  5, 0, 0},{ -5, 0, 0},{  6, 0, 0},
      { -6, 0, 0},{  7, 0, 0},{ -7, 0, 0},{  8, 0, 0},{ -8, 0, 0},{  1, 1, 0},
      { -1, 1, 0},{  9, 0, 0},{ -9, 0, 0},{ 10, 0, 0},{-10, 0, 0},{ 11, 0, 0},
      {-11, 0, 0},{  2, 1, 0},{ -2, 1, 0},{ 12, 0, 0},{-12, 0, 0},{ 13, 0, 0},
      {-13, 0, 0},{  3, 1, 0},{ -3, 1, 0},{ 14, 0, 0},{-14, 0, 0},{  1, 2, 0},
      { -1, 2, 0},{ 15, 0, 0},{-15, 0, 0},{  4, 1, 0},{ -4, 1, 0},{ 16, 0, 0},
      {-16, 0, 0},{ 17, 0, 0},{-17, 0, 0},{  5, 1, 0},{ -5, 1, 0},{  1, 3, 0},
      { -1, 3, 0},{  2, 2, 0},{ -2, 2, 0},{ 18, 0, 0},{-18, 0, 0},{  6, 1, 0},
      { -6, 1, 0},{ 19, 0, 0},{-19, 0, 0},{  1, 4, 0},{ -1, 4, 0},
    },
    //level_add
    {20, 7, 3, 2, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     -1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    0, //golomb_order
    INT_MAX, //inc_limit
    4, //max_run
  }
};

static const uint8_t alpha_tab[64] = {
   0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  2,  2,  2,  3,  3,
   4,  4,  5,  5,  6,  7,  8,  9, 10, 11, 12, 13, 15, 16, 18, 20,
  22, 24, 26, 28, 30, 33, 33, 35, 35, 36, 37, 37, 39, 39, 42, 44,
  46, 48, 50, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64
};

static const uint8_t beta_tab[64] = {
   0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,
   2,  2,  3,  3,  3,  3,  4,  4,  4,  4,  5,  5,  5,  5,  6,  6,
   6,  7,  7,  7,  8,  8,  8,  9,  9, 10, 10, 11, 11, 12, 13, 14,
  15, 16, 17, 18, 19, 20, 21, 22, 23, 23, 24, 24, 25, 25, 26, 27
};

static const uint8_t tc_tab[64] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2,
  2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4,
  5, 5, 5, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9
};

static const int_fast8_t left_modifier_l[8] = { 0,-1, 6,-1,-1, 7, 6, 7};
static const int_fast8_t top_modifier_l[8]  = {-1, 1, 5,-1,-1, 5, 7, 7};
static const int_fast8_t left_modifier_c[7] = { 5,-1, 2,-1, 6, 5, 6};
static const int_fast8_t top_modifier_c[7]  = { 4, 1,-1,-1, 4, 6, 6};
