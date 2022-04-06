/*
 * cs47l92.c  --  ALSA SoC Audio driver for CS47L92/CS47L93 codecs
 *
 * Copyright 2016-2017 Cirrus Logic
 *
 * Author: Stuart Henderson <stuarth@opensource.wolfsonmicro.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include <linux/mfd/madera/core.h>
#include <linux/mfd/madera/registers.h>

#include "madera.h"
#include "wm_adsp.h"

#define CS47L92_NUM_ADSP	1
#define CS47L92_MONO_OUTPUTS	3

struct cs47l92 {
	struct madera_priv core;
	struct madera_fll fll[2];
};

static const struct wm_adsp_region cs47l92_dsp1_regions[] = {
	{ .type = WMFW_ADSP2_PM, .base = 0x080000 },
	{ .type = WMFW_ADSP2_ZM, .base = 0x0e0000 },
	{ .type = WMFW_ADSP2_XM, .base = 0x0a0000 },
	{ .type = WMFW_ADSP2_YM, .base = 0x0c0000 },
};

static const char * const cs47l92_outdemux_texts[] = {
	"HPOUT3",
	"HPOUT4",
};

static int cs47l92_put_demux(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
					snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct snd_soc_codec *codec = snd_soc_dapm_kcontrol_codec(kcontrol);
	struct madera *madera = dev_get_drvdata(codec->dev->parent);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int ep_sel, mux, change, mask, cur;
	int ret, i;
	bool out_mono, accdet_out = false;

	if (ucontrol->value.enumerated.item[0] > e->items - 1)
		return -EINVAL;
	mux = ucontrol->value.enumerated.item[0];
	ep_sel = mux << e->shift_l;
	mask = e->mask << e->shift_l;

	snd_soc_dapm_mutex_lock(dapm);

	change = snd_soc_test_bits(codec, e->reg, mask, ep_sel);
	if (!change)
		goto end;

	for (i = 0; i < MADERA_MAX_ACCESSORY; i++) {
		if (madera->pdata.accdet[i].output == 3) {
			accdet_out = true;
			break;
		}
	}

	if (accdet_out) {
		/* HPOUT3 is associated with accessory detect activities */
		cur = madera->hp_ena;
	} else {
		ret = regmap_read(madera->regmap,
				  MADERA_OUTPUT_ENABLES_1,
				  &cur);
		if (ret != 0)
			dev_warn(madera->dev,
				 "Failed to read current reg: %d\n",
				 ret);
	}

	/* EP_SEL and OUT3_MONO should not be modified while HPOUT3 or 4
	 * are enabled
	 */
	ret = regmap_update_bits(madera->regmap, MADERA_OUTPUT_ENABLES_1,
				 MADERA_OUT3L_ENA | MADERA_OUT3R_ENA, 0);
	if (ret)
		dev_warn(madera->dev, "Failed to disable outputs: %d\n", ret);

	usleep_range(2000, 3000); /* wait for wseq to complete */

	ret = regmap_update_bits(madera->regmap, MADERA_OUTPUT_ENABLES_1,
				 MADERA_EP_SEL, ep_sel);
	if (ret) {
		dev_err(madera->dev, "Failed to set EP_SEL: %d\n", ret);
	} else {
		out_mono = madera->pdata.codec.out_mono[2 + mux];
		ret = madera_set_output_mode(codec, 3, out_mono);
		if (ret < 0)
			dev_warn(madera->dev,
				 "Failed to set output mode: %d\n", ret);
	}

	ret = regmap_update_bits(madera->regmap, MADERA_OUTPUT_ENABLES_1,
				 MADERA_OUT3L_ENA | MADERA_OUT3R_ENA, cur);
	if (ret) {
		dev_warn(madera->dev, "Failed to restore outputs: %d\n", ret);
	} else {
		/* wait for wseq */
		if (cur & (MADERA_OUT3L_ENA | MADERA_OUT3R_ENA))
			msleep(34); /* enable delay */
		else
			usleep_range(2000, 3000); /* disable delay */
	}

end:
	snd_soc_dapm_mutex_unlock(dapm);

	return snd_soc_dapm_mux_update_power(dapm, kcontrol, mux, e, NULL);
}

static SOC_ENUM_SINGLE_DECL(cs47l92_outdemux_enum,
			    MADERA_OUTPUT_ENABLES_1,
			    MADERA_EP_SEL_SHIFT,
			    cs47l92_outdemux_texts);

static const struct snd_kcontrol_new cs47l92_outdemux =
	SOC_DAPM_ENUM_EXT("OUT3 Demux", cs47l92_outdemux_enum,
			snd_soc_dapm_get_enum_double, cs47l92_put_demux);

static const char * const cs47l92_auxpdm_freq_texts[] = {
	"3.072Mhz",
	"2.048Mhz",
	"1.536Mhz",
	"768khz",
};
static SOC_ENUM_SINGLE_DECL(cs47l92_auxpdm_freq_enum,
			    MADERA_AUXPDM1_CTRL_1,
			    MADERA_AUXPDM1_CLK_FREQ_SHIFT,
			    cs47l92_auxpdm_freq_texts);

static const char * const cs47l92_auxpdm_in_texts[] = {
	"IN1L",
	"IN1R",
	"IN2L",
	"IN2R",
};
static SOC_ENUM_SINGLE_DECL(cs47l92_auxpdm_in_enum,
			    MADERA_AUXPDM1_CTRL_0,
			    MADERA_AUXPDM1_SRC_SHIFT,
			    cs47l92_auxpdm_in_texts);

static const struct snd_kcontrol_new cs47l92_auxpdm1_inmux =
		SOC_DAPM_ENUM("AUXPDM1 Input", cs47l92_auxpdm_in_enum);

static const struct snd_kcontrol_new cs47l92_auxpdm1_switch =
		SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0);

static int cs47l92_adsp_power_ev(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol,
				 int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct cs47l92 *cs47l92 = snd_soc_codec_get_drvdata(codec);
	struct madera_priv *priv = &cs47l92->core;
	struct madera *madera = priv->madera;
	unsigned int freq;
	int ret;

	ret = regmap_read(madera->regmap, MADERA_DSP_CLOCK_2, &freq);
	if (ret != 0) {
		dev_err(madera->dev,
			"Failed to read MADERA_DSP_CLOCK_2: %d\n", ret);
		return ret;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = madera_set_adsp_clk(&cs47l92->core, w->shift, freq);
		if (ret)
			return ret;
		break;
	default:
		break;
	}

	return wm_adsp2_early_event(w, kcontrol, event, freq);
}

static int cs47l92_asrc_ev(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = snd_soc_dapm_to_codec(w->dapm);
	struct cs47l92 *cs47l92 = snd_soc_codec_get_drvdata(codec);
	struct madera_priv *priv = &cs47l92->core;
	struct madera *madera = priv->madera;
	unsigned int val;
	int rate;
	int ret;

	ret = regmap_read(madera->regmap, w->reg + 2, &val);
	if (ret)
		return ret;

	val &= MADERA_ASRC1_RATE1_MASK;
	val >>= MADERA_ASRC1_RATE1_SHIFT;

	rate = madera_sample_rate_val_to_rate(madera, val);
	if (rate < 0)
		return rate;

	if (rate > 192000)
		dev_err(madera->dev, "Sample rate too high for ASRC\n");

	ret = regmap_read(madera->regmap, w->reg + 3, &val);
	if (ret)
		return ret;

	val &= MADERA_ASRC1_RATE1_MASK;
	val >>= MADERA_ASRC1_RATE1_SHIFT;

	rate = madera_sample_rate_val_to_rate(madera, val);
	if (rate < 0)
		return rate;

	if (rate > 192000)
		dev_err(madera->dev, "Sample rate too high for ASRC\n");

	return 0;
}

#define CS47L92_NG_SRC(name, base) \
	SOC_SINGLE(name " NG HPOUT1L Switch",  base,  0, 1, 0), \
	SOC_SINGLE(name " NG HPOUT1R Switch",  base,  1, 1, 0), \
	SOC_SINGLE(name " NG HPOUT2L Switch",  base,  2, 1, 0), \
	SOC_SINGLE(name " NG HPOUT2R Switch",  base,  3, 1, 0), \
	SOC_SINGLE(name " NG HPOUT3L Switch",  base,  4, 1, 0), \
	SOC_SINGLE(name " NG HPOUT3R Switch",  base,  5, 1, 0), \
	SOC_SINGLE(name " NG SPKDAT1L Switch", base,  8, 1, 0), \
	SOC_SINGLE(name " NG SPKDAT1R Switch", base,  9, 1, 0)

