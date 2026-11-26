// SPDX-License-Identifier: GPL-2.0-only
/*
 * Smart dimming for the Samsung AMSA05RB06 panel's Anapass ANA38401 DDI:
 * the gamma for each brightness level, computed from the panel's factory
 * calibration (MTP), as Samsung's driver computes it, and the AMOLED off
 * ratio (AOR) and ELVSS that go with each level.
 *
 * The code and tables below are Samsung's, from its SM-T860 kernel
 * (ss_dsi_smart_dimming_ANA38401_AMSA05RB06.[ch] and
 * ss_dsi_smart_dimming_common.c for the gamma, Copyright (c) 2017 Samsung
 * Electronics; ss_dsi_interpolation_ANA38401_AMSA05RB06.c for the AOR and
 * ELVSS), without HBM, debug output and the interfaces to Samsung's display
 * framework, kept as they are so that the result stays Samsung's: on this
 * tablet's panel it is, byte for byte. Changes: only the gamma 2.2 curve is
 * searched (the only one used), a division by 0 gives 0, as on arm64,
 * instead of being undefined, and variables that are set but never read
 * are gone.
 *
 * Not reentrant (Samsung's code keeps state in static variables): the panel
 * driver calls ana38401_dimming_gamma() once, under its command lock.
 */

#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "panel-samsung-amsa05rb06-dimming.h"

/* do_div() takes a 32-bit divisor, and Samsung passes 64-bit ones */
#define ana38401_do_div(n, base) ({				\
	u32 __ana38401_base = (base);				\
	u32 __ana38401_rem = 0;					\
								\
	if (__ana38401_base)					\
		__ana38401_rem = do_div(n, __ana38401_base);	\
	else							\
		(n) = 0;					\
	__ana38401_rem;						\
})

enum {
	GAMMA_CURVE_1P9 = 0,
	GAMMA_CURVE_2P15,
	GAMMA_CURVE_2P2,
};

#define LUMINANCE_MAX 65
#define GAMMA_SET_MAX 33
#define BIT_SHIFT 22
/*
	it means BIT_SHIFT is 22.  pow(2,BIT_SHIFT) is 4194304.
	BIT_SHIFT is used for right bit shfit
*/
#define BIT_SHFIT_MUL 4194304

#define GRAY_SCALE_MAX 256

/* 6.8 * 4194304 */
#define ANA34801_VREG0_REF_L 28521267
/* 1.5 * 4194304 */
#define ANA34801_VREG0_REF_H 6291456


/*VT V0,V3,V11,V23,V35,V51,V87,V151,V203,V255*/
#define ARRAY_MAX 11

/* PANEL DEPENDENT THINGS */
#define MAX_CANDELA 360
#define MIN_CANDELA	2

/*
*	ID 0x20
*/
#define V255_300CD_R_MSB_20 0x01
#define V255_300CD_R_LSB_20 0x00

#define V255_300CD_G_MSB_20 0x01
#define V255_300CD_G_LSB_20 0x00

#define V255_300CD_B_MSB_20 0x01
#define V255_300CD_B_LSB_20 0x00

#define V203_300CD_R_20 0x80
#define V203_300CD_G_20 0x80
#define V203_300CD_B_20 0x80

#define V151_300CD_R_20 0x80
#define V151_300CD_G_20 0x80
#define V151_300CD_B_20 0x80

#define V87_300CD_R_20 0x80
#define V87_300CD_G_20 0x80
#define V87_300CD_B_20 0x80

#define V51_300CD_R_20 0x80
#define V51_300CD_G_20 0x80
#define V51_300CD_B_20 0x80

#define V35_300CD_R_20 0x80
#define V35_300CD_G_20 0x80
#define V35_300CD_B_20 0x80

#define V23_300CD_R_20 0x80
#define V23_300CD_G_20 0x80
#define V23_300CD_B_20 0x80

#define V11_300CD_R_20 0x80
#define V11_300CD_G_20 0x80
#define V11_300CD_B_20 0x80

#define V3_300CD_R_20 0x80
#define V3_300CD_G_20 0x80
#define V3_300CD_B_20 0x80

#define VT_300CD_R_20 0x00
#define VT_300CD_G_20 0x00
#define VT_300CD_B_20 0x00

#define HBM_INTERPOLATION_STEP 8

enum {
	V1_INDEX = 0,
	V3_INDEX = 1,
	V11_INDEX = 2,
	V23_INDEX = 3,
	V35_INDEX = 4,
	V51_INDEX = 5,
	V87_INDEX = 6,
	V151_INDEX = 7,
	V203_INDEX = 8,
	V255_INDEX = 9,
};

struct GAMMA_LEVEL {
	int level_0;
	int level_1;
	int level_3;
	int level_11;
	int level_23;
	int level_35;
	int level_51;
	int level_87;
	int level_151;
	int level_203;
	int level_255;
} __packed;

struct RGB_OUTPUT_VOLTARE {
	struct GAMMA_LEVEL R_VOLTAGE;
	struct GAMMA_LEVEL G_VOLTAGE;
	struct GAMMA_LEVEL B_VOLTAGE;
} __packed;

struct GRAY_VOLTAGE {
	/*
		This voltage value use 14bit right shit
		it means voltage is divied by 16384.
	*/
	int R_Gray;
	int G_Gray;
	int B_Gray;
} __packed;

struct GRAY_SCALE {
	struct GRAY_VOLTAGE TABLE[GRAY_SCALE_MAX];
	struct GRAY_VOLTAGE VT_TABLE;
} __packed;

/*V0,V1,V3,V11,V23,V35,V51,V87,V151,V203,V255*/

struct MTP_SET {
	char OFFSET_255_MSB;
	char OFFSET_255_LSB;
	char OFFSET_203;
	char OFFSET_151;
	char OFFSET_87;
	char OFFSET_51;
	char OFFSET_35;
	char OFFSET_23;
	char OFFSET_11;
	char OFFSET_3;
	char OFFSET_1;
} __packed;

struct MTP_OFFSET {
	struct MTP_SET R_OFFSET;
	struct MTP_SET G_OFFSET;
	struct MTP_SET B_OFFSET;
} __packed;

struct illuminance_table {
	int lux;
	unsigned char gamma_setting[GAMMA_SET_MAX];
} __packed;

struct SMART_DIM {
	struct MTP_OFFSET MTP_ORIGN;
	struct MTP_OFFSET MTP;
	struct RGB_OUTPUT_VOLTARE RGB_OUTPUT;
	struct GRAY_SCALE GRAY;

	/* Because of AID funtion, below members are added*/
	int lux_table_max;
	int *plux_table;
	struct illuminance_table gen_table[LUMINANCE_MAX];

	int brightness_level;
	int ldi_revision;
	/* HBM interpolation */
	struct illuminance_table hbm_interpolation_table[HBM_INTERPOLATION_STEP];
	char *hbm_payload;
	int hbm_brightness_level;
} __packed;


#define CANDELA_MAX_TABLE 64
#define RGB_COMPENSATION 24

static int candela_table[][2] = {
	{2, 0},
	{3, 1},
	{4, 2},
	{5, 3},
	{6, 4},
	{7, 5},
	{8, 6},
	{9, 7},
	{10, 8},
	{11, 9},
	{12, 10},
	{13, 11},
	{14, 12},
	{15, 13},
	{16, 14},
	{17, 15},
	{19, 16},
	{20, 17},
	{21, 18},
	{22, 19},
	{24, 20},
	{25, 21},
	{27, 22},
	{29, 23},
	{30, 24},
	{32, 25},
	{34, 26},
	{37, 27},
	{39, 28},
	{41, 29},
	{44, 30},
	{47, 31},
	{50, 32},
	{53, 33},
	{56, 34},
	{60, 35},
	{64, 36},
	{68, 37},
	{72, 38},
	{77, 39},
	{82, 40},
	{87, 41},
	{93, 42},
	{98, 43},
	{105, 44},
	{111, 45},
	{119, 46},
	{126, 47},
	{134, 48},
	{143, 49},
	{152, 50},
	{162, 51},
	{172, 52},
	{183, 53},
	{195, 54},
	{207, 55},
	{220, 56},
	{234, 57},
	{249, 58},
	{265, 59},
	{282, 60},
	{300, 61},
	{316, 62},
	{333, 63},
	{360, 64},
};

static int base_luminance_revA[][2] = {
	{2, 112},
	{3, 112},
	{4, 112},
	{5, 112},
	{6, 112},
	{7, 112},
	{8, 112},
	{9, 112},
	{10, 112},
	{11, 112},
	{12, 112},
	{13, 112},
	{14, 112},
	{15, 112},
	{16, 112},
	{17, 112},
	{19, 112},
	{20, 112},
	{21, 112},
	{22, 112},
	{24, 112},
	{25, 112},
	{27, 112},
	{29, 112},
	{30, 112},
	{32, 112},
	{34, 112},
	{37, 112},
	{39, 112},
	{41, 112},
	{44, 112},
	{47, 112},
	{50, 112},
	{53, 112},
	{56, 112},
	{60, 112},
	{64, 112},
	{68, 112},
	{72, 116},
	{77, 121},
	{82, 131},
	{87, 140},
	{93, 150},
	{98, 158},
	{105, 170},
	{111, 178},
	{119, 191},
	{126, 200},
	{134, 214},
	{143, 227},
	{152, 238},
	{162, 254},
	{172, 254},
	{183, 254},
	{195, 254},
	{207, 254},
	{220, 254},
	{234, 254},
	{249, 254},
	{265, 272},
	{282, 289},
	{300, 306},
	{316, 320},
	{333, 335},
	{360, 360},
};

