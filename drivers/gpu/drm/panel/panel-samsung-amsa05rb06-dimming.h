/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PANEL_SAMSUNG_AMSA05RB06_DIMMING_H
#define PANEL_SAMSUNG_AMSA05RB06_DIMMING_H

#include <linux/types.h>

#define ANA38401_DIMMING_LEVELS		65
#define ANA38401_DIMMING_GAMMA_LEN	33

struct ana38401_level {
	u16 cd;		/* luminance */
	u16 aor;	/* AMOLED off ratio, in units of 4 lines */
	u8 elvss;	/* at normal temperature */
};

extern const struct ana38401_level ana38401_levels[ANA38401_DIMMING_LEVELS];

/* The luminance the gamma of a level (an index into ana38401_levels) is for */
unsigned int ana38401_dimming_base(unsigned int level);

/* The gamma register for each level, from the 33 bytes of the MTP */
int ana38401_dimming_gamma(const u8 *mtp,
			   u8 (*gamma)[ANA38401_DIMMING_GAMMA_LEN]);

#endif
