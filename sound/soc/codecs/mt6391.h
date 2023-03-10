/* Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __MT6391_H__
#define __MT6391_H__

/*  digital pmic register definition */
#define MT6397_DIG_AUDIO_BASE          (0x4000)
#define MT6397_AFE_UL_DL_CON0          (MT6397_DIG_AUDIO_BASE + 0x0000)
#define MT6397_AFE_DL_SRC2_CON0_H      (MT6397_DIG_AUDIO_BASE + 0x0002)
#define MT6397_AFE_DL_SRC2_CON0_L      (MT6397_DIG_AUDIO_BASE + 0x0004)
#define MT6397_AFE_DL_SDM_CON0         (MT6397_DIG_AUDIO_BASE + 0x0006)
#define MT6397_AFE_DL_SDM_CON1         (MT6397_DIG_AUDIO_BASE + 0x0008)
#define MT6397_AFE_UL_SRC_CON0_H       (MT6397_DIG_AUDIO_BASE + 0x000A)
#define MT6397_AFE_UL_SRC_CON0_L       (MT6397_DIG_AUDIO_BASE + 0x000C)
#define MT6397_AFE_UL_SRC_CON1_H       (MT6397_DIG_AUDIO_BASE + 0x000E)
#define MT6397_AFE_UL_SRC_CON1_L       (MT6397_DIG_AUDIO_BASE + 0x0010)
#define MT6397_ANA_AFE_TOP_CON0        (MT6397_DIG_AUDIO_BASE + 0x0012)
#define MT6397_ANA_AUDIO_TOP_CON0      (MT6397_DIG_AUDIO_BASE + 0x0014)
#define MT6397_AFE_DL_SRC_MON0         (MT6397_DIG_AUDIO_BASE + 0x0016)
#define MT6397_AFE_DL_SDM_TEST0        (MT6397_DIG_AUDIO_BASE + 0x0018)
#define MT6397_AFE_MON_DEBUG0          (MT6397_DIG_AUDIO_BASE + 0x001A)
#define MT6397_AFUNC_AUD_CON0          (MT6397_DIG_AUDIO_BASE + 0x001C)
#define MT6397_AFUNC_AUD_CON1          (MT6397_DIG_AUDIO_BASE + 0x001E)
#define MT6397_AFUNC_AUD_CON2          (MT6397_DIG_AUDIO_BASE + 0x0020)
#define MT6397_AFUNC_AUD_CON3          (MT6397_DIG_AUDIO_BASE + 0x0022)
#define MT6397_AFUNC_AUD_CON4          (MT6397_DIG_AUDIO_BASE + 0x0024)
#define MT6397_AFUNC_AUD_MON0          (MT6397_DIG_AUDIO_BASE + 0x0026)
#define MT6397_AFUNC_AUD_MON1          (MT6397_DIG_AUDIO_BASE + 0x0028)
#define MT6397_AUDRC_TUNE_MON0         (MT6397_DIG_AUDIO_BASE + 0x002A)
#define MT6397_AFE_UP8X_FIFO_CFG0      (MT6397_DIG_AUDIO_BASE + 0x002C)
#define MT6397_AFE_UP8X_FIFO_LOG_MON0  (MT6397_DIG_AUDIO_BASE + 0x002E)
#define MT6397_AFE_UP8X_FIFO_LOG_MON1  (MT6397_DIG_AUDIO_BASE + 0x0030)
#define MT6397_AFE_DL_DC_COMP_CFG0     (MT6397_DIG_AUDIO_BASE + 0x0032)
#define MT6397_AFE_DL_DC_COMP_CFG1     (MT6397_DIG_AUDIO_BASE + 0x0034)
#define MT6397_AFE_DL_DC_COMP_CFG2     (MT6397_DIG_AUDIO_BASE + 0x0036)
#define MT6397_AFE_PMIC_NEWIF_CFG0     (MT6397_DIG_AUDIO_BASE + 0x0038)
#define MT6397_AFE_PMIC_NEWIF_CFG1     (MT6397_DIG_AUDIO_BASE + 0x003A)
#define MT6397_AFE_PMIC_NEWIF_CFG2     (MT6397_DIG_AUDIO_BASE + 0x003C)
#define MT6397_AFE_PMIC_NEWIF_CFG3     (MT6397_DIG_AUDIO_BASE + 0x003E)
#define MT6397_AFE_SGEN_CFG0           (MT6397_DIG_AUDIO_BASE + 0x0040)
#define MT6397_AFE_SGEN_CFG1           (MT6397_DIG_AUDIO_BASE + 0x0042)

#ifdef CONFIG_ASUS_MT8173_ACCDET
extern int mt8173_accdet_enable(struct snd_soc_codec *codec);
#else
static inline int mt8173_accdet_enable(struct snd_soc_codec *codec)
{
	return -ENOENT;
}
#endif /* CONFIG_ASUS_MT8173_ACCDET */

#endif