static int gradation_offset_revA[][9] = {
/*      V255 V203 V151 V87 V51 V35 V23 V11 V3 */
	{0, 6, 9, 18, 20, 25, 30, 34, 35},
	{0, 6, 7, 13, 15, 18, 23, 27, 30},
	{0, 5, 6, 10, 13, 16, 19, 23, 25},
	{0, 4, 4, 9, 12, 13, 17, 20, 23},
	{0, 4, 4, 7, 10, 12, 16, 19, 21},
	{0, 4, 4, 6, 9, 11, 14, 18, 20},
	{0, 3, 2, 5, 7, 8, 11, 15, 17},
	{0, 3, 2, 5, 7, 7, 10, 14, 16},
	{0, 3, 2, 3, 4, 5, 8, 12, 14},
	{0, 3, 2, 3, 4, 5, 8, 12, 14},
	{0, 3, 2, 3, 4, 5, 8, 12, 15},
	{0, 3, 2, 3, 4, 5, 8, 12, 14},
	{0, 3, 2, 3, 4, 5, 8, 11, 14},
	{0, 3, 2, 3, 4, 5, 8, 11, 13},
	{0, 3, 2, 3, 4, 5, 8, 11, 13},
	{0, 3, 2, 3, 4, 5, 8, 11, 13},
	{0, 3, 2, 3, 4, 5, 8, 11, 13},
	{0, 3, 2, 3, 4, 5, 8, 11, 13},
	{0, 2, 2, 3, 3, 3, 6, 9, 11},
	{0, 2, 2, 2, 3, 3, 6, 9, 11},
	{0, 2, 2, 2, 3, 3, 6, 9, 11},
	{0, 2, 2, 2, 3, 3, 6, 9, 11},
	{0, 2, 2, 2, 2, 2, 5, 8, 10},
	{0, 2, 2, 2, 2, 2, 5, 7, 9},
	{0, 2, 2, 2, 2, 2, 5, 7, 9},
	{0, 2, 2, 2, 2, 2, 4, 7, 9},
	{0, 2, 2, 2, 2, 2, 4, 6, 8},
	{0, 2, 2, 2, 2, 2, 4, 6, 8},
	{0, 2, 2, 2, 2, 2, 4, 6, 8},
	{0, 2, 2, 2, 2, 2, 4, 6, 8},
	{0, 2, 2, 2, 2, 2, 4, 6, 8},
	{0, 2, 2, 2, 2, 2, 4, 6, 8},
	{0, 2, 2, 2, 2, 2, 4, 6, 8},
	{0, 2, 2, 2, 2, 2, 4, 5, 7},
	{0, 2, 2, 2, 2, 2, 4, 5, 7},
	{0, 2, 2, 2, 2, 2, 4, 5, 7},
	{0, 2, 2, 2, 2, 2, 4, 5, 7},
	{0, 2, 2, 2, 2, 2, 4, 5, 7},
	{0, 2, 2, 2, 1, 2, 3, 5, 7},
	{0, 3, 3, 2, 2, 2, 2, 4, 7},
	{0, 1, 1, 1, 1, 1, 2, 4, 6},
	{0, 1, 2, 1, 1, 1, 2, 3, 6},
	{0, 1, 2, 1, 0, 1, 3, 4, 7},
	{0, 1, 2, 2, 1, 2, 2, 3, 6},
	{0, 0, 3, 2, 1, 2, 3, 4, 7},
	{0, 0, 3, 1, 1, 1, 2, 3, 7},
	{0, 0, 2, 2, 1, 2, 2, 3, 6},
	{0, 2, 2, 2, 1, 1, 2, 3, 6},
	{0, 1, 2, 2, 0, 1, 2, 3, 5},
	{0, 1, 3, 2, 1, 1, 2, 3, 6},
	{0, 0, 2, 1, 1, 1, 1, 2, 4},
	{0, 1, 2, 1, 1, 1, 1, 3, 5},
	{0, 1, 2, 1, 0, 0, 0, 2, 4},
	{0, 1, 1, 1, 0, 0, 1, 2, 4},
	{0, 2, 2, 1, 0, 1, 1, 1, 3},
	{0, 1, 2, 1, 1, 1, 0, 1, 3},
	{0, 0, 2, 1, 1, 1, 0, 1, 3},
	{0, 0, 2, 1, 1, 1, 0, 1, 3},
	{0, 1, 2, 1, 1, 0, 0, 1, 3},
	{0, 0, 1, 1, 0, 0, 0, 2, 2},
	{0, 0, 1, 0, 0, 0, 0, 0, 2},
	{0, 0, 1, 0, 0, 0, 0, 0, 2},
	{0, 1, 0, 0, 0, 0, 0, 1, 2},
	{0, 1, 0, 0, -1, 0, -1, 0, 1},
	{0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static int rgb_offset_revA[][RGB_COMPENSATION] = {
/*	R255 G255 B255 R203 G203 B203 R151 G151 B151
	R87 G87 B87 R51 G51 B51 R35 G35 B35
	R23 G23 B23 R11 G11 B11
*/
	{-6, 0, 0, -3, 0, -1, -5, 2, -1, -17, 3, -7, -17, 11, -1, -13, 1, -6, -4, 3, -3, -7, 3, -6},
	{-8, -5, -4, -4, 0, -2, -3, 1, 0, -13, 4, -5, -16, 9, -2, -11, 6, -2, -8, 2, -6, -9, 3, -9},
	{-2, 1, 2, -2, 0, -1, -3, 1, -1, -9, 4, -3, -18, 5, -5, -15, 3, -6, -2, 9, 2, -12, 4, -9},
	{-4, -2, -1, -1, 1, 0, -2, 2, 0, -9, 2, -4, -18, 4, -7, -11, 7, -2, -6, 6, -2, -3, 7, -5},
	{-5, -4, -3, -1, 0, 0, -2, 2, 0, -6, 3, -2, -14, 6, -5, -16, 1, -7, -9, 5, -4, -8, 6, -8},
	{-2, -1, 0, -1, 0, 0, -1, 1, 0, -6, 4, -2, -16, 3, -7, -15, 2, -6, -7, 7, -1, -9, 2, -9},
	{-3, -2, -1, -1, 0, 0, 0, 2, 1, -5, 3, -2, -14, 4, -5, -9, 9, 0, -8, 7, -2, -7, 3, -8},
	{-1, 0, 1, -1, 0, 0, -1, 1, 0, -5, 3, -2, -13, 3, -5, -7, 11, 2, -8, 7, -2, -5, 3, -6},
	{0, 0, 0, 0, 0, 0, -1, 1, 0, -2, 5, 1, -10, 7, -2, -8, 9, 1, -9, 7, -2, -5, 4, -5},
	{-2, -1, 0, 0, 0, 0, -1, 1, 0, -2, 4, 0, -11, 6, -3, -9, 8, 0, -9, 7, -3, -4, 5, -5},
	{0, 0, 1, 0, 1, 1, 0, 1, 1, -3, 3, -1, -10, 6, -3, -8, 7, 0, -11, 6, -4, -9, 4, -7},
	{-1, -1, 0, 0, 1, 1, 0, 1, 1, -3, 3, -1, -10, 4, -4, -10, 6, -2, -11, 6, -3, -8, 2, -9},
	{-1, -1, 0, 0, 1, 1, 0, 1, 1, -3, 2, -2, -10, 4, -3, -10, 5, -3, -12, 5, -4, -5, 8, -2},
	{0, 0, 1, 0, 1, 1, 0, 1, 1, -3, 1, -2, -10, 4, -3, -9, 5, -3, -14, 3, -6, -5, 8, -2},
	{0, 0, 1, -1, 0, 0, 0, 0, 0, -3, 2, -1, -10, 3, -4, -10, 5, -3, -13, 4, -6, -3, 9, -3},
	{-1, -1, 0, 0, 1, 1, 0, 0, 0, -3, 2, -1, -10, 2, -5, -10, 4, -3, -12, 4, -6, -7, 7, -5},
	{0, 0, 1, 0, 1, 1, 0, 0, 0, -3, 1, -2, -10, 1, -5, -11, 3, -4, -13, 2, -7, -10, 6, -7},
	{-1, -1, 0, 0, 1, 1, 0, 0, 0, -2, 1, -1, -10, 1, -5, -11, 2, -5, -16, 1, -9, -9, 6, -6},
	{-1, -1, 0, 1, 1, 1, -1, 0, 0, -2, 1, -1, -6, 4, -2, -6, 7, -1, -8, 4, -4, -8, 7, -5},
	{-1, -1, 0, 1, 1, 1, -1, -1, -1, -1, 3, 1, -7, 2, -3, -6, 7, -1, -10, 3, -5, -10, 6, -6},
	{-1, -1, 0, 1, 1, 1, -1, -1, -1, -1, 3, 1, -7, 1, -3, -5, 6, -1, -12, 2, -7, -12, 5, -7},
	{-1, -1, 0, 1, 1, 1, -1, -1, -1, -1, 2, 0, -8, 1, -3, -5, 6, -1, -11, 1, -7, -13, 5, -7},
	{0, 0, 1, 1, 1, 1, -1, -1, -1, -1, 2, 0, -4, 4, -1, -5, 6, 0, -9, 3, -5, -12, 4, -9},
	{-1, -1, 0, 1, 1, 1, -1, -1, -1, 0, 2, 1, -4, 4, -1, -6, 4, -2, -9, 3, -5, -6, 10, -3},
	{0, 0, 1, 1, 1, 1, -1, -1, -1, 0, 2, 1, -4, 3, -2, -6, 4, -1, -8, 3, -5, -10, 8, -6},
	{0, 0, 1, 1, 1, 1, -1, -1, -1, 0, 2, 1, -4, 3, -2, -5, 4, -1, -4, 7, -1, -12, 4, -9},
	{0, 0, 1, 1, 1, 1, -1, -1, -1, -1, 1, 0, -4, 2, -2, -5, 4, -1, -3, 7, -1, -10, 8, -7},
	{0, -1, 0, 1, 2, 2, -1, -1, -1, 0, 1, 0, -4, 2, -2, -7, 3, -2, -2, 7, -1, -9, 8, -6},
	{-1, -1, 0, 1, 1, 1, -1, -1, -1, 0, 1, 0, -4, 2, -2, -7, 2, -3, -4, 6, -2, -9, 8, -6},
	{0, 0, 1, 1, 1, 1, -1, -1, -1, 0, 1, 0, -3, 2, -1, -7, 2, -3, -5, 4, -4, -11, 7, -7},
	{-1, -1, 0, 2, 2, 2, -1, -1, -1, -1, 0, -1, -3, 2, -1, -6, 2, -3, -4, 4, -3, -14, 5, -11},
	{0, -1, 0, 1, 2, 2, 0, 0, 0, -1, 0, -1, -4, 0, -3, -6, 2, -2, -6, 3, -5, -15, 4, -11},
	{0, -1, 0, 0, 1, 1, 0, 0, 0, -1, 0, -1, -4, 0, -3, -5, 2, -2, -5, 3, -4, -15, 4, -12},
	{0, -1, 0, 0, 1, 1, 0, 0, 0, -1, 0, -1, -4, 0, -3, -5, 1, -2, -5, 3, -4, -9, 9, -5},
	{0, -1, 0, 0, 1, 1, 0, 0, 0, -1, 0, -1, -3, 0, -2, -6, 1, -3, -5, 2, -5, -9, 9, -5},
	{0, -1, 0, 0, 1, 1, 0, 0, 0, -1, 0, -1, -2, 0, -2, -5, 1, -2, -8, 0, -7, -11, 7, -7},
	{0, -1, 0, 0, 1, 1, 0, 0, 0, 0, -1, -1, -5, -1, -4, -5, 1, -2, -7, 0, -7, -12, 6, -7},
	{0, -1, 0, 0, 1, 1, 0, 0, 0, 0, -1, -1, -4, -1, -3, -6, 0, -4, -7, 0, -6, -16, 3, -11},
	{0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, -1, -2, 0, -1, -7, -1, -4, -7, -1, -7, -17, 2, -12},
	{1, 0, 1, -1, 0, 0, 0, 0, 0, 0, 0, 0, -4, -1, -3, -3, 2, -2, -1, 4, 0, -17, 2, -13},
	{-1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1, -1, -2, 3, 0, 1, 4, 0, -19, 0, -14},
	{-1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, -1, 0, -1, -6, 0, -3, -2, 1, -3, -13, 6, -8},
	{-1, -1, 0, 0, 1, 1, 1, 0, 1, -1, 0, -1, -1, 1, 0, -5, -1, -5, -5, -1, -4, -16, 3, -11},
	{-1, 0, 0, 0, 0, 1, 1, 0, 0, -1, 0, -1, -1, 1, 0, -5, -1, -4, -3, 1, -3, -8, 9, -4},
	{-2, -1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, -3, -1, -3, -4, 0, -2, -7, -1, -6, -10, 7, -6},
	{-1, -1, -1, 0, 0, 0, 0, 0, 1, 1, 1, 1, -1, 0, -1, -4, 0, -2, -5, -1, -5, -13, 5, -8},
	{-1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, -1, 0, -2, -3, 1, -1, -1, 3, -1, -11, 5, -7},
	{-1, 0, -1, 0, -1, -1, 2, 1, 2, 0, 0, 0, -1, 1, 0, -3, 0, -2, -7, -3, -6, -9, 6, -6},
	{-2, -1, -1, 1, -1, 0, 1, 2, 2, 0, 0, 1, 0, 1, 0, -2, 0, -1, -4, 0, -3, -11, 4, -8},
	{-2, -1, -1, 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 0, 0, -2, 0, -2, -8, -3, -7, -12, 2, -8},
	{-1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, -2, -1, -1, -2, 1, -1, -1, 3, -1, -9, 3, -6},
	{-1, 0, -1, 0, 0, 0, 1, 0, 1, 1, 1, 1, -1, 0, -1, -2, 0, -1, -5, 0, -4, -12, 1, -8},
	{-1, 0, -1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, -1, 1, 0, -1, 2, -1, -12, -1, -9},
	{-1, 0, -1, 0, 0, 0, 2, 1, 2, 0, 0, 0, 0, 1, 0, -1, 0, -1, -6, -3, -6, -6, 3, -3},
	{-1, 0, -1, 0, 0, 0, 1, 1, 2, 1, 0, 0, 0, 1, 0, -2, -1, -2, -5, -1, -3, -3, 6, -1},
	{-1, 0, -1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, -1, -2, 0, -1, -3, 0, -2, -5, 5, -2},
	{-1, 0, -1, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, -1, -1, 0, -1, -2, 0, -2, -7, 2, -3},
	{-1, 0, -1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, -2, -1, -1, -1, 0, -2, -7, 1, -4},
	{0, 0, -1, -1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, -1, 0, 0, 0, -2, 0, -2, -7, 0, -5},
	{-1, 0, 0, -1, 0, 0, 1, 0, 1, 2, 0, 1, 0, 0, 0, -1, 0, 0, -1, 0, -1, -5, 0, -4},
	{-1, 0, -1, 0, 0, 1, 1, 0, 0, 1, 0, 1, -1, 0, -1, 0, 0, 0, -2, 0, -2, -3, 1, -3},
	{-1, 0, -1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, -3, 0, -2, -2, 0, -1},
	{-1, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, -1, 0, -1, 0, 0, 0, -1, 0, -1, -2, 0, 0},
	{-1, 0, 0, -1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, -2, 0, -1, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

#define GAMMA_INDEX_MAX 256


static const int candela_coeff_2p15[GAMMA_INDEX_MAX] = {
	0, 28, 125, 298, 553, 894, 1323, 1843,
	2456, 3164, 3968, 4871, 5873, 6976, 8180, 9488,
	10901, 12418, 14042, 15773, 17612, 19560, 21618, 23786,
	26065, 28456, 30959, 33576, 36307, 39152, 42112, 45188,
	48381, 51690, 55116, 58660, 62323, 66105, 70006, 74027,
	78168, 82430, 86813, 91318, 95945, 100694, 105567, 110563,
	115682, 120926, 126294, 131788, 137406, 143150, 149020, 155017,
	161140, 167390, 173768, 180273, 186906, 193668, 200558, 207578,
	214726, 222005, 229413, 236951, 244620, 252420, 260351, 268413,
	276607, 284933, 293391, 301981, 310705, 319561, 328550, 337673,
	346930, 356321, 365846, 375506, 385300, 395229, 405294, 415494,
	425830, 436302, 446910, 457654, 468535, 479553, 490708, 502001,
	513431, 524998, 536704, 548547, 560530, 572650, 584910, 597308,
	609846, 622523, 635340, 648296, 661393, 674630, 688007, 701525,
	715183, 728983, 742923, 757005, 771229, 785594, 800101, 814750,
	829542, 844476, 859552, 874771, 890133, 905639, 921288, 937080,
	953015, 969095, 985319, 1001686, 1018199, 1034855, 1051656, 1068602,
	1085693, 1102929, 1120311, 1137838, 1155510, 1173328, 1191293, 1209403,
	1227659, 1246062, 1264612, 1283308, 1302151, 1321140, 1340277, 1359562,
	1378994, 1398573, 1418300, 1438175, 1458198, 1478369, 1498688, 1519156,
	1539772, 1560537, 1581451, 1602514, 1623726, 1645087, 1666598, 1688258,
	1710068, 1732028, 1754138, 1776398, 1798807, 1821368, 1844078, 1866940,
	1889952, 1913115, 1936429, 1959894, 1983510, 2007278, 2031197, 2055268,
	2079490, 2103864, 2128391, 2153069, 2177900, 2202883, 2228018, 2253306,
	2278747, 2304341, 2330088, 2355987, 2382040, 2408246, 2434606, 2461119,
	2487786, 2514607, 2541581, 2568710, 2595992, 2623429, 2651020, 2678766,
	2706666, 2734721, 2762931, 2791295, 2819815, 2848490, 2877320, 2906305,
	2935446, 2964742, 2994194, 3023802, 3053566, 3083485, 3113561, 3143793,
	3174181, 3204726, 3235427, 3266285, 3297300, 3328471, 3359800, 3391285,
	3422927, 3454727, 3486684, 3518799, 3551071, 3583500, 3616088, 3648833,
	3681736, 3714798, 3748017, 3781395, 3814931, 3848625, 3882478, 3916489,
	3950660, 3984989, 4019477, 4054124, 4088930, 4123895, 4159020, 4194304,
};


static const int candela_coeff_2p2[GAMMA_INDEX_MAX] = {
	0, 21, 98, 239, 450, 735, 1097, 1540,
	2066, 2677, 3375, 4162, 5040, 6011, 7075, 8235,
	9492, 10846, 12299, 13853, 15507, 17264, 19125, 21090,
	23160, 25336, 27619, 30010, 32510, 35119, 37839, 40669,
	43612, 46666, 49834, 53115, 56512, 60023, 63650, 67393,
	71253, 75231, 79327, 83541, 87875, 92329, 96903, 101598,
	106415, 111353, 116414, 121598, 126905, 132336, 137892, 143572,
	149377, 155309, 161366, 167551, 173862, 180301, 186867, 193562,
	200386, 207339, 214421, 221634, 228977, 236450, 244055, 251791,
	259659, 267659, 275792, 284058, 292457, 300989, 309656, 318457,
	327393, 336464, 345670, 355012, 364490, 374105, 383856, 393744,
	403769, 413932, 424234, 434673, 445251, 455968, 466824, 477819,
	488954, 500230, 511645, 523201, 534899, 546737, 558717, 570839,
	583102, 595509, 608057, 620749, 633583, 646561, 659683, 672949,
	686359, 699913, 713612, 727456, 741445, 755580, 769860, 784287,
	798859, 813578, 828444, 843457, 858616, 873924, 889379, 904981,
	920732, 936632, 952680, 968876, 985222, 1001717, 1018362, 1035156,
	1052100, 1069195, 1086439, 1103835, 1121381, 1139078, 1156927, 1174927,
	1193078, 1211382, 1229838, 1248446, 1267206, 1286119, 1305186, 1324405,
	1343778, 1363304, 1382984, 1402818, 1422806, 1442948, 1463245, 1483697,
	1504304, 1525065, 1545982, 1567055, 1588283, 1609668, 1631208, 1652904,
	1674757, 1696767, 1718934, 1741257, 1763738, 1786376, 1809172, 1832125,
	1855237, 1878506, 1901934, 1925521, 1949266, 1973169, 1997232, 2021454,
	2045836, 2070376, 2095077, 2119937, 2144958, 2170139, 2195480, 2220981,
	2246644, 2272467, 2298451, 2324597, 2350904, 2377372, 2404003, 2430795,
	2457749, 2484865, 2512144, 2539585, 2567189, 2594956, 2622886, 2650979,
	2679235, 2707655, 2736239, 2764986, 2793897, 2822972, 2852212, 2881616,
	2911185, 2940918, 2970816, 3000880, 3031108, 3061502, 3092061, 3122786,
	3153677, 3184733, 3215956, 3247345, 3278900, 3310622, 3342510, 3374566,
	3406788, 3439177, 3471734, 3504458, 3537349, 3570408, 3603635, 3637030,
	3670593, 3704324, 3738224, 3772292, 3806529, 3840934, 3875509, 3910253,
	3945165, 3980247, 4015499, 4050920, 4086511, 4122272, 4158203, 4194304,
};


static const int curve_2p2_360[GAMMA_INDEX_MAX] = {
	0, 7666, 35224, 85949, 161847, 264428, 394918, 554357,
	743656, 963624, 1214994, 1498435, 1814566, 2163961, 2547157, 2964659,
	3416945, 3904466, 4427652, 4986915, 5582645, 6215219, 6884999, 7592333,
	8337556, 9120991, 9942953, 10803744, 11703659, 12642984, 13621997, 14640966,
	15700156, 16799822, 17940214, 19121577, 20344148, 21608161, 22913842, 24261416,
	25651100, 27083107, 28557649, 30074929, 31635150, 33238509, 34885202, 36575418,
	38309345, 40087169, 41909069, 43775226, 45685814, 47641006, 49640972, 51685881,
	53775898, 55911184, 58091902, 60318208, 62590261, 64908212, 67272215, 69682419,
	72138972, 74642022, 77191711, 79788183, 82431579, 85122037, 87859696, 90644692,
	93477158, 96357229, 99285035, 102260706, 105284372, 108356159, 111476194, 114644602,
	117861504, 121127025, 124441284, 127804402, 131216496, 134677686, 138188086, 141747812,
	145356979, 149015699, 152724084, 156482246, 160290295, 164148340, 168056489, 172014850,
	176023528, 180082630, 184192259, 188352521, 192563517, 196825349, 201138120, 205501929,
	209916876, 214383060, 218900580, 223469532, 228090013, 232762120, 237485947, 242261589,
	247089140, 251968693, 256900341, 261884176, 266920288, 272008769, 277149707, 282343194,
	287589317, 292888165, 298239825, 303644384, 309101929, 314612546, 320176319, 325793334,
	331463675, 337187426, 342964669, 348795488, 354679965, 360618181, 366610218, 372656155,
	378756075, 384910055, 391118176, 397380516, 403697153, 410068166, 416493632, 422973628,
	429508231, 436097516, 442741560, 449440438, 456194224, 463002994, 469866820, 476785778,
	483759940, 490789380, 497874169, 505014380, 512210085, 519461355, 526768261, 534130874,
	541549264, 549023502, 556553657, 564139797, 571781993, 579480313, 587234824, 595045596,
	602912695, 610836188, 618816144, 626852629, 634945708, 643095448, 651301914, 659565173,
	667885289, 676262327, 684696351, 693187426, 701735616, 710340985, 719003595, 727723510,
	736500793, 745335507, 754227713, 763177473, 772184850, 781249905, 790372699, 799553293,
	808791748, 818088124, 827442482, 836854881, 846325381, 855854042, 865440922, 875086082,
	884789579, 894551472, 904371820, 914250680, 924188110, 934184168, 944238911, 954352397,
	964524681, 974755822, 985045875, 995394897, 1005802944, 1016270071, 1026796335, 1037381790,
	1048026492, 1058730496, 1069493857, 1080316629, 1091198867, 1102140624, 1113141956, 1124202915,
	1135323556, 1146503931, 1157744095, 1169044100, 1180403998, 1191823843, 1203303688, 1214843583,
	1226443583, 1238103738, 1249824100, 1261604721, 1273445652, 1285346945, 1297308650, 1309330819,
	1321413502, 1333556750, 1345760613, 1358025142, 1370350386, 1382736395, 1395183219, 1407690908,
	1420259511, 1432889077, 1445579655, 1458331295, 1471144045, 1484017953, 1496953069, 1509949440,
};


static char max_lux_table[GAMMA_SET_MAX];

static char max_lux_table2[GAMMA_SET_MAX] = {
	0x80, 0x80,	/* V11[7:0],  V23[7:0]	Red */
	0x80, 0x80,	/* V35[7:0],  V51[7:0]	Red */
	0x80, 0x80,	/* V87[7:0],  V151[7:0]	Red */
	0x80, 0x00,	/* V203[7:0], V255[7:0]	Red */
	0x80, 0x80,	/* V3[7:0], V255[8]:V0[6:0]Red */
	0x00,		/* VT[3:0]		Red */
	0x80, 0x80,	/* V11[7:0],  V23[7:0]	Green */
	0x80, 0x80,	/* V35[7:0],  V51[7:0]	Green */
	0x80, 0x80,	/* V87[7:0],  V151[7:0]	Green */
	0x80, 0x00,	/* V203[7:0], V255[7:0]	Green */
	0x80, 0x80,	/* V3[7:0], V255[8]:V0[6:0]Green */
	0x00,		/* VT[3:0]		Green */
	0x80, 0x80,	/* V11[7:0],  V23[7:0]	Blue */
	0x80, 0x80,	/* V35[7:0],  V51[7:0]	Blue */
	0x80, 0x80,	/* V87[7:0],  V151[7:0]	Blue */
	0x80, 0x00,	/* V203[7:0], V255[7:0]	Blue */
	0x80, 0x80,	/* V3[7:0], V255[8]:V0[6:0]Blue */
	0x00		/* VT[3:0]		Blue */
};

/*
*	To support different center cell gamma setting
*/
static char V255_300CD_R_MSB;
static char V255_300CD_R_LSB;

static char V255_300CD_G_MSB;
static char V255_300CD_G_LSB;

static char V255_300CD_B_MSB;
static char V255_300CD_B_LSB;

static char V203_300CD_R;
static char V203_300CD_G;
static char V203_300CD_B;

static char V151_300CD_R;
static char V151_300CD_G;
static char V151_300CD_B;

static char V87_300CD_R;
static char V87_300CD_G;
static char V87_300CD_B;

static char V51_300CD_R;
static char V51_300CD_G;
static char V51_300CD_B;

static char V35_300CD_R;
static char V35_300CD_G;
static char V35_300CD_B;

static char V23_300CD_R;
static char V23_300CD_G;
static char V23_300CD_B;

static char V11_300CD_R;
static char V11_300CD_G;
static char V11_300CD_B;

static char V3_300CD_R;
static char V3_300CD_G;
static char V3_300CD_B;

static char VT_300CD_R;
static char VT_300CD_G;
static char VT_300CD_B;

static int char_to_int(char data1)
{
	int cal_data;

	if (data1 & 0x80) {
		cal_data = data1 & 0x7F;
		cal_data *= -1;
	} else
		cal_data = data1;

	return cal_data;
}

static int char_to_int_v255(char data1, char data2)
{
	int cal_data;

	if (data1)
		cal_data = data2 * -1;
	else
		cal_data = data2;

	return cal_data;
}


#define v255_coefficient 129
#define v255_denominator 640
static int v255_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;
	int v255_value;

	v255_value = (V255_300CD_R_MSB << 8) | (V255_300CD_R_LSB);
	LSB = char_to_int_v255(pSmart->MTP.R_OFFSET.OFFSET_255_MSB,
				pSmart->MTP.R_OFFSET.OFFSET_255_LSB);
	add_mtp = LSB + v255_value;
	result_2 = (v255_coefficient+add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v255_denominator);
	result_3 = ((ANA34801_VREG0_REF_L-ANA34801_VREG0_REF_H) * result_2) >> BIT_SHIFT;
	result_4 = ANA34801_VREG0_REF_L - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_255 = result_4;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_0 = ANA34801_VREG0_REF_L;

	v255_value = (V255_300CD_G_MSB << 8) | (V255_300CD_G_LSB);
	LSB = char_to_int_v255(pSmart->MTP.G_OFFSET.OFFSET_255_MSB,
				pSmart->MTP.G_OFFSET.OFFSET_255_LSB);
	add_mtp = LSB + v255_value;
	result_2 = (v255_coefficient+add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v255_denominator);
	result_3 = ((ANA34801_VREG0_REF_L-ANA34801_VREG0_REF_H) * result_2) >> BIT_SHIFT;
	result_4 = ANA34801_VREG0_REF_L - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_255 = result_4;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_0 = ANA34801_VREG0_REF_L;

	v255_value = (V255_300CD_B_MSB << 8) | (V255_300CD_B_LSB);
	LSB = char_to_int_v255(pSmart->MTP.B_OFFSET.OFFSET_255_MSB,
				pSmart->MTP.B_OFFSET.OFFSET_255_LSB);
	add_mtp = LSB + v255_value;
	result_2 = (v255_coefficient+add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v255_denominator);
	result_3 = ((ANA34801_VREG0_REF_L-ANA34801_VREG0_REF_H) * result_2) >> BIT_SHIFT;
	result_4 = ANA34801_VREG0_REF_L - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_255 = result_4;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_0 = ANA34801_VREG0_REF_L;


	return 0;
}

static void v255_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = ANA34801_VREG0_REF_L -
		(pSmart->GRAY.TABLE[index[V255_INDEX]].R_Gray);
	result_2 = result_1 * v255_denominator;
	ana38401_do_div(result_2, (ANA34801_VREG0_REF_L-ANA34801_VREG0_REF_H));
	result_3 = result_2  - v255_coefficient;
	str[0] = (result_3 & 0xff00) >> 8;
	str[1] = result_3 & 0xff;

	result_1 = ANA34801_VREG0_REF_L -
		(pSmart->GRAY.TABLE[index[V255_INDEX]].G_Gray);
	result_2 = result_1 * v255_denominator;
	ana38401_do_div(result_2, (ANA34801_VREG0_REF_L-ANA34801_VREG0_REF_H));
	result_3 = result_2  - v255_coefficient;
	str[2] = (result_3 & 0xff00) >> 8;
	str[3] = result_3 & 0xff;

	result_1 = ANA34801_VREG0_REF_L -
			(pSmart->GRAY.TABLE[index[V255_INDEX]].B_Gray);
	result_2 = result_1 * v255_denominator;
	ana38401_do_div(result_2, (ANA34801_VREG0_REF_L-ANA34801_VREG0_REF_H));
	result_3 = result_2  - v255_coefficient;
	str[4] = (result_3 & 0xff00) >> 8;
	str[5] = result_3 & 0xff;

}

static int vt_coefficient[] = {
	0, 12, 24, 36, 48,
	60, 72, 84, 96, 108,
	138, 148, 158, 168,
	178, 186,
};
#define vt_denominator 600
static int vt_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_2, result_3, result_4;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_1);
	result_2 = vt_coefficient[LSB] << BIT_SHIFT;
	ana38401_do_div(result_2, vt_denominator);
	result_3 = (ANA34801_VREG0_REF_L * result_2) >> BIT_SHIFT;
	result_4 = ANA34801_VREG0_REF_L - result_3;
	pSmart->GRAY.VT_TABLE.R_Gray = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_1);
	result_2 = vt_coefficient[LSB] << BIT_SHIFT;
	ana38401_do_div(result_2, vt_denominator);
	result_3 = (ANA34801_VREG0_REF_L * result_2) >> BIT_SHIFT;
	result_4 = ANA34801_VREG0_REF_L - result_3;
	pSmart->GRAY.VT_TABLE.G_Gray = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_1);
	result_2 = vt_coefficient[LSB] << BIT_SHIFT;
	ana38401_do_div(result_2, vt_denominator);
	result_3 = (ANA34801_VREG0_REF_L * result_2) >> BIT_SHIFT;
	result_4 = ANA34801_VREG0_REF_L - result_3;
	pSmart->GRAY.VT_TABLE.B_Gray = result_4;


	return 0;

}

