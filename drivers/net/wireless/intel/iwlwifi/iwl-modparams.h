/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright (C) 2005-2014, 2018-2022, 2024 Intel Corporation
 */
#ifndef __iwl_modparams_h__
#define __iwl_modparams_h__

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <iwl-config.h>

extern struct iwl_mod_params iwlwifi_mod_params;

enum iwl_power_level {
	IWL_POWER_INDEX_1,
	IWL_POWER_INDEX_2,
	IWL_POWER_INDEX_3,
	IWL_POWER_INDEX_4,
	IWL_POWER_INDEX_5,
	IWL_POWER_NUM
};

enum iwl_disable_11n {
	IWL_DISABLE_HT_ALL	 = BIT(0),
	IWL_DISABLE_HT_TXAGG	 = BIT(1),
	IWL_DISABLE_HT_RXAGG	 = BIT(2),
	IWL_ENABLE_HT_TXAGG	 = BIT(3),
};

enum iwl_amsdu_size {
	IWL_AMSDU_DEF = 0,
	IWL_AMSDU_4K = 1,
	IWL_AMSDU_8K = 2,
	IWL_AMSDU_12K = 3,
	/* Add 2K at the end to avoid breaking current API */
	IWL_AMSDU_2K = 4, /* ax200 blows up if you set it to this */
};

enum iwl_uapsd_disable {
	IWL_DISABLE_UAPSD_BSS		= BIT(0),
	IWL_DISABLE_UAPSD_P2P_CLIENT	= BIT(1),
};

/**
 * struct iwl_mod_params
 *
 * Holds the module parameters
 *
 * @swcrypto: using hardware encryption, default = 0
 * @disable_11n: disable 11n capabilities, default = 0,
 *	use IWL_[DIS,EN]ABLE_HT_* constants
 * @amsdu_size: See &enum iwl_amsdu_size.
 * @fw_restart: restart firmware, default = 1
 * @bt_coex_active: enable bt coex, default = true
 * @led_mode: system default, default = 0
 * @power_save: enable power save, default = false
 * @power_level: power level, default = 1
 * @debug_level: levels are IWL_DL_*
 * @nvm_file: specifies a external NVM file
 * @uapsd_disable: disable U-APSD, see &enum iwl_uapsd_disable, default =
 *	IWL_DISABLE_UAPSD_BSS | IWL_DISABLE_UAPSD_P2P_CLIENT
 * @disable_11ac: disable VHT capabilities, default = false.
 * @remove_when_gone: remove an inaccessible device from the PCIe bus.
 * @enable_ini: enable new FW debug infratructure (INI TLVs)
 * @disable_11be: disable EHT capabilities, default = false.
 * @wbem_override: Override the WBEM value read from ACPI.
 * @dsm_override: Override any DSM value read from ACPI or BIOS.
 * @allow_puncturing:  Force allow puncturing in US and CAN regdom.
 */
struct iwl_mod_params {
	int swcrypto;
	unsigned int disable_11n;
	int amsdu_size;
	bool fw_restart;
	bool bt_coex_active;
	int led_mode;
	bool power_save;
	int power_level;
#ifdef CONFIG_IWLWIFI_DEBUG
	u32 debug_level;
#endif
	int allow_puncturing;
	char *nvm_file;
	u32 uapsd_disable;
	bool disable_11ac;
	/**
	 * @disable_11ax: disable HE capabilities, default = false
	 */
	bool disable_11ax;
	bool remove_when_gone;
	u32 enable_ini;
	bool disable_11be;
	u32 wbem_override;
	/* Magic number 13 is length of enum iwl_dsm_funcs */
	u32 dsm_override[13];
};

static inline bool iwl_enable_rx_ampdu(void)
{
	if (iwlwifi_mod_params.disable_11n & IWL_DISABLE_HT_RXAGG)
		return false;
	return true;
}

static inline bool iwl_enable_tx_ampdu(void)
{
	if (iwlwifi_mod_params.disable_11n & IWL_DISABLE_HT_TXAGG)
		return false;
	if (iwlwifi_mod_params.disable_11n & IWL_ENABLE_HT_TXAGG)
		return true;

	/* enabled by default */
	return true;
}

/* Verify amsdu_size module parameter and convert it to a rxb size */
static inline enum iwl_amsdu_size
iwl_amsdu_size_to_rxb_size(int family)
{
	switch (iwlwifi_mod_params.amsdu_size) {
	case IWL_AMSDU_8K:
		return IWL_AMSDU_8K;
	case IWL_AMSDU_12K:
		return IWL_AMSDU_12K;
	default:
		pr_err("%s: Unsupported amsdu_size: %d\n", KBUILD_MODNAME,
		       iwlwifi_mod_params.amsdu_size);
		fallthrough;
	case IWL_AMSDU_2K:
		/* ax200 blows up with this setting. */
		if (family >= IWL_DEVICE_FAMILY_AX210)
			return IWL_AMSDU_2K;
		fallthrough;
	case IWL_AMSDU_DEF:
	case IWL_AMSDU_4K:
		return IWL_AMSDU_4K;
	}
}

#endif /* #__iwl_modparams_h__ */