static const struct snd_kcontrol_new cs47l92_snd_controls[] = {
SOC_ENUM("IN1 OSR", madera_in_dmic_osr[0]),
SOC_ENUM("IN2 OSR", madera_in_dmic_osr[1]),
SOC_ENUM("IN3 OSR", madera_in_dmic_osr[2]),
SOC_ENUM("IN4 OSR", madera_in_dmic_osr[3]),

SOC_SINGLE_RANGE_TLV("IN1L Volume", MADERA_IN1L_CONTROL,
		     MADERA_IN1L_PGA_VOL_SHIFT, 0x40, 0x5f, 0, madera_ana_tlv),
SOC_SINGLE_RANGE_TLV("IN1R Volume", MADERA_IN1R_CONTROL,
		     MADERA_IN1R_PGA_VOL_SHIFT, 0x40, 0x5f, 0, madera_ana_tlv),
SOC_SINGLE_RANGE_TLV("IN2L Volume", MADERA_IN2L_CONTROL,
		     MADERA_IN2L_PGA_VOL_SHIFT, 0x40, 0x5f, 0, madera_ana_tlv),
SOC_SINGLE_RANGE_TLV("IN2R Volume", MADERA_IN2R_CONTROL,
		     MADERA_IN2R_PGA_VOL_SHIFT, 0x40, 0x5f, 0, madera_ana_tlv),

SOC_ENUM("IN HPF Cutoff Frequency", madera_in_hpf_cut_enum),

SOC_SINGLE_EXT("IN1L LP Switch", MADERA_ADC_DIGITAL_VOLUME_1L,
	       MADERA_IN1L_LP_MODE_SHIFT, 1, 0,
	       snd_soc_get_volsw, madera_lp_mode_put),
SOC_SINGLE_EXT("IN1R LP Switch", MADERA_ADC_DIGITAL_VOLUME_1R,
	       MADERA_IN1L_LP_MODE_SHIFT, 1, 0,
	       snd_soc_get_volsw, madera_lp_mode_put),
SOC_SINGLE_EXT("IN2L LP Switch", MADERA_ADC_DIGITAL_VOLUME_2L,
	       MADERA_IN1L_LP_MODE_SHIFT, 1, 0,
	       snd_soc_get_volsw, madera_lp_mode_put),
SOC_SINGLE_EXT("IN2R LP Switch", MADERA_ADC_DIGITAL_VOLUME_2R,
	       MADERA_IN1L_LP_MODE_SHIFT, 1, 0,
	       snd_soc_get_volsw, madera_lp_mode_put),

SOC_SINGLE("IN1L HPF Switch", MADERA_IN1L_CONTROL,
	   MADERA_IN1L_HPF_SHIFT, 1, 0),
SOC_SINGLE("IN1R HPF Switch", MADERA_IN1R_CONTROL,
	   MADERA_IN1R_HPF_SHIFT, 1, 0),
SOC_SINGLE("IN2L HPF Switch", MADERA_IN2L_CONTROL,
	   MADERA_IN2L_HPF_SHIFT, 1, 0),
SOC_SINGLE("IN2R HPF Switch", MADERA_IN2R_CONTROL,
	   MADERA_IN2R_HPF_SHIFT, 1, 0),
SOC_SINGLE("IN3L HPF Switch", MADERA_IN3L_CONTROL,
	   MADERA_IN3L_HPF_SHIFT, 1, 0),
SOC_SINGLE("IN3R HPF Switch", MADERA_IN3R_CONTROL,
	   MADERA_IN3R_HPF_SHIFT, 1, 0),
SOC_SINGLE("IN4L HPF Switch", MADERA_IN4L_CONTROL,
	   MADERA_IN4L_HPF_SHIFT, 1, 0),
SOC_SINGLE("IN4R HPF Switch", MADERA_IN4R_CONTROL,
	   MADERA_IN4R_HPF_SHIFT, 1, 0),

SOC_SINGLE_TLV("IN1L Digital Volume", MADERA_ADC_DIGITAL_VOLUME_1L,
	       MADERA_IN1L_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),
SOC_SINGLE_TLV("IN1R Digital Volume", MADERA_ADC_DIGITAL_VOLUME_1R,
	       MADERA_IN1R_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),
SOC_SINGLE_TLV("IN2L Digital Volume", MADERA_ADC_DIGITAL_VOLUME_2L,
	       MADERA_IN2L_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),
SOC_SINGLE_TLV("IN2R Digital Volume", MADERA_ADC_DIGITAL_VOLUME_2R,
	       MADERA_IN2R_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),
SOC_SINGLE_TLV("IN3L Digital Volume", MADERA_ADC_DIGITAL_VOLUME_3L,
	       MADERA_IN3L_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),
SOC_SINGLE_TLV("IN3R Digital Volume", MADERA_ADC_DIGITAL_VOLUME_3R,
	       MADERA_IN3R_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),
SOC_SINGLE_TLV("IN4L Digital Volume", MADERA_ADC_DIGITAL_VOLUME_4L,
	       MADERA_IN4L_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),
SOC_SINGLE_TLV("IN4R Digital Volume", MADERA_ADC_DIGITAL_VOLUME_4R,
	       MADERA_IN4R_DIG_VOL_SHIFT, 0xbf, 0, madera_digital_tlv),

SOC_ENUM("Input Ramp Up", madera_in_vi_ramp),
SOC_ENUM("Input Ramp Down", madera_in_vd_ramp),

MADERA_MIXER_CONTROLS("EQ1", MADERA_EQ1MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("EQ2", MADERA_EQ2MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("EQ3", MADERA_EQ3MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("EQ4", MADERA_EQ4MIX_INPUT_1_SOURCE),

MADERA_EQ_CONTROL("EQ1 Coefficients", MADERA_EQ1_2),
SOC_SINGLE_TLV("EQ1 B1 Volume", MADERA_EQ1_1, MADERA_EQ1_B1_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ1 B2 Volume", MADERA_EQ1_1, MADERA_EQ1_B2_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ1 B3 Volume", MADERA_EQ1_1, MADERA_EQ1_B3_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ1 B4 Volume", MADERA_EQ1_2, MADERA_EQ1_B4_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ1 B5 Volume", MADERA_EQ1_2, MADERA_EQ1_B5_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),

MADERA_EQ_CONTROL("EQ2 Coefficients", MADERA_EQ2_2),
SOC_SINGLE_TLV("EQ2 B1 Volume", MADERA_EQ2_1, MADERA_EQ2_B1_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ2 B2 Volume", MADERA_EQ2_1, MADERA_EQ2_B2_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ2 B3 Volume", MADERA_EQ2_1, MADERA_EQ2_B3_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ2 B4 Volume", MADERA_EQ2_2, MADERA_EQ2_B4_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ2 B5 Volume", MADERA_EQ2_2, MADERA_EQ2_B5_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),

MADERA_EQ_CONTROL("EQ3 Coefficients", MADERA_EQ3_2),
SOC_SINGLE_TLV("EQ3 B1 Volume", MADERA_EQ3_1, MADERA_EQ3_B1_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ3 B2 Volume", MADERA_EQ3_1, MADERA_EQ3_B2_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ3 B3 Volume", MADERA_EQ3_1, MADERA_EQ3_B3_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ3 B4 Volume", MADERA_EQ3_2, MADERA_EQ3_B4_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ3 B5 Volume", MADERA_EQ3_2, MADERA_EQ3_B5_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),

MADERA_EQ_CONTROL("EQ4 Coefficients", MADERA_EQ4_2),
SOC_SINGLE_TLV("EQ4 B1 Volume", MADERA_EQ4_1, MADERA_EQ4_B1_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ4 B2 Volume", MADERA_EQ4_1, MADERA_EQ4_B2_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ4 B3 Volume", MADERA_EQ4_1, MADERA_EQ4_B3_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ4 B4 Volume", MADERA_EQ4_2, MADERA_EQ4_B4_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),
SOC_SINGLE_TLV("EQ4 B5 Volume", MADERA_EQ4_2, MADERA_EQ4_B5_GAIN_SHIFT,
	       24, 0, madera_eq_tlv),

SOC_SINGLE("DAC High Performance Mode Switch", MADERA_OUTPUT_RATE_1,
	   MADERA_CP_DAC_MODE_SHIFT, 1, 0),

MADERA_MIXER_CONTROLS("DRC1L", MADERA_DRC1LMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("DRC1R", MADERA_DRC1RMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("DRC2L", MADERA_DRC2LMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("DRC2R", MADERA_DRC2RMIX_INPUT_1_SOURCE),

SND_SOC_BYTES_MASK("DRC1", MADERA_DRC1_CTRL1, 5,
		   MADERA_DRC1R_ENA | MADERA_DRC1L_ENA),
SND_SOC_BYTES_MASK("DRC2", MADERA_DRC2_CTRL1, 5,
		   MADERA_DRC2R_ENA | MADERA_DRC2L_ENA),

MADERA_MIXER_CONTROLS("LHPF1", MADERA_HPLP1MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("LHPF2", MADERA_HPLP2MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("LHPF3", MADERA_HPLP3MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("LHPF4", MADERA_HPLP4MIX_INPUT_1_SOURCE),

MADERA_LHPF_CONTROL("LHPF1 Coefficients", MADERA_HPLPF1_2),
MADERA_LHPF_CONTROL("LHPF2 Coefficients", MADERA_HPLPF2_2),
MADERA_LHPF_CONTROL("LHPF3 Coefficients", MADERA_HPLPF3_2),
MADERA_LHPF_CONTROL("LHPF4 Coefficients", MADERA_HPLPF4_2),

SOC_ENUM("LHPF1 Mode", madera_lhpf1_mode),
SOC_ENUM("LHPF2 Mode", madera_lhpf2_mode),
SOC_ENUM("LHPF3 Mode", madera_lhpf3_mode),
SOC_ENUM("LHPF4 Mode", madera_lhpf4_mode),

SOC_ENUM("Sample Rate 2", madera_sample_rate[0]),
SOC_ENUM("Sample Rate 3", madera_sample_rate[1]),
SOC_ENUM("ASYNC Sample Rate 2", madera_sample_rate[2]),

MADERA_RATE_ENUM("FX Rate", madera_fx_rate),

MADERA_RATE_ENUM("ISRC1 FSL", madera_isrc_fsl[0]),
MADERA_RATE_ENUM("ISRC2 FSL", madera_isrc_fsl[1]),
MADERA_RATE_ENUM("ISRC1 FSH", madera_isrc_fsh[0]),
MADERA_RATE_ENUM("ISRC2 FSH", madera_isrc_fsh[1]),
MADERA_RATE_ENUM("ASRC1 Rate 1", madera_asrc1_bidir_rate[0]),
MADERA_RATE_ENUM("ASRC1 Rate 2", madera_asrc1_bidir_rate[1]),

SOC_ENUM("AUXPDM1 Rate", cs47l92_auxpdm_freq_enum),

WM_ADSP2_PRELOAD_SWITCH("DSP1", 1),

MADERA_MIXER_CONTROLS("DSP1L", MADERA_DSP1LMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("DSP1R", MADERA_DSP1RMIX_INPUT_1_SOURCE),

SOC_SINGLE_TLV("Noise Generator Volume", MADERA_COMFORT_NOISE_GENERATOR,
	       MADERA_NOISE_GEN_GAIN_SHIFT, 0x16, 0, madera_noise_tlv),

MADERA_MIXER_CONTROLS("HPOUT1L", MADERA_OUT1LMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("HPOUT1R", MADERA_OUT1RMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("HPOUT2L", MADERA_OUT2LMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("HPOUT2R", MADERA_OUT2RMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("HPOUT3L", MADERA_OUT3LMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("HPOUT3R", MADERA_OUT3RMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SPKDAT1L", MADERA_OUT5LMIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SPKDAT1R", MADERA_OUT5RMIX_INPUT_1_SOURCE),

SOC_SINGLE("HPOUT1 SC Protect Switch", MADERA_HP1_SHORT_CIRCUIT_CTRL,
	   MADERA_HP1_SC_ENA_SHIFT, 1, 0),
SOC_SINGLE("HPOUT2 SC Protect Switch", MADERA_HP2_SHORT_CIRCUIT_CTRL,
	   MADERA_HP2_SC_ENA_SHIFT, 1, 0),
SOC_SINGLE("HPOUT3 SC Protect Switch", MADERA_HP3_SHORT_CIRCUIT_CTRL,
	   MADERA_HP3_SC_ENA_SHIFT, 1, 0),

SOC_SINGLE("SPKDAT1 High Performance Switch", MADERA_OUTPUT_PATH_CONFIG_5L,
	   MADERA_OUT5_OSR_SHIFT, 1, 0),

SOC_DOUBLE_R("HPOUT1 Digital Switch", MADERA_DAC_DIGITAL_VOLUME_1L,
	     MADERA_DAC_DIGITAL_VOLUME_1R, MADERA_OUT1L_MUTE_SHIFT, 1, 1),
SOC_DOUBLE_R("HPOUT2 Digital Switch", MADERA_DAC_DIGITAL_VOLUME_2L,
	     MADERA_DAC_DIGITAL_VOLUME_2R, MADERA_OUT2L_MUTE_SHIFT, 1, 1),
SOC_DOUBLE_R("HPOUT3 Digital Switch", MADERA_DAC_DIGITAL_VOLUME_3L,
	     MADERA_DAC_DIGITAL_VOLUME_3R, MADERA_OUT3L_MUTE_SHIFT, 1, 1),
SOC_DOUBLE_R("SPKDAT1 Digital Switch", MADERA_DAC_DIGITAL_VOLUME_5L,
	     MADERA_DAC_DIGITAL_VOLUME_5R, MADERA_OUT5L_MUTE_SHIFT, 1, 1),

SOC_SINGLE_EXT("HPOUT1 Internal Ground Switch", MADERA_OUTPUT_PATH_CONFIG_1,
	       0, 1, 0,
	       madera_internal_gnd_get, madera_internal_gnd_put),
SOC_SINGLE_EXT("HPOUT2 Internal Ground Switch", MADERA_OUTPUT_PATH_CONFIG_2,
	       1, 1, 0,
	       madera_internal_gnd_get, madera_internal_gnd_put),
SOC_SINGLE_EXT("HPOUT3 Internal Ground Switch", MADERA_OUTPUT_PATH_CONFIG_3,
	       2, 1, 0,
	       madera_internal_gnd_get, madera_internal_gnd_put),

SOC_DOUBLE_R_TLV("HPOUT1 Digital Volume", MADERA_DAC_DIGITAL_VOLUME_1L,
		 MADERA_DAC_DIGITAL_VOLUME_1R, MADERA_OUT1L_VOL_SHIFT,
		 0xbf, 0, madera_digital_tlv),
SOC_DOUBLE_R_TLV("HPOUT2 Digital Volume", MADERA_DAC_DIGITAL_VOLUME_2L,
		 MADERA_DAC_DIGITAL_VOLUME_2R, MADERA_OUT2L_VOL_SHIFT,
		 0xbf, 0, madera_digital_tlv),
SOC_DOUBLE_R_TLV("HPOUT3 Digital Volume", MADERA_DAC_DIGITAL_VOLUME_3L,
		 MADERA_DAC_DIGITAL_VOLUME_3R, MADERA_OUT3L_VOL_SHIFT,
		 0xbf, 0, madera_digital_tlv),
SOC_DOUBLE_R_TLV("SPKDAT1 Digital Volume", MADERA_DAC_DIGITAL_VOLUME_5L,
		 MADERA_DAC_DIGITAL_VOLUME_5R, MADERA_OUT5L_VOL_SHIFT,
		 0xbf, 0, madera_digital_tlv),

SOC_DOUBLE("SPKDAT1 Switch", MADERA_PDM_SPK1_CTRL_1, MADERA_SPK1L_MUTE_SHIFT,
	   MADERA_SPK1R_MUTE_SHIFT, 1, 1),

SOC_DOUBLE_EXT("HPOUT1 DRE Switch", MADERA_DRE_ENABLE,
	       MADERA_DRE1L_ENA_SHIFT, MADERA_DRE1R_ENA_SHIFT, 1, 0,
	       snd_soc_get_volsw, madera_dre_put),
SOC_DOUBLE_EXT("HPOUT2 DRE Switch", MADERA_DRE_ENABLE,
	       MADERA_DRE2L_ENA_SHIFT, MADERA_DRE2R_ENA_SHIFT, 1, 0,
	       snd_soc_get_volsw, madera_dre_put),
SOC_DOUBLE_EXT("HPOUT3 DRE Switch", MADERA_DRE_ENABLE,
	       MADERA_DRE3L_ENA_SHIFT, MADERA_DRE3R_ENA_SHIFT, 1, 0,
	       snd_soc_get_volsw, madera_dre_put),

SOC_DOUBLE("HPOUT1 EDRE Switch", MADERA_EDRE_ENABLE,
	   MADERA_EDRE_OUT1L_THR1_ENA_SHIFT,
	   MADERA_EDRE_OUT1R_THR1_ENA_SHIFT, 1, 0),
SOC_DOUBLE("HPOUT2 EDRE Switch", MADERA_EDRE_ENABLE,
	   MADERA_EDRE_OUT2L_THR1_ENA_SHIFT,
	   MADERA_EDRE_OUT2R_THR1_ENA_SHIFT, 1, 0),
SOC_DOUBLE("HPOUT3 EDRE Switch", MADERA_EDRE_ENABLE,
	   MADERA_EDRE_OUT3L_THR1_ENA_SHIFT,
	   MADERA_EDRE_OUT3R_THR1_ENA_SHIFT, 1, 0),

SOC_ENUM("Output Ramp Up", madera_out_vi_ramp),
SOC_ENUM("Output Ramp Down", madera_out_vd_ramp),

MADERA_RATE_ENUM("SPDIF1 Rate", madera_spdif_rate),

SOC_SINGLE("Noise Gate Switch", MADERA_NOISE_GATE_CONTROL,
	   MADERA_NGATE_ENA_SHIFT, 1, 0),
SOC_SINGLE_TLV("Noise Gate Threshold Volume", MADERA_NOISE_GATE_CONTROL,
	       MADERA_NGATE_THR_SHIFT, 7, 1, madera_ng_tlv),
SOC_ENUM("Noise Gate Hold", madera_ng_hold),

MADERA_RATE_ENUM("Output Rate 1", madera_output_ext_rate),

SOC_ENUM_EXT("IN1L Rate", madera_input_rate[0],
	snd_soc_get_enum_double, madera_in_rate_put),
SOC_ENUM_EXT("IN1R Rate", madera_input_rate[1],
	snd_soc_get_enum_double, madera_in_rate_put),
SOC_ENUM_EXT("IN2L Rate", madera_input_rate[2],
	snd_soc_get_enum_double, madera_in_rate_put),
SOC_ENUM_EXT("IN2R Rate", madera_input_rate[3],
	snd_soc_get_enum_double, madera_in_rate_put),
SOC_ENUM_EXT("IN3L Rate", madera_input_rate[4],
	snd_soc_get_enum_double, madera_in_rate_put),
SOC_ENUM_EXT("IN3R Rate", madera_input_rate[5],
	snd_soc_get_enum_double, madera_in_rate_put),
SOC_ENUM_EXT("IN4L Rate", madera_input_rate[6],
	snd_soc_get_enum_double, madera_in_rate_put),
SOC_ENUM_EXT("IN4R Rate", madera_input_rate[7],
	snd_soc_get_enum_double, madera_in_rate_put),

SOC_ENUM_EXT("DFC1RX Width", madera_dfc_width[0],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC1RX Type", madera_dfc_type[0],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC1TX Width", madera_dfc_width[1],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC1TX Type", madera_dfc_type[1],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC2RX Width", madera_dfc_width[2],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC2RX Type", madera_dfc_type[2],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC2TX Width", madera_dfc_width[3],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC2TX Type", madera_dfc_type[3],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC3RX Width", madera_dfc_width[4],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC3RX Type", madera_dfc_type[4],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC3TX Width", madera_dfc_width[5],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC3TX Type", madera_dfc_type[5],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC4RX Width", madera_dfc_width[6],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC4RX Type", madera_dfc_type[6],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC4TX Width", madera_dfc_width[7],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC4TX Type", madera_dfc_type[7],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC5RX Width", madera_dfc_width[8],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC5RX Type", madera_dfc_type[8],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC5TX Width", madera_dfc_width[9],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC5TX Type", madera_dfc_type[9],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC6RX Width", madera_dfc_width[10],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC6RX Type", madera_dfc_type[10],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC6TX Width", madera_dfc_width[11],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC6TX Type", madera_dfc_type[11],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC7RX Width", madera_dfc_width[12],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC7RX Type", madera_dfc_type[12],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC7TX Width", madera_dfc_width[13],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC7TX Type", madera_dfc_type[13],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC8RX Width", madera_dfc_width[14],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC8RX Type", madera_dfc_type[14],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC8TX Width", madera_dfc_width[15],
	snd_soc_get_enum_double, madera_dfc_put),
SOC_ENUM_EXT("DFC8TX Type", madera_dfc_type[15],
	snd_soc_get_enum_double, madera_dfc_put),

CS47L92_NG_SRC("HPOUT1L", MADERA_NOISE_GATE_SELECT_1L),
CS47L92_NG_SRC("HPOUT1R", MADERA_NOISE_GATE_SELECT_1R),
CS47L92_NG_SRC("HPOUT2L", MADERA_NOISE_GATE_SELECT_2L),
CS47L92_NG_SRC("HPOUT2R", MADERA_NOISE_GATE_SELECT_2R),
CS47L92_NG_SRC("HPOUT3L", MADERA_NOISE_GATE_SELECT_3L),
CS47L92_NG_SRC("HPOUT3R", MADERA_NOISE_GATE_SELECT_3R),
CS47L92_NG_SRC("SPKDAT1L", MADERA_NOISE_GATE_SELECT_5L),
CS47L92_NG_SRC("SPKDAT1R", MADERA_NOISE_GATE_SELECT_5R),

MADERA_MIXER_CONTROLS("AIF1TX1", MADERA_AIF1TX1MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF1TX2", MADERA_AIF1TX2MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF1TX3", MADERA_AIF1TX3MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF1TX4", MADERA_AIF1TX4MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF1TX5", MADERA_AIF1TX5MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF1TX6", MADERA_AIF1TX6MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF1TX7", MADERA_AIF1TX7MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF1TX8", MADERA_AIF1TX8MIX_INPUT_1_SOURCE),

MADERA_MIXER_CONTROLS("AIF2TX1", MADERA_AIF2TX1MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF2TX2", MADERA_AIF2TX2MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF2TX3", MADERA_AIF2TX3MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF2TX4", MADERA_AIF2TX4MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF2TX5", MADERA_AIF2TX5MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF2TX6", MADERA_AIF2TX6MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF2TX7", MADERA_AIF2TX7MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF2TX8", MADERA_AIF2TX8MIX_INPUT_1_SOURCE),

MADERA_MIXER_CONTROLS("AIF3TX1", MADERA_AIF3TX1MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF3TX2", MADERA_AIF3TX2MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF3TX3", MADERA_AIF3TX3MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("AIF3TX4", MADERA_AIF3TX4MIX_INPUT_1_SOURCE),

MADERA_MIXER_CONTROLS("SLIMTX1", MADERA_SLIMTX1MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SLIMTX2", MADERA_SLIMTX2MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SLIMTX3", MADERA_SLIMTX3MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SLIMTX4", MADERA_SLIMTX4MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SLIMTX5", MADERA_SLIMTX5MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SLIMTX6", MADERA_SLIMTX6MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SLIMTX7", MADERA_SLIMTX7MIX_INPUT_1_SOURCE),
MADERA_MIXER_CONTROLS("SLIMTX8", MADERA_SLIMTX8MIX_INPUT_1_SOURCE),

MADERA_GAINMUX_CONTROLS("SPDIFTX1", MADERA_SPDIF1TX1MIX_INPUT_1_SOURCE),
MADERA_GAINMUX_CONTROLS("SPDIFTX2", MADERA_SPDIF1TX2MIX_INPUT_1_SOURCE),
};

MADERA_MIXER_ENUMS(EQ1, MADERA_EQ1MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(EQ2, MADERA_EQ2MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(EQ3, MADERA_EQ3MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(EQ4, MADERA_EQ4MIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(DRC1L, MADERA_DRC1LMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(DRC1R, MADERA_DRC1RMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(DRC2L, MADERA_DRC2LMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(DRC2R, MADERA_DRC2RMIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(LHPF1, MADERA_HPLP1MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(LHPF2, MADERA_HPLP2MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(LHPF3, MADERA_HPLP3MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(LHPF4, MADERA_HPLP4MIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(DSP1L, MADERA_DSP1LMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(DSP1R, MADERA_DSP1RMIX_INPUT_1_SOURCE);
MADERA_DSP_AUX_ENUMS(DSP1, MADERA_DSP1AUX1MIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(PWM1, MADERA_PWM1MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(PWM2, MADERA_PWM2MIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(OUT1L, MADERA_OUT1LMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(OUT1R, MADERA_OUT1RMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(OUT2L, MADERA_OUT2LMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(OUT2R, MADERA_OUT2RMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(OUT3L, MADERA_OUT3LMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(OUT3R, MADERA_OUT3RMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SPKDAT1L, MADERA_OUT5LMIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SPKDAT1R, MADERA_OUT5RMIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(AIF1TX1, MADERA_AIF1TX1MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF1TX2, MADERA_AIF1TX2MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF1TX3, MADERA_AIF1TX3MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF1TX4, MADERA_AIF1TX4MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF1TX5, MADERA_AIF1TX5MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF1TX6, MADERA_AIF1TX6MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF1TX7, MADERA_AIF1TX7MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF1TX8, MADERA_AIF1TX8MIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(AIF2TX1, MADERA_AIF2TX1MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF2TX2, MADERA_AIF2TX2MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF2TX3, MADERA_AIF2TX3MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF2TX4, MADERA_AIF2TX4MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF2TX5, MADERA_AIF2TX5MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF2TX6, MADERA_AIF2TX6MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF2TX7, MADERA_AIF2TX7MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF2TX8, MADERA_AIF2TX8MIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(AIF3TX1, MADERA_AIF3TX1MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF3TX2, MADERA_AIF3TX2MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF3TX3, MADERA_AIF3TX3MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(AIF3TX4, MADERA_AIF3TX4MIX_INPUT_1_SOURCE);

MADERA_MIXER_ENUMS(SLIMTX1, MADERA_SLIMTX1MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SLIMTX2, MADERA_SLIMTX2MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SLIMTX3, MADERA_SLIMTX3MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SLIMTX4, MADERA_SLIMTX4MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SLIMTX5, MADERA_SLIMTX5MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SLIMTX6, MADERA_SLIMTX6MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SLIMTX7, MADERA_SLIMTX7MIX_INPUT_1_SOURCE);
MADERA_MIXER_ENUMS(SLIMTX8, MADERA_SLIMTX8MIX_INPUT_1_SOURCE);

MADERA_MUX_ENUMS(SPD1TX1, MADERA_SPDIF1TX1MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(SPD1TX2, MADERA_SPDIF1TX2MIX_INPUT_1_SOURCE);

MADERA_MUX_ENUMS(ASRC1IN1L, MADERA_ASRC1_1LMIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(ASRC1IN1R, MADERA_ASRC1_1RMIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(ASRC1IN2L, MADERA_ASRC1_2LMIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(ASRC1IN2R, MADERA_ASRC1_2RMIX_INPUT_1_SOURCE);

MADERA_MUX_ENUMS(ISRC1INT1, MADERA_ISRC1INT1MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(ISRC1INT2, MADERA_ISRC1INT2MIX_INPUT_1_SOURCE);

MADERA_MUX_ENUMS(ISRC1DEC1, MADERA_ISRC1DEC1MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(ISRC1DEC2, MADERA_ISRC1DEC2MIX_INPUT_1_SOURCE);

MADERA_MUX_ENUMS(ISRC2INT1, MADERA_ISRC2INT1MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(ISRC2INT2, MADERA_ISRC2INT2MIX_INPUT_1_SOURCE);

MADERA_MUX_ENUMS(ISRC2DEC1, MADERA_ISRC2DEC1MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(ISRC2DEC2, MADERA_ISRC2DEC2MIX_INPUT_1_SOURCE);

MADERA_MUX_ENUMS(DFC1, MADERA_DFC1MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(DFC2, MADERA_DFC2MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(DFC3, MADERA_DFC3MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(DFC4, MADERA_DFC4MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(DFC5, MADERA_DFC5MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(DFC6, MADERA_DFC6MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(DFC7, MADERA_DFC7MIX_INPUT_1_SOURCE);
MADERA_MUX_ENUMS(DFC8, MADERA_DFC8MIX_INPUT_1_SOURCE);

static const char * const cs47l92_aec_loopback_texts[] = {
	"HPOUT1L", "HPOUT1R", "HPOUT2L", "HPOUT2R", "HPOUT3L", "HPOUT3R",
	"SPKDAT1L", "SPKDAT1R",
};

static const unsigned int cs47l92_aec_loopback_values[] = {
	0, 1, 2, 3, 4, 5, 8, 9
};

static const struct soc_enum cs47l92_aec_loopback =
	SOC_VALUE_ENUM_SINGLE(MADERA_DAC_AEC_CONTROL_1,
			      MADERA_AEC1_LOOPBACK_SRC_SHIFT, 0xf,
			      ARRAY_SIZE(cs47l92_aec_loopback_texts),
			      cs47l92_aec_loopback_texts,
			      cs47l92_aec_loopback_values);

static const struct snd_kcontrol_new cs47l92_aec_loopback_mux =
	SOC_DAPM_ENUM("AEC1 Loopback", cs47l92_aec_loopback);

static const struct snd_soc_dapm_widget cs47l92_dapm_widgets[] = {
SND_SOC_DAPM_SUPPLY("SYSCLK", MADERA_SYSTEM_CLOCK_1, MADERA_SYSCLK_ENA_SHIFT,
		    0, madera_sysclk_ev,
		    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
SND_SOC_DAPM_SUPPLY("ASYNCCLK", MADERA_ASYNC_CLOCK_1,
		    MADERA_ASYNC_CLK_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("OPCLK", MADERA_OUTPUT_SYSTEM_CLOCK,
		    MADERA_OPCLK_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("ASYNCOPCLK", MADERA_OUTPUT_ASYNC_CLOCK,
		    MADERA_OPCLK_ASYNC_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("DSPCLK", MADERA_DSP_CLOCK_1,
		    MADERA_DSP_CLK_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_REGULATOR_SUPPLY("CPVDD1", 20, 0),
SND_SOC_DAPM_REGULATOR_SUPPLY("CPVDD2", 20, 0),
SND_SOC_DAPM_REGULATOR_SUPPLY("MICVDD", 0, SND_SOC_DAPM_REGULATOR_BYPASS),

SND_SOC_DAPM_SUPPLY("MICBIAS1", MADERA_MIC_BIAS_CTRL_1,
		    MADERA_MICB1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("MICBIAS2", MADERA_MIC_BIAS_CTRL_2,
		    MADERA_MICB1_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_SUPPLY("MICBIAS1A", MADERA_MIC_BIAS_CTRL_5,
			MADERA_MICB1A_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("MICBIAS1B", MADERA_MIC_BIAS_CTRL_5,
			MADERA_MICB1B_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("MICBIAS1C", MADERA_MIC_BIAS_CTRL_5,
			MADERA_MICB1C_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("MICBIAS1D", MADERA_MIC_BIAS_CTRL_5,
			MADERA_MICB1D_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_SUPPLY("MICBIAS2A", MADERA_MIC_BIAS_CTRL_6,
			MADERA_MICB2A_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("MICBIAS2B", MADERA_MIC_BIAS_CTRL_6,
			MADERA_MICB2B_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_SUPPLY("FXCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_FX, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("ASRC1CLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_ASRC1, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("ISRC1CLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_ISRC1, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("ISRC2CLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_ISRC2, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("OUTCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_OUT, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("SPDCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_SPD, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("DSP1CLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_DSP1, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("AIF1TXCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_AIF1, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("AIF2TXCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_AIF2, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("AIF3TXCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_AIF3, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("SLIMBUSCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_SLIMBUS, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("PWMCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_PWM, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("DFCCLK", SND_SOC_NOPM,
		    MADERA_DOM_GRP_DFC, 0,
		    madera_domain_clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_SIGGEN("TONE"),
SND_SOC_DAPM_SIGGEN("NOISE"),

SND_SOC_DAPM_INPUT("IN1AL"),
SND_SOC_DAPM_INPUT("IN1BL"),
SND_SOC_DAPM_INPUT("IN1AR"),
SND_SOC_DAPM_INPUT("IN1BR"),
SND_SOC_DAPM_INPUT("IN2AL"),
SND_SOC_DAPM_INPUT("IN2BL"),
SND_SOC_DAPM_INPUT("IN2AR"),
SND_SOC_DAPM_INPUT("IN2BR"),
SND_SOC_DAPM_INPUT("IN3L"),
SND_SOC_DAPM_INPUT("IN3R"),
SND_SOC_DAPM_INPUT("IN4L"),
SND_SOC_DAPM_INPUT("IN4R"),

SND_SOC_DAPM_DEMUX("OUT3 Demux", SND_SOC_NOPM, 0, 0, &cs47l92_outdemux),

SND_SOC_DAPM_OUTPUT("DRC1 Signal Activity"),
SND_SOC_DAPM_OUTPUT("DRC2 Signal Activity"),

SND_SOC_DAPM_MUX("IN1L Mux", SND_SOC_NOPM, 0, 0, &madera_inmux[0]),
SND_SOC_DAPM_MUX("IN1R Mux", SND_SOC_NOPM, 0, 0, &madera_inmux[1]),
SND_SOC_DAPM_MUX("IN2L Mux", SND_SOC_NOPM, 0, 0, &madera_inmux[2]),
SND_SOC_DAPM_MUX("IN2R Mux", SND_SOC_NOPM, 0, 0, &madera_inmux[3]),

SND_SOC_DAPM_PGA("PWM1 Driver", MADERA_PWM_DRIVE_1, MADERA_PWM1_ENA_SHIFT,
		 0, NULL, 0),
SND_SOC_DAPM_PGA("PWM2 Driver", MADERA_PWM_DRIVE_1, MADERA_PWM2_ENA_SHIFT,
		 0, NULL, 0),

SND_SOC_DAPM_AIF_OUT("AIF1TX1", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF1TX2", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF1TX3", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF1TX4", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX4_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF1TX5", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX5_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF1TX6", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX6_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF1TX7", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX7_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF1TX8", NULL, 0,
		     MADERA_AIF1_TX_ENABLES, MADERA_AIF1TX8_ENA_SHIFT, 0),

SND_SOC_DAPM_AIF_OUT("AIF2TX1", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF2TX2", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF2TX3", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF2TX4", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX4_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF2TX5", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX5_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF2TX6", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX6_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF2TX7", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX7_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF2TX8", NULL, 0,
		     MADERA_AIF2_TX_ENABLES, MADERA_AIF2TX8_ENA_SHIFT, 0),

SND_SOC_DAPM_AIF_OUT("SLIMTX1", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("SLIMTX2", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("SLIMTX3", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("SLIMTX4", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX4_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("SLIMTX5", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX5_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("SLIMTX6", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX6_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("SLIMTX7", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX7_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("SLIMTX8", NULL, 0,
		     MADERA_SLIMBUS_TX_CHANNEL_ENABLE,
		     MADERA_SLIMTX8_ENA_SHIFT, 0),

SND_SOC_DAPM_AIF_OUT("AIF3TX1", NULL, 0,
		     MADERA_AIF3_TX_ENABLES, MADERA_AIF3TX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF3TX2", NULL, 0,
		     MADERA_AIF3_TX_ENABLES, MADERA_AIF3TX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF3TX3", NULL, 0,
		     MADERA_AIF3_TX_ENABLES, MADERA_AIF3TX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_OUT("AIF3TX4", NULL, 0,
		     MADERA_AIF3_TX_ENABLES, MADERA_AIF3TX4_ENA_SHIFT, 0),

SND_SOC_DAPM_PGA_E("OUT1L", SND_SOC_NOPM,
		   MADERA_OUT1L_ENA_SHIFT, 0, NULL, 0, madera_hp_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("OUT1R", SND_SOC_NOPM,
		   MADERA_OUT1R_ENA_SHIFT, 0, NULL, 0, madera_hp_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("OUT2L", SND_SOC_NOPM,
		   MADERA_OUT2L_ENA_SHIFT, 0, NULL, 0, madera_hp_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("OUT2R", SND_SOC_NOPM,
		   MADERA_OUT2R_ENA_SHIFT, 0, NULL, 0, madera_hp_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("OUT3L", SND_SOC_NOPM,
		   MADERA_OUT3L_ENA_SHIFT, 0, NULL, 0, madera_hp_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("OUT3R", SND_SOC_NOPM,
		   MADERA_OUT3R_ENA_SHIFT, 0, NULL, 0, madera_hp_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("OUT5L", MADERA_OUTPUT_ENABLES_1,
		   MADERA_OUT5L_ENA_SHIFT, 0, NULL, 0, madera_out_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("OUT5R", MADERA_OUTPUT_ENABLES_1,
		   MADERA_OUT5R_ENA_SHIFT, 0, NULL, 0, madera_out_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),

SND_SOC_DAPM_PGA("SPD1TX1", MADERA_SPD1_TX_CONTROL,
		   MADERA_SPD1_VAL1_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("SPD1TX2", MADERA_SPD1_TX_CONTROL,
		   MADERA_SPD1_VAL2_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_OUT_DRV("SPD1", MADERA_SPD1_TX_CONTROL,
		     MADERA_SPD1_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_SWITCH("AUXPDM1 Output", MADERA_AUXPDM1_CTRL_0,
		    MADERA_AUXPDM1_ENABLE_SHIFT, 0, &cs47l92_auxpdm1_switch),

/*
 * mux_in widgets : arranged in the order of sources
 * specified in MADERA_MIXER_INPUT_ROUTES
 */

SND_SOC_DAPM_PGA("Noise Generator", MADERA_COMFORT_NOISE_GENERATOR,
		 MADERA_NOISE_GEN_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_PGA("Tone Generator 1", MADERA_TONE_GENERATOR_1,
		 MADERA_TONE1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("Tone Generator 2", MADERA_TONE_GENERATOR_1,
		 MADERA_TONE2_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_SIGGEN("HAPTICS"),

SND_SOC_DAPM_MUX("AEC1 Loopback", MADERA_DAC_AEC_CONTROL_1,
		 MADERA_AEC1_LOOPBACK_ENA_SHIFT, 0,
		 &cs47l92_aec_loopback_mux),

SND_SOC_DAPM_PGA_E("IN1L PGA", MADERA_INPUT_ENABLES, MADERA_IN1L_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("IN1R PGA", MADERA_INPUT_ENABLES, MADERA_IN1R_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("IN2L PGA", MADERA_INPUT_ENABLES, MADERA_IN2L_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("IN2R PGA", MADERA_INPUT_ENABLES, MADERA_IN2R_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("IN3L PGA", MADERA_INPUT_ENABLES, MADERA_IN3L_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("IN3R PGA", MADERA_INPUT_ENABLES, MADERA_IN3R_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("IN4L PGA", MADERA_INPUT_ENABLES, MADERA_IN4L_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
SND_SOC_DAPM_PGA_E("IN4R PGA", MADERA_INPUT_ENABLES, MADERA_IN4R_ENA_SHIFT,
		   0, NULL, 0, madera_in_ev,
		   SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD |
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),

SND_SOC_DAPM_MUX("AUXPDM1 Input", SND_SOC_NOPM, 0, 0, &cs47l92_auxpdm1_inmux),

SND_SOC_DAPM_AIF_IN("AIF1RX1", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX2", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX3", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX4", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX4_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX5", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX5_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX6", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX6_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX7", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX7_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF1RX8", NULL, 0,
			MADERA_AIF1_RX_ENABLES, MADERA_AIF1RX8_ENA_SHIFT, 0),

SND_SOC_DAPM_AIF_IN("AIF2RX1", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF2RX2", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF2RX3", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF2RX4", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX4_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF2RX5", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX5_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF2RX6", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX6_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF2RX7", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX7_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF2RX8", NULL, 0,
			MADERA_AIF2_RX_ENABLES, MADERA_AIF2RX8_ENA_SHIFT, 0),

SND_SOC_DAPM_AIF_IN("AIF3RX1", NULL, 0,
			MADERA_AIF3_RX_ENABLES, MADERA_AIF3RX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF3RX2", NULL, 0,
			MADERA_AIF3_RX_ENABLES, MADERA_AIF3RX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF3RX3", NULL, 0,
			MADERA_AIF3_RX_ENABLES, MADERA_AIF3RX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("AIF3RX4", NULL, 0,
			MADERA_AIF3_RX_ENABLES, MADERA_AIF3RX4_ENA_SHIFT, 0),

SND_SOC_DAPM_AIF_IN("SLIMRX1", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX1_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("SLIMRX2", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX2_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("SLIMRX3", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX3_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("SLIMRX4", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX4_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("SLIMRX5", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX5_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("SLIMRX6", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX6_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("SLIMRX7", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX7_ENA_SHIFT, 0),
SND_SOC_DAPM_AIF_IN("SLIMRX8", NULL, 0,
			MADERA_SLIMBUS_RX_CHANNEL_ENABLE,
			MADERA_SLIMRX8_ENA_SHIFT, 0),

SND_SOC_DAPM_PGA("EQ1", MADERA_EQ1_1, MADERA_EQ1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("EQ2", MADERA_EQ2_1, MADERA_EQ2_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("EQ3", MADERA_EQ3_1, MADERA_EQ3_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("EQ4", MADERA_EQ4_1, MADERA_EQ4_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_PGA("DRC1L", MADERA_DRC1_CTRL1, MADERA_DRC1L_ENA_SHIFT, 0,
		 NULL, 0),
SND_SOC_DAPM_PGA("DRC1R", MADERA_DRC1_CTRL1, MADERA_DRC1R_ENA_SHIFT, 0,
		 NULL, 0),
SND_SOC_DAPM_PGA("DRC2L", MADERA_DRC2_CTRL1, MADERA_DRC2L_ENA_SHIFT, 0,
		 NULL, 0),
SND_SOC_DAPM_PGA("DRC2R", MADERA_DRC2_CTRL1, MADERA_DRC2R_ENA_SHIFT, 0,
		 NULL, 0),

SND_SOC_DAPM_PGA("LHPF1", MADERA_HPLPF1_1, MADERA_LHPF1_ENA_SHIFT, 0,
		 NULL, 0),
SND_SOC_DAPM_PGA("LHPF2", MADERA_HPLPF2_1, MADERA_LHPF2_ENA_SHIFT, 0,
		 NULL, 0),
SND_SOC_DAPM_PGA("LHPF3", MADERA_HPLPF3_1, MADERA_LHPF3_ENA_SHIFT, 0,
		 NULL, 0),
SND_SOC_DAPM_PGA("LHPF4", MADERA_HPLPF4_1, MADERA_LHPF4_ENA_SHIFT, 0,
		 NULL, 0),

SND_SOC_DAPM_PGA_E("ASRC1IN1L", MADERA_ASRC1_ENABLE,
		   MADERA_ASRC1_IN1L_ENA_SHIFT, 0, NULL, 0,
		   cs47l92_asrc_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("ASRC1IN1R", MADERA_ASRC1_ENABLE,
		   MADERA_ASRC1_IN1R_ENA_SHIFT, 0, NULL, 0,
		   cs47l92_asrc_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("ASRC1IN2L", MADERA_ASRC1_ENABLE,
		   MADERA_ASRC1_IN2L_ENA_SHIFT, 0, NULL, 0,
		   cs47l92_asrc_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("ASRC1IN2R", MADERA_ASRC1_ENABLE,
		   MADERA_ASRC1_IN2R_ENA_SHIFT, 0, NULL, 0,
		   cs47l92_asrc_ev, SND_SOC_DAPM_PRE_PMU),

SND_SOC_DAPM_PGA("ISRC1DEC1", MADERA_ISRC_1_CTRL_3,
		 MADERA_ISRC1_DEC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("ISRC1DEC2", MADERA_ISRC_1_CTRL_3,
		 MADERA_ISRC1_DEC2_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_PGA("ISRC1INT1", MADERA_ISRC_1_CTRL_3,
		 MADERA_ISRC1_INT1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("ISRC1INT2", MADERA_ISRC_1_CTRL_3,
		 MADERA_ISRC1_INT2_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_PGA("ISRC2DEC1", MADERA_ISRC_2_CTRL_3,
		 MADERA_ISRC2_DEC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("ISRC2DEC2", MADERA_ISRC_2_CTRL_3,
		 MADERA_ISRC2_DEC2_ENA_SHIFT, 0, NULL, 0),

SND_SOC_DAPM_PGA("ISRC2INT1", MADERA_ISRC_2_CTRL_3,
		 MADERA_ISRC2_INT1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("ISRC2INT2", MADERA_ISRC_2_CTRL_3,
		 MADERA_ISRC2_INT2_ENA_SHIFT, 0, NULL, 0),

WM_ADSP2("DSP1", 0, cs47l92_adsp_power_ev),

/* end of ordered widget list */

SND_SOC_DAPM_PGA("DFC1", MADERA_DFC1_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("DFC2", MADERA_DFC2_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("DFC3", MADERA_DFC3_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("DFC4", MADERA_DFC4_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("DFC5", MADERA_DFC5_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("DFC6", MADERA_DFC6_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("DFC7", MADERA_DFC7_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),
SND_SOC_DAPM_PGA("DFC8", MADERA_DFC8_CTRL, MADERA_DFC1_ENA_SHIFT, 0, NULL, 0),

MADERA_MIXER_WIDGETS(EQ1, "EQ1"),
MADERA_MIXER_WIDGETS(EQ2, "EQ2"),
MADERA_MIXER_WIDGETS(EQ3, "EQ3"),
MADERA_MIXER_WIDGETS(EQ4, "EQ4"),

MADERA_MIXER_WIDGETS(DRC1L, "DRC1L"),
MADERA_MIXER_WIDGETS(DRC1R, "DRC1R"),
MADERA_MIXER_WIDGETS(DRC2L, "DRC2L"),
MADERA_MIXER_WIDGETS(DRC2R, "DRC2R"),

SND_SOC_DAPM_SWITCH("DRC1 Activity Output", SND_SOC_NOPM, 0, 0,
		    &madera_drc_activity_output_mux[0]),
SND_SOC_DAPM_SWITCH("DRC2 Activity Output", SND_SOC_NOPM, 0, 0,
		    &madera_drc_activity_output_mux[1]),

MADERA_MIXER_WIDGETS(LHPF1, "LHPF1"),
MADERA_MIXER_WIDGETS(LHPF2, "LHPF2"),
MADERA_MIXER_WIDGETS(LHPF3, "LHPF3"),
MADERA_MIXER_WIDGETS(LHPF4, "LHPF4"),

MADERA_MIXER_WIDGETS(PWM1, "PWM1"),
MADERA_MIXER_WIDGETS(PWM2, "PWM2"),

MADERA_MIXER_WIDGETS(OUT1L, "HPOUT1L"),
MADERA_MIXER_WIDGETS(OUT1R, "HPOUT1R"),
MADERA_MIXER_WIDGETS(OUT2L, "HPOUT2L"),
MADERA_MIXER_WIDGETS(OUT2R, "HPOUT2R"),
MADERA_MIXER_WIDGETS(OUT3L, "HPOUT3L"),
MADERA_MIXER_WIDGETS(OUT3R, "HPOUT3R"),
MADERA_MIXER_WIDGETS(SPKDAT1L, "SPKDAT1L"),
MADERA_MIXER_WIDGETS(SPKDAT1R, "SPKDAT1R"),

MADERA_MIXER_WIDGETS(AIF1TX1, "AIF1TX1"),
MADERA_MIXER_WIDGETS(AIF1TX2, "AIF1TX2"),
MADERA_MIXER_WIDGETS(AIF1TX3, "AIF1TX3"),
MADERA_MIXER_WIDGETS(AIF1TX4, "AIF1TX4"),
MADERA_MIXER_WIDGETS(AIF1TX5, "AIF1TX5"),
MADERA_MIXER_WIDGETS(AIF1TX6, "AIF1TX6"),
MADERA_MIXER_WIDGETS(AIF1TX7, "AIF1TX7"),
MADERA_MIXER_WIDGETS(AIF1TX8, "AIF1TX8"),

MADERA_MIXER_WIDGETS(AIF2TX1, "AIF2TX1"),
MADERA_MIXER_WIDGETS(AIF2TX2, "AIF2TX2"),
MADERA_MIXER_WIDGETS(AIF2TX3, "AIF2TX3"),
MADERA_MIXER_WIDGETS(AIF2TX4, "AIF2TX4"),
MADERA_MIXER_WIDGETS(AIF2TX5, "AIF2TX5"),
MADERA_MIXER_WIDGETS(AIF2TX6, "AIF2TX6"),
MADERA_MIXER_WIDGETS(AIF2TX7, "AIF2TX7"),
MADERA_MIXER_WIDGETS(AIF2TX8, "AIF2TX8"),

MADERA_MIXER_WIDGETS(AIF3TX1, "AIF3TX1"),
MADERA_MIXER_WIDGETS(AIF3TX2, "AIF3TX2"),
MADERA_MIXER_WIDGETS(AIF3TX3, "AIF3TX3"),
MADERA_MIXER_WIDGETS(AIF3TX4, "AIF3TX4"),

MADERA_MIXER_WIDGETS(SLIMTX1, "SLIMTX1"),
MADERA_MIXER_WIDGETS(SLIMTX2, "SLIMTX2"),
MADERA_MIXER_WIDGETS(SLIMTX3, "SLIMTX3"),
MADERA_MIXER_WIDGETS(SLIMTX4, "SLIMTX4"),
MADERA_MIXER_WIDGETS(SLIMTX5, "SLIMTX5"),
MADERA_MIXER_WIDGETS(SLIMTX6, "SLIMTX6"),
MADERA_MIXER_WIDGETS(SLIMTX7, "SLIMTX7"),
MADERA_MIXER_WIDGETS(SLIMTX8, "SLIMTX8"),

MADERA_MUX_WIDGETS(SPD1TX1, "SPDIFTX1"),
MADERA_MUX_WIDGETS(SPD1TX2, "SPDIFTX2"),

MADERA_MUX_WIDGETS(ASRC1IN1L, "ASRC1IN1L"),
MADERA_MUX_WIDGETS(ASRC1IN1R, "ASRC1IN1R"),
MADERA_MUX_WIDGETS(ASRC1IN2L, "ASRC1IN2L"),
MADERA_MUX_WIDGETS(ASRC1IN2R, "ASRC1IN2R"),

MADERA_DSP_WIDGETS(DSP1, "DSP1"),

MADERA_MUX_WIDGETS(ISRC1DEC1, "ISRC1DEC1"),
MADERA_MUX_WIDGETS(ISRC1DEC2, "ISRC1DEC2"),

MADERA_MUX_WIDGETS(ISRC1INT1, "ISRC1INT1"),
MADERA_MUX_WIDGETS(ISRC1INT2, "ISRC1INT2"),

MADERA_MUX_WIDGETS(ISRC2DEC1, "ISRC2DEC1"),
MADERA_MUX_WIDGETS(ISRC2DEC2, "ISRC2DEC2"),

MADERA_MUX_WIDGETS(ISRC2INT1, "ISRC2INT1"),
MADERA_MUX_WIDGETS(ISRC2INT2, "ISRC2INT2"),

MADERA_MUX_WIDGETS(DFC1, "DFC1"),
MADERA_MUX_WIDGETS(DFC2, "DFC2"),
MADERA_MUX_WIDGETS(DFC3, "DFC3"),
MADERA_MUX_WIDGETS(DFC4, "DFC4"),
MADERA_MUX_WIDGETS(DFC5, "DFC5"),
MADERA_MUX_WIDGETS(DFC6, "DFC6"),
MADERA_MUX_WIDGETS(DFC7, "DFC7"),
MADERA_MUX_WIDGETS(DFC8, "DFC8"),

SND_SOC_DAPM_OUTPUT("HPOUT1L"),
SND_SOC_DAPM_OUTPUT("HPOUT1R"),
SND_SOC_DAPM_OUTPUT("HPOUT2L"),
SND_SOC_DAPM_OUTPUT("HPOUT2R"),
SND_SOC_DAPM_OUTPUT("HPOUT3L"),
SND_SOC_DAPM_OUTPUT("HPOUT3R"),
SND_SOC_DAPM_OUTPUT("HPOUT4L"),
SND_SOC_DAPM_OUTPUT("HPOUT4R"),
SND_SOC_DAPM_OUTPUT("SPKDAT1L"),
SND_SOC_DAPM_OUTPUT("SPKDAT1R"),
SND_SOC_DAPM_OUTPUT("SPDIF1"),
SND_SOC_DAPM_OUTPUT("AUXPDM1"),

SND_SOC_DAPM_OUTPUT("MICSUPP"),
};

#define MADERA_MIXER_INPUT_ROUTES(name)	\
	{ name, "Noise Generator", "Noise Generator" }, \
	{ name, "Tone Generator 1", "Tone Generator 1" }, \
	{ name, "Tone Generator 2", "Tone Generator 2" }, \
	{ name, "Haptics", "HAPTICS" }, \
	{ name, "AEC1", "AEC1 Loopback" }, \
	{ name, "IN1L", "IN1L PGA" }, \
	{ name, "IN1R", "IN1R PGA" }, \
	{ name, "IN2L", "IN2L PGA" }, \
	{ name, "IN2R", "IN2R PGA" }, \
	{ name, "IN3L", "IN3L PGA" }, \
	{ name, "IN3R", "IN3R PGA" }, \
	{ name, "IN4L", "IN4L PGA" }, \
	{ name, "IN4R", "IN4R PGA" }, \
	{ name, "AIF1RX1", "AIF1RX1" }, \
	{ name, "AIF1RX2", "AIF1RX2" }, \
	{ name, "AIF1RX3", "AIF1RX3" }, \
	{ name, "AIF1RX4", "AIF1RX4" }, \
	{ name, "AIF1RX5", "AIF1RX5" }, \
	{ name, "AIF1RX6", "AIF1RX6" }, \
	{ name, "AIF1RX7", "AIF1RX7" }, \
	{ name, "AIF1RX8", "AIF1RX8" }, \
	{ name, "AIF2RX1", "AIF2RX1" }, \
	{ name, "AIF2RX2", "AIF2RX2" }, \
	{ name, "AIF2RX3", "AIF2RX3" }, \
	{ name, "AIF2RX4", "AIF2RX4" }, \
	{ name, "AIF2RX5", "AIF2RX5" }, \
	{ name, "AIF2RX6", "AIF2RX6" }, \
	{ name, "AIF2RX7", "AIF2RX7" }, \
	{ name, "AIF2RX8", "AIF2RX8" }, \
	{ name, "AIF3RX1", "AIF3RX1" }, \
	{ name, "AIF3RX2", "AIF3RX2" }, \
	{ name, "AIF3RX3", "AIF3RX3" }, \
	{ name, "AIF3RX4", "AIF3RX4" }, \
	{ name, "SLIMRX1", "SLIMRX1" }, \
	{ name, "SLIMRX2", "SLIMRX2" }, \
	{ name, "SLIMRX3", "SLIMRX3" }, \
	{ name, "SLIMRX4", "SLIMRX4" }, \
	{ name, "SLIMRX5", "SLIMRX5" }, \
	{ name, "SLIMRX6", "SLIMRX6" }, \
	{ name, "SLIMRX7", "SLIMRX7" }, \
	{ name, "SLIMRX8", "SLIMRX8" }, \
	{ name, "EQ1", "EQ1" }, \
	{ name, "EQ2", "EQ2" }, \
	{ name, "EQ3", "EQ3" }, \
	{ name, "EQ4", "EQ4" }, \
	{ name, "DRC1L", "DRC1L" }, \
	{ name, "DRC1R", "DRC1R" }, \
	{ name, "DRC2L", "DRC2L" }, \
	{ name, "DRC2R", "DRC2R" }, \
	{ name, "LHPF1", "LHPF1" }, \
	{ name, "LHPF2", "LHPF2" }, \
	{ name, "LHPF3", "LHPF3" }, \
	{ name, "LHPF4", "LHPF4" }, \
	{ name, "ASRC1IN1L", "ASRC1IN1L" }, \
	{ name, "ASRC1IN1R", "ASRC1IN1R" }, \
	{ name, "ASRC1IN2L", "ASRC1IN2L" }, \
	{ name, "ASRC1IN2R", "ASRC1IN2R" }, \
	{ name, "ISRC1DEC1", "ISRC1DEC1" }, \
	{ name, "ISRC1DEC2", "ISRC1DEC2" }, \
	{ name, "ISRC1INT1", "ISRC1INT1" }, \
	{ name, "ISRC1INT2", "ISRC1INT2" }, \
	{ name, "ISRC2DEC1", "ISRC2DEC1" }, \
	{ name, "ISRC2DEC2", "ISRC2DEC2" }, \
	{ name, "ISRC2INT1", "ISRC2INT1" }, \
	{ name, "ISRC2INT2", "ISRC2INT2" }, \
	{ name, "DSP1.1", "DSP1" }, \
	{ name, "DSP1.2", "DSP1" }, \
	{ name, "DSP1.3", "DSP1" }, \
	{ name, "DSP1.4", "DSP1" }, \
	{ name, "DSP1.5", "DSP1" }, \
	{ name, "DSP1.6", "DSP1" }, \
	{ name, "DFC1", "DFC1" }, \
	{ name, "DFC2", "DFC2" }, \
	{ name, "DFC3", "DFC3" }, \
	{ name, "DFC4", "DFC4" }, \
	{ name, "DFC5", "DFC5" }, \
	{ name, "DFC6", "DFC6" }, \
	{ name, "DFC7", "DFC7" }, \
	{ name, "DFC8", "DFC8" }

static const struct snd_soc_dapm_route cs47l92_dapm_routes[] = {
	/* Internal clock domains */
	{ "EQ1", NULL, "FXCLK" },
	{ "EQ2", NULL, "FXCLK" },
	{ "EQ3", NULL, "FXCLK" },
	{ "EQ4", NULL, "FXCLK" },
	{ "DRC1L", NULL, "FXCLK" },
	{ "DRC1R", NULL, "FXCLK" },
	{ "DRC2L", NULL, "FXCLK" },
	{ "DRC2R", NULL, "FXCLK" },
	{ "LHPF1", NULL, "FXCLK" },
	{ "LHPF2", NULL, "FXCLK" },
	{ "LHPF3", NULL, "FXCLK" },
	{ "LHPF4", NULL, "FXCLK" },
	{ "PWM1 Mixer", NULL, "PWMCLK" },
	{ "PWM2 Mixer", NULL, "PWMCLK" },
	{ "OUT1L", NULL, "OUTCLK" },
	{ "OUT1R", NULL, "OUTCLK" },
	{ "OUT2L", NULL, "OUTCLK" },
	{ "OUT2R", NULL, "OUTCLK" },
	{ "OUT3L", NULL, "OUTCLK" },
	{ "OUT3R", NULL, "OUTCLK" },
	{ "OUT5L", NULL, "OUTCLK" },
	{ "OUT5R", NULL, "OUTCLK" },
	{ "AIF1TX1", NULL, "AIF1TXCLK" },
	{ "AIF1TX2", NULL, "AIF1TXCLK" },
	{ "AIF1TX3", NULL, "AIF1TXCLK" },
	{ "AIF1TX4", NULL, "AIF1TXCLK" },
	{ "AIF1TX5", NULL, "AIF1TXCLK" },
	{ "AIF1TX6", NULL, "AIF1TXCLK" },
	{ "AIF1TX7", NULL, "AIF1TXCLK" },
	{ "AIF1TX8", NULL, "AIF1TXCLK" },
	{ "AIF2TX1", NULL, "AIF2TXCLK" },
	{ "AIF2TX2", NULL, "AIF2TXCLK" },
	{ "AIF2TX3", NULL, "AIF2TXCLK" },
	{ "AIF2TX4", NULL, "AIF2TXCLK" },
	{ "AIF2TX5", NULL, "AIF2TXCLK" },
	{ "AIF2TX6", NULL, "AIF2TXCLK" },
	{ "AIF2TX7", NULL, "AIF2TXCLK" },
	{ "AIF2TX8", NULL, "AIF2TXCLK" },
	{ "AIF3TX1", NULL, "AIF3TXCLK" },
	{ "AIF3TX2", NULL, "AIF3TXCLK" },
	{ "AIF3TX3", NULL, "AIF3TXCLK" },
	{ "AIF3TX4", NULL, "AIF3TXCLK" },
	{ "SLIMTX1", NULL, "SLIMBUSCLK" },
	{ "SLIMTX2", NULL, "SLIMBUSCLK" },
	{ "SLIMTX3", NULL, "SLIMBUSCLK" },
	{ "SLIMTX4", NULL, "SLIMBUSCLK" },
	{ "SLIMTX5", NULL, "SLIMBUSCLK" },
	{ "SLIMTX6", NULL, "SLIMBUSCLK" },
	{ "SLIMTX7", NULL, "SLIMBUSCLK" },
	{ "SLIMTX8", NULL, "SLIMBUSCLK" },
	{ "SPD1TX1", NULL, "SPDCLK" },
	{ "SPD1TX2", NULL, "SPDCLK" },
	{ "DSP1", NULL, "DSP1CLK" },
	{ "ISRC1DEC1", NULL, "ISRC1CLK" },
	{ "ISRC1DEC2", NULL, "ISRC1CLK" },
	{ "ISRC1INT1", NULL, "ISRC1CLK" },
	{ "ISRC1INT2", NULL, "ISRC1CLK" },
	{ "ISRC2DEC1", NULL, "ISRC2CLK" },
	{ "ISRC2DEC2", NULL, "ISRC2CLK" },
	{ "ISRC2INT1", NULL, "ISRC2CLK" },
	{ "ISRC2INT2", NULL, "ISRC2CLK" },
	{ "ASRC1IN1L", NULL, "ASRC1CLK" },
	{ "ASRC1IN1R", NULL, "ASRC1CLK" },
	{ "ASRC1IN2L", NULL, "ASRC1CLK" },
	{ "ASRC1IN2R", NULL, "ASRC1CLK" },
	{ "DFC1", NULL, "DFCCLK" },
	{ "DFC2", NULL, "DFCCLK" },
	{ "DFC3", NULL, "DFCCLK" },
	{ "DFC4", NULL, "DFCCLK" },
	{ "DFC5", NULL, "DFCCLK" },
	{ "DFC6", NULL, "DFCCLK" },
	{ "DFC7", NULL, "DFCCLK" },
	{ "DFC8", NULL, "DFCCLK" },

	{ "OUT1L", NULL, "CPVDD1" },
	{ "OUT1L", NULL, "CPVDD2" },
	{ "OUT1R", NULL, "CPVDD1" },
	{ "OUT1R", NULL, "CPVDD2" },
	{ "OUT2L", NULL, "CPVDD1" },
	{ "OUT2L", NULL, "CPVDD2" },
	{ "OUT2R", NULL, "CPVDD1" },
	{ "OUT2R", NULL, "CPVDD2" },
	{ "OUT3L", NULL, "CPVDD1" },
	{ "OUT3L", NULL, "CPVDD2" },
	{ "OUT3R", NULL, "CPVDD1" },
	{ "OUT3R", NULL, "CPVDD2" },

	{ "OUT1L", NULL, "SYSCLK" },
	{ "OUT1R", NULL, "SYSCLK" },
	{ "OUT2L", NULL, "SYSCLK" },
	{ "OUT2R", NULL, "SYSCLK" },
	{ "OUT3L", NULL, "SYSCLK" },
	{ "OUT3R", NULL, "SYSCLK" },
	{ "OUT5L", NULL, "SYSCLK" },
	{ "OUT5R", NULL, "SYSCLK" },

	{ "SPD1", NULL, "SYSCLK" },
	{ "SPD1", NULL, "SPD1TX1" },
	{ "SPD1", NULL, "SPD1TX2" },

	{ "IN1AL", NULL, "SYSCLK" },
	{ "IN1BL", NULL, "SYSCLK" },
	{ "IN1AR", NULL, "SYSCLK" },
	{ "IN1BR", NULL, "SYSCLK" },
	{ "IN2AL", NULL, "SYSCLK" },
	{ "IN2BL", NULL, "SYSCLK" },
	{ "IN2AR", NULL, "SYSCLK" },
	{ "IN2BR", NULL, "SYSCLK" },
	{ "IN3L", NULL, "SYSCLK" },
	{ "IN3R", NULL, "SYSCLK" },
	{ "IN4L", NULL, "SYSCLK" },
	{ "IN4R", NULL, "SYSCLK" },

	{ "ASRC1IN1L", NULL, "SYSCLK" },
	{ "ASRC1IN1R", NULL, "SYSCLK" },
	{ "ASRC1IN2L", NULL, "SYSCLK" },
	{ "ASRC1IN2R", NULL, "SYSCLK" },

	{ "ASRC1IN1L", NULL, "ASYNCCLK" },
	{ "ASRC1IN1R", NULL, "ASYNCCLK" },
	{ "ASRC1IN2L", NULL, "ASYNCCLK" },
	{ "ASRC1IN2R", NULL, "ASYNCCLK" },

	{ "MICBIAS1", NULL, "MICVDD" },
	{ "MICBIAS2", NULL, "MICVDD" },

	{ "MICBIAS1A", NULL, "MICBIAS1" },
	{ "MICBIAS1B", NULL, "MICBIAS1" },
	{ "MICBIAS1C", NULL, "MICBIAS1" },
	{ "MICBIAS1D", NULL, "MICBIAS1" },

	{ "MICBIAS2A", NULL, "MICBIAS2" },
	{ "MICBIAS2B", NULL, "MICBIAS2" },

	{ "Noise Generator", NULL, "SYSCLK" },
	{ "Tone Generator 1", NULL, "SYSCLK" },
	{ "Tone Generator 2", NULL, "SYSCLK" },

	{ "Noise Generator", NULL, "NOISE" },
	{ "Tone Generator 1", NULL, "TONE" },
	{ "Tone Generator 2", NULL, "TONE" },

	{ "AIF1 Capture", NULL, "AIF1TX1" },
	{ "AIF1 Capture", NULL, "AIF1TX2" },
	{ "AIF1 Capture", NULL, "AIF1TX3" },
	{ "AIF1 Capture", NULL, "AIF1TX4" },
	{ "AIF1 Capture", NULL, "AIF1TX5" },
	{ "AIF1 Capture", NULL, "AIF1TX6" },
	{ "AIF1 Capture", NULL, "AIF1TX7" },
	{ "AIF1 Capture", NULL, "AIF1TX8" },

	{ "AIF1RX1", NULL, "AIF1 Playback" },
	{ "AIF1RX2", NULL, "AIF1 Playback" },
	{ "AIF1RX3", NULL, "AIF1 Playback" },
	{ "AIF1RX4", NULL, "AIF1 Playback" },
	{ "AIF1RX5", NULL, "AIF1 Playback" },
	{ "AIF1RX6", NULL, "AIF1 Playback" },
	{ "AIF1RX7", NULL, "AIF1 Playback" },
	{ "AIF1RX8", NULL, "AIF1 Playback" },

	{ "AIF2 Capture", NULL, "AIF2TX1" },
	{ "AIF2 Capture", NULL, "AIF2TX2" },
	{ "AIF2 Capture", NULL, "AIF2TX3" },
	{ "AIF2 Capture", NULL, "AIF2TX4" },
	{ "AIF2 Capture", NULL, "AIF2TX5" },
	{ "AIF2 Capture", NULL, "AIF2TX6" },
	{ "AIF2 Capture", NULL, "AIF2TX7" },
	{ "AIF2 Capture", NULL, "AIF2TX8" },

	{ "AIF2RX1", NULL, "AIF2 Playback" },
	{ "AIF2RX2", NULL, "AIF2 Playback" },
	{ "AIF2RX3", NULL, "AIF2 Playback" },
	{ "AIF2RX4", NULL, "AIF2 Playback" },
	{ "AIF2RX5", NULL, "AIF2 Playback" },
	{ "AIF2RX6", NULL, "AIF2 Playback" },
	{ "AIF2RX7", NULL, "AIF2 Playback" },
	{ "AIF2RX8", NULL, "AIF2 Playback" },

	{ "AIF3 Capture", NULL, "AIF3TX1" },
	{ "AIF3 Capture", NULL, "AIF3TX2" },
	{ "AIF3 Capture", NULL, "AIF3TX3" },
	{ "AIF3 Capture", NULL, "AIF3TX4" },

	{ "AIF3RX1", NULL, "AIF3 Playback" },
	{ "AIF3RX2", NULL, "AIF3 Playback" },
	{ "AIF3RX3", NULL, "AIF3 Playback" },
	{ "AIF3RX4", NULL, "AIF3 Playback" },

	{ "Slim1 Capture", NULL, "SLIMTX1" },
	{ "Slim1 Capture", NULL, "SLIMTX2" },
	{ "Slim1 Capture", NULL, "SLIMTX3" },
	{ "Slim1 Capture", NULL, "SLIMTX4" },

	{ "SLIMRX1", NULL, "Slim1 Playback" },
	{ "SLIMRX2", NULL, "Slim1 Playback" },
	{ "SLIMRX3", NULL, "Slim1 Playback" },
	{ "SLIMRX4", NULL, "Slim1 Playback" },

	{ "Slim2 Capture", NULL, "SLIMTX5" },
	{ "Slim2 Capture", NULL, "SLIMTX6" },

	{ "SLIMRX5", NULL, "Slim2 Playback" },
	{ "SLIMRX6", NULL, "Slim2 Playback" },

	{ "Slim3 Capture", NULL, "SLIMTX7" },
	{ "Slim3 Capture", NULL, "SLIMTX8" },

	{ "SLIMRX7", NULL, "Slim3 Playback" },
	{ "SLIMRX8", NULL, "Slim3 Playback" },

	{ "AIF1 Playback", NULL, "SYSCLK" },
	{ "AIF2 Playback", NULL, "SYSCLK" },
	{ "AIF3 Playback", NULL, "SYSCLK" },
	{ "Slim1 Playback", NULL, "SYSCLK" },
	{ "Slim2 Playback", NULL, "SYSCLK" },
	{ "Slim3 Playback", NULL, "SYSCLK" },

	{ "AIF1 Capture", NULL, "SYSCLK" },
	{ "AIF2 Capture", NULL, "SYSCLK" },
	{ "AIF3 Capture", NULL, "SYSCLK" },
	{ "Slim1 Capture", NULL, "SYSCLK" },
	{ "Slim2 Capture", NULL, "SYSCLK" },
	{ "Slim3 Capture", NULL, "SYSCLK" },

	{ "Audio Trace DSP", NULL, "DSP1" },

	{ "IN1L Mux", "A", "IN1AL" },
	{ "IN1L Mux", "B", "IN1BL" },
	{ "IN1R Mux", "A", "IN1AR" },
	{ "IN1R Mux", "B", "IN1BR" },

	{ "IN2L Mux", "A", "IN2AL" },
	{ "IN2L Mux", "B", "IN2BL" },
	{ "IN2R Mux", "A", "IN2AR" },
	{ "IN2R Mux", "B", "IN2BR" },

	{ "IN1L PGA", NULL, "IN1L Mux" },
	{ "IN1R PGA", NULL, "IN1R Mux" },

	{ "IN2L PGA", NULL, "IN2L Mux" },
	{ "IN2R PGA", NULL, "IN2R Mux" },

	{ "IN3L PGA", NULL, "IN3L" },
	{ "IN3R PGA", NULL, "IN3R" },

	{ "IN4L PGA", NULL, "IN4L" },
	{ "IN4R PGA", NULL, "IN4R" },

	MADERA_MIXER_ROUTES("OUT1L", "HPOUT1L"),
	MADERA_MIXER_ROUTES("OUT1R", "HPOUT1R"),
	MADERA_MIXER_ROUTES("OUT2L", "HPOUT2L"),
	MADERA_MIXER_ROUTES("OUT2R", "HPOUT2R"),
	MADERA_MIXER_ROUTES("OUT3L", "HPOUT3L"),
	MADERA_MIXER_ROUTES("OUT3R", "HPOUT3R"),

	MADERA_MIXER_ROUTES("OUT5L", "SPKDAT1L"),
	MADERA_MIXER_ROUTES("OUT5R", "SPKDAT1R"),

	MADERA_MIXER_ROUTES("PWM1 Driver", "PWM1"),
	MADERA_MIXER_ROUTES("PWM2 Driver", "PWM2"),

	MADERA_MIXER_ROUTES("AIF1TX1", "AIF1TX1"),
	MADERA_MIXER_ROUTES("AIF1TX2", "AIF1TX2"),
	MADERA_MIXER_ROUTES("AIF1TX3", "AIF1TX3"),
	MADERA_MIXER_ROUTES("AIF1TX4", "AIF1TX4"),
	MADERA_MIXER_ROUTES("AIF1TX5", "AIF1TX5"),
	MADERA_MIXER_ROUTES("AIF1TX6", "AIF1TX6"),
	MADERA_MIXER_ROUTES("AIF1TX7", "AIF1TX7"),
	MADERA_MIXER_ROUTES("AIF1TX8", "AIF1TX8"),

	MADERA_MIXER_ROUTES("AIF2TX1", "AIF2TX1"),
	MADERA_MIXER_ROUTES("AIF2TX2", "AIF2TX2"),
	MADERA_MIXER_ROUTES("AIF2TX3", "AIF2TX3"),
	MADERA_MIXER_ROUTES("AIF2TX4", "AIF2TX4"),
	MADERA_MIXER_ROUTES("AIF2TX5", "AIF2TX5"),
	MADERA_MIXER_ROUTES("AIF2TX6", "AIF2TX6"),
	MADERA_MIXER_ROUTES("AIF2TX7", "AIF2TX7"),
	MADERA_MIXER_ROUTES("AIF2TX8", "AIF2TX8"),

	MADERA_MIXER_ROUTES("AIF3TX1", "AIF3TX1"),
	MADERA_MIXER_ROUTES("AIF3TX2", "AIF3TX2"),
	MADERA_MIXER_ROUTES("AIF3TX3", "AIF3TX3"),
	MADERA_MIXER_ROUTES("AIF3TX4", "AIF3TX4"),

	MADERA_MIXER_ROUTES("SLIMTX1", "SLIMTX1"),
	MADERA_MIXER_ROUTES("SLIMTX2", "SLIMTX2"),
	MADERA_MIXER_ROUTES("SLIMTX3", "SLIMTX3"),
	MADERA_MIXER_ROUTES("SLIMTX4", "SLIMTX4"),
	MADERA_MIXER_ROUTES("SLIMTX5", "SLIMTX5"),
	MADERA_MIXER_ROUTES("SLIMTX6", "SLIMTX6"),
	MADERA_MIXER_ROUTES("SLIMTX7", "SLIMTX7"),
	MADERA_MIXER_ROUTES("SLIMTX8", "SLIMTX8"),

	MADERA_MUX_ROUTES("SPD1TX1", "SPDIFTX1"),
	MADERA_MUX_ROUTES("SPD1TX2", "SPDIFTX2"),

	MADERA_MIXER_ROUTES("EQ1", "EQ1"),
	MADERA_MIXER_ROUTES("EQ2", "EQ2"),
	MADERA_MIXER_ROUTES("EQ3", "EQ3"),
	MADERA_MIXER_ROUTES("EQ4", "EQ4"),

	MADERA_MIXER_ROUTES("DRC1L", "DRC1L"),
	MADERA_MIXER_ROUTES("DRC1R", "DRC1R"),
	MADERA_MIXER_ROUTES("DRC2L", "DRC2L"),
	MADERA_MIXER_ROUTES("DRC2R", "DRC2R"),

	MADERA_MIXER_ROUTES("LHPF1", "LHPF1"),
	MADERA_MIXER_ROUTES("LHPF2", "LHPF2"),
	MADERA_MIXER_ROUTES("LHPF3", "LHPF3"),
	MADERA_MIXER_ROUTES("LHPF4", "LHPF4"),

	MADERA_MUX_ROUTES("ASRC1IN1L", "ASRC1IN1L"),
	MADERA_MUX_ROUTES("ASRC1IN1R", "ASRC1IN1R"),
	MADERA_MUX_ROUTES("ASRC1IN2L", "ASRC1IN2L"),
	MADERA_MUX_ROUTES("ASRC1IN2R", "ASRC1IN2R"),

	MADERA_DSP_ROUTES("DSP1"),

	MADERA_MUX_ROUTES("ISRC1INT1", "ISRC1INT1"),
	MADERA_MUX_ROUTES("ISRC1INT2", "ISRC1INT2"),

	MADERA_MUX_ROUTES("ISRC1DEC1", "ISRC1DEC1"),
	MADERA_MUX_ROUTES("ISRC1DEC2", "ISRC1DEC2"),

	MADERA_MUX_ROUTES("ISRC2INT1", "ISRC2INT1"),
	MADERA_MUX_ROUTES("ISRC2INT2", "ISRC2INT2"),

	MADERA_MUX_ROUTES("ISRC2DEC1", "ISRC2DEC1"),
	MADERA_MUX_ROUTES("ISRC2DEC2", "ISRC2DEC2"),

	{ "AEC1 Loopback", "HPOUT1L", "OUT1L" },
	{ "AEC1 Loopback", "HPOUT1R", "OUT1R" },
	{ "HPOUT1L", NULL, "OUT1L" },
	{ "HPOUT1R", NULL, "OUT1R" },

	{ "AEC1 Loopback", "HPOUT2L", "OUT2L" },
	{ "AEC1 Loopback", "HPOUT2R", "OUT2R" },
	{ "HPOUT2L", NULL, "OUT2L" },
	{ "HPOUT2R", NULL, "OUT2R" },

	{ "AEC1 Loopback", "HPOUT3L", "OUT3L" },
	{ "AEC1 Loopback", "HPOUT3R", "OUT3R" },
	{ "OUT3 Demux", NULL, "OUT3L" },
	{ "OUT3 Demux", NULL, "OUT3R" },

	{ "HPOUT3L", "HPOUT3", "OUT3 Demux" },
	{ "HPOUT3R", "HPOUT3", "OUT3 Demux" },
	{ "HPOUT4L", "HPOUT4", "OUT3 Demux" },
	{ "HPOUT4R", "HPOUT4", "OUT3 Demux" },

	{ "AEC1 Loopback", "SPKDAT1L", "OUT5L" },
	{ "AEC1 Loopback", "SPKDAT1R", "OUT5R" },
	{ "SPKDAT1L", NULL, "OUT5L" },
	{ "SPKDAT1R", NULL, "OUT5R" },

	{ "SPDIF1", NULL, "SPD1" },

	{ "AUXPDM1 Input", "IN1L", "IN1L PGA" },
	{ "AUXPDM1 Input", "IN1R", "IN1R PGA" },
	{ "AUXPDM1 Input", "IN2L", "IN2L PGA" },
	{ "AUXPDM1 Input", "IN2R", "IN2R PGA" },

	{ "AUXPDM1 Output", "Switch", "AUXPDM1 Input" },
	{ "AUXPDM1", NULL, "AUXPDM1 Output" },

	{ "MICSUPP", NULL, "SYSCLK" },

	{ "DRC1 Signal Activity", NULL, "DRC1 Activity Output" },
	{ "DRC2 Signal Activity", NULL, "DRC2 Activity Output" },
	{ "DRC1 Activity Output", "Switch", "DRC1L" },
	{ "DRC1 Activity Output", "Switch", "DRC1R" },
	{ "DRC2 Activity Output", "Switch", "DRC2L" },
	{ "DRC2 Activity Output", "Switch", "DRC2R" },

	MADERA_MUX_ROUTES("DFC1", "DFC1"),
	MADERA_MUX_ROUTES("DFC2", "DFC2"),
	MADERA_MUX_ROUTES("DFC3", "DFC3"),
	MADERA_MUX_ROUTES("DFC4", "DFC4"),
	MADERA_MUX_ROUTES("DFC5", "DFC5"),
	MADERA_MUX_ROUTES("DFC6", "DFC6"),
	MADERA_MUX_ROUTES("DFC7", "DFC7"),
	MADERA_MUX_ROUTES("DFC8", "DFC8"),
};

static int cs47l92_set_fll(struct snd_soc_codec *codec, int fll_id, int source,
			  unsigned int Fref, unsigned int Fout)
{
	struct cs47l92 *cs47l92 = snd_soc_codec_get_drvdata(codec);

	switch (fll_id) {
	case MADERA_FLL1_REFCLK:
		return madera_fllhj_set_refclk(&cs47l92->fll[0], source, Fref,
					       Fout);
	case MADERA_FLL2_REFCLK:
		return madera_fllhj_set_refclk(&cs47l92->fll[1], source, Fref,
					       Fout);
	default:
		return -EINVAL;
	}
}

static struct snd_soc_dai_driver cs47l92_dai[] = {
	{
		.name = "cs47l92-aif1",
		.id = 1,
		.base = MADERA_AIF1_BCLK_CTRL,
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		 },
		.ops = &madera_dai_ops,
		.symmetric_rates = 1,
		.symmetric_samplebits = 1,
	},
	{
		.name = "cs47l92-aif2",
		.id = 2,
		.base = MADERA_AIF2_BCLK_CTRL,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		 },
		.ops = &madera_dai_ops,
		.symmetric_rates = 1,
		.symmetric_samplebits = 1,
	},
	{
		.name = "cs47l92-aif3",
		.id = 3,
		.base = MADERA_AIF3_BCLK_CTRL,
		.playback = {
			.stream_name = "AIF3 Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
		.capture = {
			.stream_name = "AIF3 Capture",
			.channels_min = 1,
			.channels_max = 4,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		 },
		.ops = &madera_dai_ops,
		.symmetric_rates = 1,
		.symmetric_samplebits = 1,
	},
	{
		.name = "cs47l92-slim1",
		.id = 5,
		.playback = {
			.stream_name = "Slim1 Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
		.capture = {
			.stream_name = "Slim1 Capture",
			.channels_min = 1,
			.channels_max = 4,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		 },
		.ops = &madera_simple_dai_ops,
	},
	{
		.name = "cs47l92-slim2",
		.id = 6,
		.playback = {
			.stream_name = "Slim2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
		.capture = {
			.stream_name = "Slim2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		 },
		.ops = &madera_simple_dai_ops,
	},
	{
		.name = "cs47l92-slim3",
		.id = 7,
		.playback = {
			.stream_name = "Slim3 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
		.capture = {
			.stream_name = "Slim3 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		 },
		.ops = &madera_simple_dai_ops,
	},
	{
		.name = "cs47l92-cpu-trace",
		.capture = {
			.stream_name = "Audio Trace CPU",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
		.compress_new = snd_soc_new_compress,
	},
	{
		.name = "cs47l92-dsp-trace",
		.capture = {
			.stream_name = "Audio Trace DSP",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MADERA_RATES,
			.formats = MADERA_FORMATS,
		},
	},
};


static int cs47l92_open(struct snd_compr_stream *stream)
{
	struct snd_soc_pcm_runtime *rtd = stream->private_data;
	struct cs47l92 *cs47l92 = snd_soc_platform_get_drvdata(rtd->platform);
	struct madera_priv *priv = &cs47l92->core;
	struct madera *madera = priv->madera;
	int n_adsp;

	if (strcmp(rtd->codec_dai->name, "cs47l92-dsp-trace") == 0) {
		n_adsp = 0;
	} else {
		dev_err(madera->dev,
				"No suitable compressed stream for DAI '%s'\n",
				rtd->codec_dai->name);
		return -EINVAL;
	}

	return wm_adsp_compr_open(&priv->adsp[n_adsp], stream);
}

static irqreturn_t cs47l92_adsp2_irq(int irq, void *data)
{
	struct cs47l92 *cs47l92 = data;
	struct madera_priv *priv = &cs47l92->core;
	struct madera *madera = priv->madera;
	int ret;

	ret = wm_adsp_compr_handle_irq(&priv->adsp[0]);
	if (ret == -ENODEV) {
		dev_err(madera->dev, "Spurious compressed data IRQ\n");
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static irqreturn_t cs47l92_dsp_bus_error(int irq, void *data)
{
	struct wm_adsp *adsp = (struct wm_adsp *)data;

	return wm_adsp2_bus_error(adsp);
}

static const char * const cs47l92_dmic_refs[] = {
	"MICVDD",
	"MICBIAS1",
	"MICBIAS2",
	"MICBIAS3",
};

static const char * const cs47l92_dmic_inputs[] = {
	"IN1L Mux",
	"IN1R Mux",
	"IN2L Mux",
	"IN2R Mux",
	"IN3L",
	"IN3R",
	"IN4L",
	"IN4R",
};

static int cs47l92_codec_probe(struct snd_soc_codec *codec)
{
	struct cs47l92 *cs47l92 = snd_soc_codec_get_drvdata(codec);
	struct madera *madera = cs47l92->core.madera;
	struct madera_codec_pdata *pdata = &madera->pdata.codec;
	int ret;
	unsigned int val = 0;

	madera->dapm = snd_soc_codec_get_dapm(codec);

	ret = madera_init_inputs(codec,
				 cs47l92_dmic_inputs,
				 ARRAY_SIZE(cs47l92_dmic_inputs),
				 cs47l92_dmic_refs,
				 ARRAY_SIZE(cs47l92_dmic_refs));
	if (ret)
		return ret;

	if (!pdata->auxpdm_slave_mode)
		val = MADERA_AUXPDM1_MSTR_MASK;
	else
		val = 0;
	if (pdata->auxpdm_falling_edge)
		val |= MADERA_AUXPDM1_TXEDGE_MASK;
	regmap_update_bits(madera->regmap, MADERA_AUXPDM1_CTRL_0,
			   MADERA_AUXPDM1_TXEDGE_MASK |
			   MADERA_AUXPDM1_MSTR_MASK, val);

	ret = madera_init_outputs(codec, CS47L92_MONO_OUTPUTS);
	if (ret)
		return ret;

	ret = madera_init_aif(codec);
	if (ret)
		return ret;

	snd_soc_dapm_disable_pin(madera->dapm, "HAPTICS");

	ret = snd_soc_add_codec_controls(codec, madera_adsp_rate_controls,
					 CS47L92_NUM_ADSP);
	if (ret)
		return ret;

	return wm_adsp2_codec_probe(&cs47l92->core.adsp[0], codec);
}

static int cs47l92_codec_remove(struct snd_soc_codec *codec)
{
	struct cs47l92 *cs47l92 = snd_soc_codec_get_drvdata(codec);

	wm_adsp2_codec_remove(&cs47l92->core.adsp[0], codec);

	cs47l92->core.madera->dapm = NULL;

	return 0;
}

#define CS47L92_DIG_VU 0x0200

static unsigned int cs47l92_digital_vu[] = {
	MADERA_DAC_DIGITAL_VOLUME_1L,
	MADERA_DAC_DIGITAL_VOLUME_1R,
	MADERA_DAC_DIGITAL_VOLUME_2L,
	MADERA_DAC_DIGITAL_VOLUME_2R,
	MADERA_DAC_DIGITAL_VOLUME_3L,
	MADERA_DAC_DIGITAL_VOLUME_3R,
	MADERA_DAC_DIGITAL_VOLUME_5L,
	MADERA_DAC_DIGITAL_VOLUME_5R,
};

static struct regmap *cs47l92_get_regmap(struct device *dev)
{
	struct cs47l92 *cs47l92 = dev_get_drvdata(dev);

	return cs47l92->core.madera->regmap;
}

static const struct snd_soc_codec_driver soc_codec_dev_cs47l92 = {
	.probe = cs47l92_codec_probe,
	.remove = cs47l92_codec_remove,
	.get_regmap = cs47l92_get_regmap,

	.idle_bias_off = true,

	.set_sysclk = madera_set_sysclk,
	.set_pll = cs47l92_set_fll,

	.component_driver = {
		.controls = cs47l92_snd_controls,
		.num_controls = ARRAY_SIZE(cs47l92_snd_controls),
		.dapm_widgets = cs47l92_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(cs47l92_dapm_widgets),
		.dapm_routes = cs47l92_dapm_routes,
		.num_dapm_routes = ARRAY_SIZE(cs47l92_dapm_routes),
	},
};

static const struct snd_compr_ops cs47l92_compr_ops = {
	.open = cs47l92_open,
	.free = wm_adsp_compr_free,
	.set_params = wm_adsp_compr_set_params,
	.get_caps = wm_adsp_compr_get_caps,
	.trigger = wm_adsp_compr_trigger,
	.pointer = wm_adsp_compr_pointer,
	.copy = wm_adsp_compr_copy,
};

static const struct snd_soc_platform_driver cs47l92_compr_platform = {
	.compr_ops = &cs47l92_compr_ops,
};

static int cs47l92_probe(struct platform_device *pdev)
{
	struct madera *madera = dev_get_drvdata(pdev->dev.parent);
	struct cs47l92 *cs47l92;
	int i, ret;

	BUILD_BUG_ON(ARRAY_SIZE(cs47l92_dai) > MADERA_MAX_DAI);

	/* quick exit if Madera irqchip driver hasn't completed probe */
	if (!madera->irq_dev) {
		dev_dbg(&pdev->dev, "irqchip driver not ready\n");
		return -EPROBE_DEFER;
	}

	cs47l92 = devm_kzalloc(&pdev->dev, sizeof(struct cs47l92), GFP_KERNEL);
	if (!cs47l92)
		return -ENOMEM;

	platform_set_drvdata(pdev, cs47l92);

	/* Set of_node to parent from the SPI device to allow DAPM to
	 * locate regulator supplies
	 */
	pdev->dev.of_node = madera->dev->of_node;

	cs47l92->core.madera = madera;
	cs47l92->core.dev = &pdev->dev;
	cs47l92->core.num_inputs = 8;

	ret = madera_core_init(&cs47l92->core);
	if (ret)
		return ret;

	ret = madera_request_irq(madera, MADERA_IRQ_DSP_IRQ1,
				 "ADSP2 Compressed IRQ", cs47l92_adsp2_irq,
				 cs47l92);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to request DSP IRQ: %d\n", ret);
		goto error_core;
	}

	ret = madera_set_irq_wake(madera, MADERA_IRQ_DSP_IRQ1, 1);
	if (ret)
		dev_warn(&pdev->dev, "Failed to set DSP IRQ wake: %d\n", ret);

	cs47l92->core.adsp[0].part = "cs47l92";
	cs47l92->core.adsp[0].num = 1;
	cs47l92->core.adsp[0].type = WMFW_ADSP2;
	cs47l92->core.adsp[0].rev = 2;
	cs47l92->core.adsp[0].dev = madera->dev;
	cs47l92->core.adsp[0].regmap = madera->regmap_32bit;

	cs47l92->core.adsp[0].base = MADERA_DSP1_CONFIG_1;
	cs47l92->core.adsp[0].mem = cs47l92_dsp1_regions;
	cs47l92->core.adsp[0].num_mems = ARRAY_SIZE(cs47l92_dsp1_regions);

	cs47l92->core.adsp[0].lock_regions = WM_ADSP2_REGION_1_9;

	ret = wm_adsp2_init(&cs47l92->core.adsp[0]);
	if (ret != 0)
		goto error_dsp_irq;

	ret = madera_init_bus_error_irq(&cs47l92->core,
					0,
					cs47l92_dsp_bus_error);
	if (ret != 0) {
		wm_adsp2_remove(&cs47l92->core.adsp[0]);
		goto error_adsp;
	}

	madera_init_fll(madera, 1, MADERA_FLL1_CONTROL_1 - 1,
			&cs47l92->fll[0]);
	madera_init_fll(madera, 2, MADERA_FLL2_CONTROL_1 - 1,
			&cs47l92->fll[1]);

	for (i = 0; i < ARRAY_SIZE(cs47l92_dai); i++)
		madera_init_dai(&cs47l92->core, i);

	/* Latch volume update bits */
	for (i = 0; i < ARRAY_SIZE(cs47l92_digital_vu); i++)
		regmap_update_bits(madera->regmap, cs47l92_digital_vu[i],
				   CS47L92_DIG_VU, CS47L92_DIG_VU);

	pm_runtime_enable(&pdev->dev);
	pm_runtime_idle(&pdev->dev);

	ret = snd_soc_register_platform(&pdev->dev, &cs47l92_compr_platform);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register platform: %d\n", ret);
		goto error_pm_runtime;
	}

	ret = snd_soc_register_codec(&pdev->dev, &soc_codec_dev_cs47l92,
				     cs47l92_dai, ARRAY_SIZE(cs47l92_dai));
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register codec: %d\n", ret);
		snd_soc_unregister_platform(&pdev->dev);
		goto error_platform;
	}

	return ret;

error_platform:
	snd_soc_unregister_platform(&pdev->dev);
error_pm_runtime:
	pm_runtime_disable(&pdev->dev);
	madera_destroy_bus_error_irq(&cs47l92->core, 0);
error_adsp:
	wm_adsp2_remove(&cs47l92->core.adsp[0]);
error_dsp_irq:
	madera_set_irq_wake(madera, MADERA_IRQ_DSP_IRQ1, 0);
	madera_free_irq(madera, MADERA_IRQ_DSP_IRQ1, cs47l92);
error_core:
	madera_core_destroy(&cs47l92->core);

	return ret;
}

static int cs47l92_remove(struct platform_device *pdev)
{
	struct cs47l92 *cs47l92 = platform_get_drvdata(pdev);

	snd_soc_unregister_platform(&pdev->dev);
	snd_soc_unregister_codec(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	madera_destroy_bus_error_irq(&cs47l92->core, 0);
	wm_adsp2_remove(&cs47l92->core.adsp[0]);

	madera_set_irq_wake(cs47l92->core.madera, MADERA_IRQ_DSP_IRQ1, 0);
	madera_free_irq(cs47l92->core.madera, MADERA_IRQ_DSP_IRQ1, cs47l92);

	madera_core_destroy(&cs47l92->core);

	return 0;
}

static struct platform_driver cs47l92_codec_driver = {
	.driver = {
		.name = "cs47l92-codec",
		.suppress_bind_attrs = true,
	},
	.probe = cs47l92_probe,
	.remove = cs47l92_remove,
};

module_platform_driver(cs47l92_codec_driver);

MODULE_DESCRIPTION("ASoC CS47L92 driver");
MODULE_AUTHOR("Stuart Henderson <stuarth@opensource.wolfsonmicro.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:cs47l92-codec");