static void vt_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	str[30] = VT_300CD_R;
	str[31] = VT_300CD_G;
	str[32] = VT_300CD_B;
}

#define v203_coefficient 64
#define v203_denominator 320
static int v203_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_203);
	add_mtp = LSB + V203_300CD_R;
	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
				- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_255);
	result_2 = (v203_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v203_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.R_Gray) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_203 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_203);
	add_mtp = LSB + V203_300CD_G;
	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
				- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_255);
	result_2 = (v203_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v203_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.G_Gray) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_203 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_203);
	add_mtp = LSB + V203_300CD_B;
	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
				- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_255);
	result_2 = (v203_coefficient+add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v203_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.B_Gray) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_203 = result_4;


	return 0;

}

static void v203_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V203_INDEX]].R_Gray);
	result_2 = result_1 * v203_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V255_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[6] = (result_2  - v203_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V203_INDEX]].G_Gray);
	result_2 = result_1 * v203_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V255_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[7] = (result_2  - v203_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V203_INDEX]].B_Gray);
	result_2 = result_1 * v203_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V255_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[8] = (result_2  - v203_coefficient) & 0xff;

}

#define v151_coefficient 64
#define v151_denominator 320
static int v151_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_151);
	add_mtp = LSB + V151_300CD_R;
	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_203);
	result_2 = (v151_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v151_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.R_Gray) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_151 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_151);
	add_mtp = LSB + V151_300CD_G;
	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_203);
	result_2 = (v151_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v151_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.G_Gray) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_151 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_151);
	add_mtp = LSB + V151_300CD_B;
	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_203);
	result_2 = (v151_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v151_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.B_Gray) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_151 = result_4;


	return 0;

}

static void v151_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V151_INDEX]].R_Gray);
	result_2 = result_1 * v151_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V203_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[9] = (result_2  - v151_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V151_INDEX]].G_Gray);
	result_2 = result_1 * v151_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V203_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[10] = (result_2  - v151_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V151_INDEX]].B_Gray);
	result_2 = result_1 * v151_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V203_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[11] = (result_2  - v151_coefficient) & 0xff;
}

#define v87_coefficient 64
#define v87_denominator 320
static int v87_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_87);
	add_mtp = LSB + V87_300CD_R;
	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_151);
	result_2 = (v87_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v87_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.R_Gray) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_87 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_87);
	add_mtp = LSB + V87_300CD_G;
	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_151);
	result_2 = (v87_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v87_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.G_Gray) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_87 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_87);
	add_mtp = LSB + V87_300CD_B;
	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_151);
	result_2 = (v87_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v87_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.B_Gray) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_87 = result_4;


	return 0;
}

static void v87_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V87_INDEX]].R_Gray);
	result_2 = result_1 * v87_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V151_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[12] = (result_2  - v87_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V87_INDEX]].G_Gray);
	result_2 = result_1 * v87_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V151_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[13] = (result_2  - v87_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V87_INDEX]].B_Gray);
	result_2 = result_1 * v87_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V151_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[14] = (result_2  - v87_coefficient) & 0xff;
}

#define v51_coefficient 64
#define v51_denominator 320
static int v51_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_51);
	add_mtp = LSB + V51_300CD_R;
	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_87);
	result_2 = (v51_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v51_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.R_Gray) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_51 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_51);
	add_mtp = LSB + V51_300CD_G;
	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_87);
	result_2 = (v51_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v51_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.G_Gray) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_51 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_51);
	add_mtp = LSB + V51_300CD_B;
	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_87);
	result_2 = (v51_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v51_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.B_Gray) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_51 = result_4;


	return 0;
}

static void v51_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V51_INDEX]].R_Gray);
	result_2 = result_1 * v51_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V87_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[15] = (result_2  - v51_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V51_INDEX]].G_Gray);
	result_2 = result_1 * v51_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V87_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[16] = (result_2  - v51_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V51_INDEX]].B_Gray);
	result_2 = result_1 * v51_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V87_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[17] = (result_2  - v51_coefficient) & 0xff;
}

#define v35_coefficient 64
#define v35_denominator 320
static int v35_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_35);
	add_mtp = LSB + V35_300CD_R;
	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_51);
	result_2 = (v35_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v35_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.R_Gray) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_35 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_35);
	add_mtp = LSB + V35_300CD_G;
	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_51);
	result_2 = (v35_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v35_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.G_Gray) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_35 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_35);
	add_mtp = LSB + V35_300CD_B;
	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_51);
	result_2 = (v35_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v35_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.B_Gray) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_35 = result_4;


	return 0;
}

static void v35_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V35_INDEX]].R_Gray);
	result_2 = result_1 * v35_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V51_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[18] = (result_2  - v35_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V35_INDEX]].G_Gray);
	result_2 = result_1 * v35_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V51_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[19] = (result_2  - v35_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V35_INDEX]].B_Gray);
	result_2 = result_1 * v35_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V51_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[20] = (result_2  - v35_coefficient) & 0xff;

}

#define v23_coefficient 64
#define v23_denominator 320
static int v23_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_23);
	add_mtp = LSB + V23_300CD_R;
	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_35);
	result_2 = (v23_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v23_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.R_Gray) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_23 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_23);
	add_mtp = LSB + V23_300CD_G;
	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_35);
	result_2 = (v23_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v23_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.G_Gray) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_23 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_23);
	add_mtp = LSB + V23_300CD_B;
	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_35);
	result_2 = (v23_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v23_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.B_Gray) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_23 = result_4;


	return 0;
}

static void v23_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V23_INDEX]].R_Gray);
	result_2 = result_1 * v23_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V35_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[21] = (result_2  - v23_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V23_INDEX]].G_Gray);
	result_2 = result_1 * v23_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V35_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[22] = (result_2  - v23_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V23_INDEX]].B_Gray);
	result_2 = result_1 * v23_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V35_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[23] = (result_2  - v23_coefficient) & 0xff;
}

#define v11_coefficient 64
#define v11_denominator 320
static int v11_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	unsigned long long add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_11);
	add_mtp = LSB + V11_300CD_R;
	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_23);
	result_2 = (v11_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v11_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.R_Gray) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_11 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_11);
	add_mtp = LSB + V11_300CD_G;
	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_23);
	result_2 = (v11_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v11_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.G_Gray) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_11 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_11);
	add_mtp = LSB + V11_300CD_B;
	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_23);
	result_2 = (v11_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v11_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (pSmart->GRAY.VT_TABLE.B_Gray) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_11 = result_4;


	return 0;
}

static void v11_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V11_INDEX]].R_Gray);
	result_2 = result_1 * v11_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.R_Gray)
			- (pSmart->GRAY.TABLE[index[V23_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[24] = (result_2  - v11_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V11_INDEX]].G_Gray);
	result_2 = result_1 * v11_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.G_Gray)
			- (pSmart->GRAY.TABLE[index[V23_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[25] = (result_2  - v11_coefficient) & 0xff;

	result_1 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V11_INDEX]].B_Gray);
	result_2 = result_1 * v11_denominator;
	result_3 = (pSmart->GRAY.VT_TABLE.B_Gray)
			- (pSmart->GRAY.TABLE[index[V23_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[26] = (result_2  - v11_coefficient) & 0xff;

}

#define v3_coefficient 64
#define v3_denominator 320
static int v3_adjustment(struct SMART_DIM *pSmart)
{
	unsigned long long result_1, result_2, result_3, result_4;
	int add_mtp;
	int LSB;

	LSB = char_to_int(pSmart->MTP.R_OFFSET.OFFSET_3);
	add_mtp = LSB + V3_300CD_R;
	result_1 = (ANA34801_VREG0_REF_L)
			- (pSmart->RGB_OUTPUT.R_VOLTAGE.level_11);
	result_2 = (v3_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v3_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (ANA34801_VREG0_REF_L) - result_3;
	pSmart->RGB_OUTPUT.R_VOLTAGE.level_3 = result_4;

	LSB = char_to_int(pSmart->MTP.G_OFFSET.OFFSET_3);
	add_mtp = LSB + V3_300CD_G;
	result_1 = (ANA34801_VREG0_REF_L)
			- (pSmart->RGB_OUTPUT.G_VOLTAGE.level_11);
	result_2 = (v3_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v3_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (ANA34801_VREG0_REF_L) - result_3;
	pSmart->RGB_OUTPUT.G_VOLTAGE.level_3 = result_4;

	LSB = char_to_int(pSmart->MTP.B_OFFSET.OFFSET_3);
	add_mtp = LSB + V3_300CD_B;
	result_1 = (ANA34801_VREG0_REF_L)
			- (pSmart->RGB_OUTPUT.B_VOLTAGE.level_11);
	result_2 = (v3_coefficient + add_mtp) << BIT_SHIFT;
	ana38401_do_div(result_2, v3_denominator);
	result_3 = (result_1 * result_2) >> BIT_SHIFT;
	result_4 = (ANA34801_VREG0_REF_L) - result_3;
	pSmart->RGB_OUTPUT.B_VOLTAGE.level_3 = result_4;


	return 0;
}

static void v3_hexa(int *index, struct SMART_DIM *pSmart, char *str)
{
	unsigned long long result_1, result_2, result_3;

	result_1 = (ANA34801_VREG0_REF_L)
			- (pSmart->GRAY.TABLE[index[V3_INDEX]].R_Gray);
	result_2 = result_1 * v3_denominator;
	result_3 = (ANA34801_VREG0_REF_L)
			- (pSmart->GRAY.TABLE[index[V11_INDEX]].R_Gray);
	ana38401_do_div(result_2, result_3);
	str[27] = (result_2  - v3_coefficient) & 0xff;

	result_1 = (ANA34801_VREG0_REF_L)
			- (pSmart->GRAY.TABLE[index[V3_INDEX]].G_Gray);
	result_2 = result_1 * v3_denominator;
	result_3 = (ANA34801_VREG0_REF_L)
			- (pSmart->GRAY.TABLE[index[V11_INDEX]].G_Gray);
	ana38401_do_div(result_2, result_3);
	str[28] = (result_2  - v3_coefficient) & 0xff;

	result_1 = (ANA34801_VREG0_REF_L)
			- (pSmart->GRAY.TABLE[index[V3_INDEX]].B_Gray);
	result_2 = result_1 * v3_denominator;
	result_3 = (ANA34801_VREG0_REF_L)
			- (pSmart->GRAY.TABLE[index[V11_INDEX]].B_Gray);
	ana38401_do_div(result_2, result_3);
	str[29] = (result_2  - v3_coefficient) & 0xff;
}

/*V0,V1,V3,V11,V23,V35,V51,V87,V151,V203,V255*/
static int INFLECTION_VOLTAGE_ARRAY[ARRAY_MAX] = {0, 1, 3, 11, 23, 35, 51, 87, 151, 203, 255};

#define V0toV3_Coefficient 2
#define V0toV3_Multiple 1
#define V0toV3_denominator 3

#define V3toV11_Coefficient 7
#define V3toV11_Multiple 1
#define V3toV11_denominator 8

#define V11toV23_Coefficient 11
#define V11toV23_Multiple 1
#define V11toV23_denominator 12

#define V23toV35_Coefficient 11
#define V23toV35_Multiple 1
#define V23toV35_denominator 12

#define V35toV51_Coefficient 15
#define V35toV51_Multiple 1
#define V35toV51_denominator 16

#define V51toV87_Coefficient 35
#define V51toV87_Multiple 1
#define V51toV87_denominator 36

#define V87toV151_Coefficient 63
#define V87toV151_Multiple 1
#define V87toV151_denominator 64

#define V151toV203_Coefficient 51
#define V151toV203_Multiple 1
#define V151toV203_denominator 52

#define V203toV255_Coefficient 51
#define V203toV255_Multiple 1
#define V203toV255_denominator 52

static int cal_gray_scale_linear(int up, int low, int coeff,
int mul, int deno, int cnt)
{
	unsigned long long result_1, result_2, result_3, result_4;

	result_1 = up - low;
	result_2 = (result_1 * (coeff - (cnt * mul))) << BIT_SHIFT;
	ana38401_do_div(result_2, deno);
	result_3 = result_2 >> BIT_SHIFT;
	result_4 = low + result_3;

	return (int)result_4;
}

static int generate_gray_scale(struct SMART_DIM *pSmart)
{
	int cnt = 0, cal_cnt = 0;
	int array_index = 0;
	struct GRAY_VOLTAGE *ptable = (struct GRAY_VOLTAGE *)
						(&(pSmart->GRAY.TABLE));

	for (cnt = 0; cnt < ARRAY_MAX; cnt++) {
		pSmart->GRAY.TABLE[INFLECTION_VOLTAGE_ARRAY[cnt]].R_Gray =
			((int *)&(pSmart->RGB_OUTPUT.R_VOLTAGE))[cnt];

		pSmart->GRAY.TABLE[INFLECTION_VOLTAGE_ARRAY[cnt]].G_Gray =
			((int *)&(pSmart->RGB_OUTPUT.G_VOLTAGE))[cnt];

		pSmart->GRAY.TABLE[INFLECTION_VOLTAGE_ARRAY[cnt]].B_Gray =
			((int *)&(pSmart->RGB_OUTPUT.B_VOLTAGE))[cnt];
	}

	/*
		below codes use hard coded value.
		So it is possible to modify on each model.
		V0,V1,V3,V11,V23,V35,V51,V87,V151,V203,V255
	*/
	for (cnt = 0; cnt < GRAY_SCALE_MAX; cnt++) {

		if (cnt == INFLECTION_VOLTAGE_ARRAY[0]) {
			/* 0 */
			array_index = 0;
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[0]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[2])) {
			/* 1 ~ 2 */
			array_index = 2;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-2]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V0toV3_Coefficient, V0toV3_Multiple,
			V0toV3_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-2]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V0toV3_Coefficient, V0toV3_Multiple,
			V0toV3_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-2]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V0toV3_Coefficient, V0toV3_Multiple,
			V0toV3_denominator , cal_cnt);

			cal_cnt++;
		} else if (cnt == INFLECTION_VOLTAGE_ARRAY[2]) {
			/* 3 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[2]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[3])) {
			/* 4 ~ 10 */
			array_index = 3;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V3toV11_Coefficient, V3toV11_Multiple,
			V3toV11_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V3toV11_Coefficient, V3toV11_Multiple,
			V3toV11_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V3toV11_Coefficient, V3toV11_Multiple,
			V3toV11_denominator , cal_cnt);

			cal_cnt++;
		} else if (cnt == INFLECTION_VOLTAGE_ARRAY[3]) {
			/* 11 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[3]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[4])) {
			/* 12 ~ 22 */
			array_index = 4;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V11toV23_Coefficient, V11toV23_Multiple,
			V11toV23_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V11toV23_Coefficient, V11toV23_Multiple,
			V11toV23_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V11toV23_Coefficient, V11toV23_Multiple,
			V11toV23_denominator , cal_cnt);

			cal_cnt++;
		}  else if (cnt == INFLECTION_VOLTAGE_ARRAY[4]) {
			/* 23 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[4]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[5])) {
			/* 24 ~ 34 */
			array_index = 5;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V23toV35_Coefficient, V23toV35_Multiple,
			V23toV35_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V23toV35_Coefficient, V23toV35_Multiple,
			V23toV35_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V23toV35_Coefficient, V23toV35_Multiple,
			V23toV35_denominator , cal_cnt);

			cal_cnt++;
		} else if (cnt == INFLECTION_VOLTAGE_ARRAY[5]) {
			/* 35 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[5]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[6])) {
			/* 36 ~ 50 */
			array_index = 6;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V35toV51_Coefficient, V35toV51_Multiple,
			V35toV51_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V35toV51_Coefficient, V35toV51_Multiple,
			V35toV51_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V35toV51_Coefficient, V35toV51_Multiple,
			V35toV51_denominator, cal_cnt);
			cal_cnt++;

		} else if (cnt == INFLECTION_VOLTAGE_ARRAY[6]) {
			/* 51 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[6]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[7])) {
			/* 52 ~ 86 */
			array_index = 7;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V51toV87_Coefficient, V51toV87_Multiple,
			V51toV87_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V51toV87_Coefficient, V51toV87_Multiple,
			V51toV87_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V51toV87_Coefficient, V51toV87_Multiple,
			V51toV87_denominator, cal_cnt);
			cal_cnt++;

		} else if (cnt == INFLECTION_VOLTAGE_ARRAY[7]) {
			/* 87 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[7]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[8])) {
			/* 88 ~ 150 */
			array_index = 8;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V87toV151_Coefficient, V87toV151_Multiple,
			V87toV151_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V87toV151_Coefficient, V87toV151_Multiple,
			V87toV151_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V87toV151_Coefficient, V87toV151_Multiple,
			V87toV151_denominator, cal_cnt);

			cal_cnt++;
		} else if (cnt == INFLECTION_VOLTAGE_ARRAY[8]) {
			/* 151 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[8]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[9])) {
			/* 152 ~ 202 */
			array_index = 9;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V151toV203_Coefficient, V151toV203_Multiple,
			V151toV203_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V151toV203_Coefficient, V151toV203_Multiple,
			V151toV203_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V151toV203_Coefficient, V151toV203_Multiple,
			V151toV203_denominator, cal_cnt);

			cal_cnt++;
		} else if (cnt == INFLECTION_VOLTAGE_ARRAY[9]) {
			/* 203 */
			cal_cnt = 0;
		} else if ((cnt > INFLECTION_VOLTAGE_ARRAY[9]) &&
			(cnt < INFLECTION_VOLTAGE_ARRAY[10])) {
			/* 204 ~ 254 */
			array_index = 10;

			pSmart->GRAY.TABLE[cnt].R_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].R_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].R_Gray,
			V203toV255_Coefficient, V203toV255_Multiple,
			V203toV255_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].G_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].G_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].G_Gray,
			V203toV255_Coefficient, V203toV255_Multiple,
			V203toV255_denominator, cal_cnt);

			pSmart->GRAY.TABLE[cnt].B_Gray = cal_gray_scale_linear(
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index-1]].B_Gray,
			ptable[INFLECTION_VOLTAGE_ARRAY[array_index]].B_Gray,
			V203toV255_Coefficient, V203toV255_Multiple,
			V203toV255_denominator, cal_cnt);

			cal_cnt++;
		 } else {
			if (cnt == INFLECTION_VOLTAGE_ARRAY[10]) {
				pr_debug("%s end\n", __func__);
			} else {
				pr_err("%s fail cnt:%d\n", __func__, cnt);
				return -EINVAL;
			}
		}

	}

	return 0;
}


static int searching_function(long long candela, int *index, int gamma_curve)
{
	long long delta_1 = 0, delta_2 = 0;
	int cnt;

	/*
	*	This searching_functin should be changed with improved
		searcing algorithm to reduce searching time.
	*/
	*index = -1;

	for (cnt = 0; cnt < (GRAY_SCALE_MAX-1); cnt++) {
		/* gamma_init_revA() only ever searches gamma 2.2 */
		delta_1 = candela - curve_2p2_360[cnt];
		delta_2 = candela - curve_2p2_360[cnt+1];

		if (delta_2 < 0) {
			*index = (delta_1 + delta_2) <= 0 ? cnt : cnt+1;
			break;
		}

		if (delta_1 == 0) {
			*index = cnt;
			break;
		}

		if (delta_2 == 0) {
			*index = cnt+1;
			break;
		}
	}

	if (*index == -1)
		return -EINVAL;
	else
		return 0;
}

/* -1 means V1 */
#define TABLE_MAX  (ARRAY_MAX-1)
static void(*Make_hexa[TABLE_MAX])(int*, struct SMART_DIM*, char*) = {
	v255_hexa,
	v203_hexa,
	v151_hexa,
	v87_hexa,
	v51_hexa,
	v35_hexa,
	v23_hexa,
	v11_hexa,
	v3_hexa,
	vt_hexa,
};

static int get_base_luminance(struct SMART_DIM *pSmart)
{
	int cnt;
	int base_luminance[LUMINANCE_MAX][2];

	memcpy(base_luminance, base_luminance_revA, sizeof(base_luminance_revA));

	for (cnt = 0; cnt < LUMINANCE_MAX; cnt++)
		if (base_luminance[cnt][0] == pSmart->brightness_level)
			return base_luminance[cnt][1];

	return -1;
}

static int find_cadela_table(int brightness)
{
	int loop;
	int err = -1;

	for(loop = 0; loop <= CANDELA_MAX_TABLE; loop++)
		if (candela_table[loop][0] == brightness)
			return candela_table[loop][1];

	return err;
}

static void gamma_init_revA(struct SMART_DIM *pSmart, char *str, int size)
{
	long long candela_level[TABLE_MAX] = {-1, };
	int bl_index[TABLE_MAX] = {-1, };
	int gamma_setting[GAMMA_SET_MAX];

	long long temp_cal_data = 0;
	int bl_level = 0;

	int level_255_temp_MSB = 0;
	int level_V255 = 0;

	int point_index;
	int cnt;
	int table_index;

	pr_debug("%s : start !!\n",__func__);

	bl_level = get_base_luminance(pSmart);
	if (bl_level < 0)
		pr_err("%s : can not find base luminance!!\n", __func__);

	if (pSmart->brightness_level < 360) {
		for (cnt = 0; cnt < TABLE_MAX; cnt++) {
			point_index = INFLECTION_VOLTAGE_ARRAY[cnt+1];
			temp_cal_data =
			((long long)(candela_coeff_2p15[point_index])) *
			((long long)(bl_level));
			candela_level[cnt] = temp_cal_data;
		}
	} else {
		for (cnt = 0; cnt < TABLE_MAX; cnt++) {
			point_index = INFLECTION_VOLTAGE_ARRAY[cnt+1];
			temp_cal_data =
			((long long)(candela_coeff_2p2[point_index])) *
			((long long)(bl_level));
			candela_level[cnt] = temp_cal_data;
		}
	}


	for (cnt = 0; cnt < TABLE_MAX; cnt++) {
		if (searching_function(candela_level[cnt],
			&(bl_index[cnt]), GAMMA_CURVE_2P2)) {
			pr_debug("%s searching functioin error cnt:%d\n",
			__func__, cnt);
		}
	}

	/*
	*	Candela compensation
	*/
	for (cnt = 1; cnt < TABLE_MAX; cnt++) {
		table_index = find_cadela_table(pSmart->brightness_level);

		if (table_index == -1) {
			table_index = CANDELA_MAX_TABLE;
			pr_debug("%s fail candela table_index cnt : %d brightness %d\n",
				__func__, cnt, pSmart->brightness_level);
		}

		bl_index[TABLE_MAX - cnt] +=
			gradation_offset_revA[table_index][cnt - 1];

		/* THERE IS M-GRAY0 target */
		if (bl_index[TABLE_MAX - cnt] == 0)
			bl_index[TABLE_MAX - cnt] = 1;
	}

	/*Generate Gamma table*/
	for (cnt = 0; cnt < TABLE_MAX; cnt++)
		(void)Make_hexa[cnt](bl_index , pSmart, str);

	/* To avoid overflow */
	for (cnt = 0; cnt < GAMMA_SET_MAX; cnt++)
		gamma_setting[cnt] = str[cnt];

	/*
	*	RGB compensation
	*/
	for (cnt = 0; cnt < RGB_COMPENSATION; cnt++) {
		table_index = find_cadela_table(pSmart->brightness_level);

		if (table_index == -1) {
			table_index = CANDELA_MAX_TABLE;
			pr_debug("%s fail RGB table_index cnt : %d brightness %d",
				__func__, cnt, pSmart->brightness_level);
		}

		if (cnt < 3) {
			level_V255 = str[cnt * 2] << 8 | str[(cnt * 2) + 1];
			level_V255 +=
				rgb_offset_revA[table_index][cnt];
			level_255_temp_MSB = level_V255 / 256;

			gamma_setting[cnt * 2] = level_255_temp_MSB & 0xff;
			gamma_setting[(cnt * 2) + 1] = level_V255 & 0xff;
		} else {
			gamma_setting[cnt+3] += rgb_offset_revA[table_index][cnt];
		}
	}

	/*subtration MTP_OFFSET value from generated gamma table*/
	/* mtp_offset_substraction is not need because Change Gamma Offset Index [4] is used. */
	/* mtp_offset_substraction(pSmart, gamma_setting); */

	/* To avoid overflow */
	for (cnt = 0; cnt < GAMMA_SET_MAX; cnt++)
		str[cnt] = gamma_setting[cnt];
}


static void mtp_sorting(struct SMART_DIM *psmart)
{
	int sorting[GAMMA_SET_MAX] = {
		9, 7, 6, 5, 4, 3, 2, 1, 0, 8, 10, /* R*/
		20, 18, 17, 16, 15, 14, 13, 12, 11, 19, 21, /* G */
		31, 29, 28, 27, 26, 25, 24, 23, 22, 30, 32, /* B */
	};
	int loop;
	char *pfrom, *pdest;

	pfrom = (char *)&(psmart->MTP_ORIGN);
	pdest = (char *)&(psmart->MTP);

	for (loop = 0; loop < GAMMA_SET_MAX; loop++)
		pdest[loop] = pfrom[sorting[loop]];

	for (loop = 0; loop < 3 ; loop++) {
		if(pdest[loop*11])
			pdest[loop * 11] *= -1;
	}
}

static void gamma_cell_determine(int ldi_revision)
{
	pr_debug("%s ldi_revision: 0x%x", __func__, ldi_revision);

	max_lux_table[0] = V255_300CD_R_MSB = V255_300CD_R_MSB_20;
	max_lux_table[1] = V255_300CD_R_LSB = V255_300CD_R_LSB_20;

	max_lux_table[2] = V255_300CD_G_MSB = V255_300CD_G_MSB_20;
	max_lux_table[3] = V255_300CD_G_LSB = V255_300CD_G_LSB_20;

	max_lux_table[4] = V255_300CD_B_MSB = V255_300CD_B_MSB_20;
	max_lux_table[5] = V255_300CD_B_LSB = V255_300CD_B_LSB_20;

	max_lux_table[6] = V203_300CD_R = V203_300CD_R_20;
	max_lux_table[7] = V203_300CD_G = V203_300CD_G_20;
	max_lux_table[8] = V203_300CD_B = V203_300CD_B_20;

	max_lux_table[9] = V151_300CD_R = V151_300CD_R_20;
	max_lux_table[10] = V151_300CD_G = V151_300CD_G_20;
	max_lux_table[11] = V151_300CD_B = V151_300CD_B_20;

	max_lux_table[12] = V87_300CD_R = V87_300CD_R_20;
	max_lux_table[13] = V87_300CD_G = V87_300CD_G_20;
	max_lux_table[14] = V87_300CD_B = V87_300CD_B_20;

	max_lux_table[15] = V51_300CD_R = V51_300CD_R_20;
	max_lux_table[16] = V51_300CD_G = V51_300CD_G_20;
	max_lux_table[17] = V51_300CD_B = V51_300CD_B_20;

	max_lux_table[18] = V35_300CD_R = V35_300CD_R_20;
	max_lux_table[19] = V35_300CD_G = V35_300CD_G_20;
	max_lux_table[20] = V35_300CD_B = V35_300CD_B_20;

	max_lux_table[21] = V23_300CD_R = V23_300CD_R_20;
	max_lux_table[22] = V23_300CD_G = V23_300CD_G_20;
	max_lux_table[23] = V23_300CD_B = V23_300CD_B_20;

	max_lux_table[24] = V11_300CD_R = V11_300CD_R_20;
	max_lux_table[25] = V11_300CD_G = V11_300CD_G_20;
	max_lux_table[26] = V11_300CD_B = V11_300CD_B_20;

	max_lux_table[27] = V3_300CD_R = V3_300CD_R_20;
	max_lux_table[28] = V3_300CD_G = V3_300CD_G_20;
	max_lux_table[29] = V3_300CD_B = V3_300CD_B_20;

	max_lux_table[30] = VT_300CD_R = VT_300CD_R_20;
	max_lux_table[31] = VT_300CD_G = VT_300CD_G_20;
	max_lux_table[32] = VT_300CD_B = VT_300CD_B_20;
}

static void gamma_cell_determine_max(struct SMART_DIM *pSmart)
{
	int temp = 0; /* MAX_V255 */

	/* RED */
	max_lux_table2[0]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_11);
	max_lux_table2[1]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_23);
	max_lux_table2[2]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_35);
	max_lux_table2[3]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_51);
	max_lux_table2[4]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_87);
	max_lux_table2[5]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_151);
	max_lux_table2[6]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_203);

	temp = 256 + char_to_int_v255(pSmart->MTP.R_OFFSET.OFFSET_255_MSB, pSmart->MTP.R_OFFSET.OFFSET_255_LSB);
	if (temp >= 256)
		max_lux_table2[7]  = temp - 256;
	else
		max_lux_table2[7]  = temp;
	max_lux_table2[8]  = 128 + char_to_int(pSmart->MTP.R_OFFSET.OFFSET_3);

	if (temp >= 256)
		max_lux_table2[9]  = 0x80; /* V255[8] bit */
	else
		max_lux_table2[9]  = 0;
	max_lux_table2[10] = 0;

	/* GREEN */
	max_lux_table2[11] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_11);
	max_lux_table2[12] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_23);
	max_lux_table2[13] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_35);
	max_lux_table2[14] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_51);
	max_lux_table2[15] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_87);
	max_lux_table2[16] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_151);
	max_lux_table2[17] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_203);

	temp = 256 + char_to_int_v255(pSmart->MTP.G_OFFSET.OFFSET_255_MSB, pSmart->MTP.G_OFFSET.OFFSET_255_LSB);
	if (temp >= 256)
		max_lux_table2[18]  = temp - 256;
	else
		max_lux_table2[18]  = temp;
	max_lux_table2[19] = 128 + char_to_int(pSmart->MTP.G_OFFSET.OFFSET_3);

	if (temp >= 256)
		max_lux_table2[20]  = 0x80;
	else
		max_lux_table2[20]  = 0;
	max_lux_table2[21] = 0;

	/* BLUE */
	max_lux_table2[22] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_11);
	max_lux_table2[23] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_23);
	max_lux_table2[24] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_35);
	max_lux_table2[25] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_51);
	max_lux_table2[26] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_87);
	max_lux_table2[27] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_151);
	max_lux_table2[28] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_203);

	temp = 256 + char_to_int_v255(pSmart->MTP.B_OFFSET.OFFSET_255_MSB, pSmart->MTP.B_OFFSET.OFFSET_255_LSB);
	if (temp >= 256)
		max_lux_table2[29]  = temp - 256;
	else
		max_lux_table2[29]  = temp;
	max_lux_table2[30] = 128 + char_to_int(pSmart->MTP.B_OFFSET.OFFSET_3);

	if (temp >= 256)
		max_lux_table2[31]  = 0x80;
	else
		max_lux_table2[31]  = 0;
	max_lux_table2[32] = 0;

}

static char offeset_add(int offset,  int value)
{
	if (value + offset < 0)
		return 0;
	else if (value + offset > 255)
		return 0x80;
	else
		return value + offset;
}

static void gamma_command_sorting_post(struct SMART_DIM *psmart)
{
	int sorting[GAMMA_SET_MAX] = {
		24, 21, 18, 15, 12, 9,  6, 1, 27, 0, 30, /* R*/
		25, 22, 19, 16, 13, 10, 7, 3, 28, 2, 31, /* G */
		26, 23, 20, 17, 14, 11, 8, 5, 29, 4, 32, /* B */
	};
	int loop, loop2;
	char gamma_temp[GAMMA_SET_MAX];
	char *pfrom;

	pfrom = (char *)&(psmart->MTP_ORIGN);

	for (loop = 0; loop < LUMINANCE_MAX; loop++) {
		/* sorting */
		for (loop2 = 0; loop2 < GAMMA_SET_MAX; loop2++) {
			if(loop == LUMINANCE_MAX-1 ) {
				if(psmart->gen_table[loop].gamma_setting[sorting[loop2]] == 0x01)
					gamma_temp[loop2] =
						offeset_add(psmart->gen_table[loop].gamma_setting[sorting[loop2]], pfrom[loop2] + 0x7F);
				else
					gamma_temp[loop2] =
						offeset_add(psmart->gen_table[loop].gamma_setting[sorting[loop2]], pfrom[loop2]);
			} else {
				gamma_temp[loop2] = psmart->gen_table[loop].gamma_setting[sorting[loop2]];
			}
		}

		for (loop2 = 0; loop2 < 3; loop2++) {
			if(gamma_temp[(loop2*11)+9] == 0x01)
				gamma_temp[(loop2*11)+9] =
						gamma_temp[(loop2*11)+9] << 7;
		}

		/* Copy */
		memcpy(psmart->gen_table[loop].gamma_setting,
					gamma_temp, GAMMA_SET_MAX);
	}
}

#define HBM_CANDELA 500
#define HBM_GAMMA_SET_CNT 33
#define V255_RGB_MSB_CNT 3


/* The luminance a level's gamma is computed for (its white with no AOR) */
unsigned int ana38401_dimming_base(unsigned int level)
{
	return base_luminance_revA[level][1];
}

/* The brightness levels, from the dimmest: candela, AOR, ELVSS */
const struct ana38401_level ana38401_levels[ANA38401_DIMMING_LEVELS] = {
	{   2, 0x18d, 0x0f },
	{   3, 0x18a, 0x0f },
	{   4, 0x187, 0x0f },
	{   5, 0x184, 0x10 },
	{   6, 0x180, 0x10 },
	{   7, 0x17e, 0x10 },
	{   8, 0x17a, 0x10 },
	{   9, 0x177, 0x10 },
	{  10, 0x174, 0x10 },
	{  11, 0x16f, 0x11 },
	{  12, 0x16d, 0x11 },
	{  13, 0x16a, 0x11 },
	{  14, 0x166, 0x11 },
	{  15, 0x163, 0x11 },
	{  16, 0x15f, 0x12 },
	{  17, 0x15c, 0x12 },
	{  19, 0x155, 0x12 },
	{  20, 0x152, 0x12 },
	{  21, 0x14e, 0x13 },
	{  22, 0x14a, 0x13 },
	{  24, 0x144, 0x13 },
	{  25, 0x140, 0x13 },
	{  27, 0x13a, 0x14 },
	{  29, 0x132, 0x14 },
	{  30, 0x12f, 0x14 },
	{  32, 0x128, 0x15 },
	{  34, 0x120, 0x15 },
	{  37, 0x116, 0x16 },
	{  39, 0x10e, 0x16 },
	{  41, 0x107, 0x16 },
	{  44, 0x0fc, 0x17 },
	{  47, 0x0f2, 0x17 },
	{  50, 0x0e6, 0x18 },
	{  53, 0x0db, 0x18 },
	{  56, 0x0cf, 0x19 },
	{  60, 0x0bf, 0x19 },
	{  64, 0x0b1, 0x19 },
	{  68, 0x0a3, 0x19 },
	{  72, 0x09c, 0x19 },
	{  77, 0x09c, 0x19 },
	{  82, 0x09c, 0x19 },
	{  87, 0x09c, 0x19 },
	{  93, 0x09c, 0x19 },
	{  98, 0x09c, 0x19 },
	{ 105, 0x09c, 0x19 },
	{ 111, 0x09c, 0x19 },
	{ 119, 0x09c, 0x19 },
	{ 126, 0x09c, 0x18 },
	{ 134, 0x09c, 0x18 },
	{ 143, 0x09c, 0x17 },
	{ 152, 0x09c, 0x16 },
	{ 162, 0x09c, 0x16 },
	{ 172, 0x08e, 0x15 },
	{ 183, 0x07c, 0x15 },
	{ 195, 0x068, 0x15 },
	{ 207, 0x054, 0x15 },
	{ 220, 0x03e, 0x14 },
	{ 234, 0x024, 0x14 },
	{ 249, 0x00e, 0x14 },
	{ 265, 0x00e, 0x13 },
	{ 282, 0x00e, 0x13 },
	{ 300, 0x00e, 0x12 },
	{ 316, 0x00e, 0x11 },
	{ 333, 0x00e, 0x10 },
	{ 360, 0x00e, 0x0f },
};

/* As Samsung's smart_dimming_init(), without HBM */
int ana38401_dimming_gamma(const u8 *mtp,
			   u8 (*gamma)[ANA38401_DIMMING_GAMMA_LEN])
{
	int cds[ANA38401_DIMMING_LEVELS];
	struct SMART_DIM *psmart;
	int lux_loop, ret = 0;

	BUILD_BUG_ON(ANA38401_DIMMING_LEVELS != LUMINANCE_MAX);
	BUILD_BUG_ON(ANA38401_DIMMING_GAMMA_LEN != GAMMA_SET_MAX);

	psmart = kzalloc(sizeof(*psmart), GFP_KERNEL);
	if (!psmart)
		return -ENOMEM;

	for (lux_loop = 0; lux_loop < ANA38401_DIMMING_LEVELS; lux_loop++)
		cds[lux_loop] = ana38401_levels[lux_loop].cd;

	memcpy(&psmart->MTP_ORIGN, mtp, GAMMA_SET_MAX);
	psmart->plux_table = cds;
	psmart->lux_table_max = ANA38401_DIMMING_LEVELS;

	mtp_sorting(psmart);
	gamma_cell_determine(psmart->ldi_revision);

	v255_adjustment(psmart);
	vt_adjustment(psmart);
	v203_adjustment(psmart);
	v151_adjustment(psmart);
	v87_adjustment(psmart);
	v51_adjustment(psmart);
	v35_adjustment(psmart);
	v23_adjustment(psmart);
	v11_adjustment(psmart);
	v3_adjustment(psmart);

	if (generate_gray_scale(psmart)) {
		ret = -EINVAL;
		goto out;
	}

	for (lux_loop = 0; lux_loop < psmart->lux_table_max; lux_loop++) {
		psmart->brightness_level = psmart->plux_table[lux_loop];
		psmart->gen_table[lux_loop].lux = psmart->plux_table[lux_loop];
		gamma_init_revA(psmart,
				(char *)(&(psmart->gen_table[lux_loop].gamma_setting)),
				GAMMA_SET_MAX);
	}

	/* set 300CD max gamma table */
	memcpy(&(psmart->gen_table[lux_loop-1].gamma_setting),
			max_lux_table, GAMMA_SET_MAX);

	gamma_command_sorting_post(psmart);
	gamma_cell_determine_max(psmart);

	/* Set 360CD max gamma table */
	memcpy(&(psmart->gen_table[LUMINANCE_MAX-1].gamma_setting),
			max_lux_table2, GAMMA_SET_MAX);

	for (lux_loop = 0; lux_loop < ANA38401_DIMMING_LEVELS; lux_loop++)
		memcpy(gamma[lux_loop], psmart->gen_table[lux_loop].gamma_setting,
		       GAMMA_SET_MAX);
out:
	kfree(psmart);
	return ret;
}
