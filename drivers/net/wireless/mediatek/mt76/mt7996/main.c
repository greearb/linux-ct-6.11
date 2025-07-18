// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2022 MediaTek Inc.
 */

#include "mt7996.h"
#include "mcu.h"
#include "mac.h"

u32 debug_lvl = MTK_DEBUG_FATAL | MTK_DEBUG_WRN;
module_param(debug_lvl, uint, 0644);
MODULE_PARM_DESC(debug_lvl,
		 "Enable debugging messages\n"
		 "0x00001	tx path\n"
		 "0x00002	tx path verbose\n"
		 "0x00004	fatal/very-important messages\n"
		 "0x00008	warning messages\n"
		 "0x00010	Info about messages to/from firmware\n"
		 "0x00020	Configuration logs.\n"
		 "0xffffffff	any/all\n"
	);

static void __mt7996_configure_filter(struct ieee80211_hw *hw,
				      unsigned int changed_flags,
				      unsigned int *total_flags,
				      u64 multicast);

static bool mt7996_dev_running(struct mt7996_dev *dev)
{
	struct mt7996_phy *phy;

	if (test_bit(MT76_STATE_RUNNING, &dev->mphy.state))
		return true;

	phy = mt7996_phy2(dev);
	if (phy && test_bit(MT76_STATE_RUNNING, &phy->mt76->state))
		return true;

	phy = mt7996_phy3(dev);

	return phy && test_bit(MT76_STATE_RUNNING, &phy->mt76->state);
}

static void mt7996_testmode_disable_all(struct mt7996_dev *dev)
{
	struct mt7996_phy *phy;
	int i;

	for (i = 0; i < __MT_MAX_BAND; i++) {
		phy = __mt7996_phy(dev, i);
		if (phy)
			mt76_testmode_set_state(phy->mt76, MT76_TM_STATE_OFF);
	}
}

int mt7996_run(struct ieee80211_hw *hw)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	bool running;
	int ret;

	running = mt7996_dev_running(dev);
	if (!running) {
		ret = mt7996_mcu_set_hdr_trans(dev, true);
		if (ret)
			goto out;

		if (is_mt7992(&dev->mt76)) {
			u8 queue = mt76_connac_lmac_mapping(IEEE80211_AC_VI);

			ret = mt7996_mcu_cp_support(dev, queue);
			if (ret)
				goto out;
		}
	}

	phy->sr_enable = true;
	phy->enhanced_sr_enable = true;

	/* needed to re-apply power tables after SER */
	ret = mt7996_mcu_set_txpower_sku(phy);

	mt7996_testmode_disable_all(dev);

	mt7996_mac_enable_nf(dev, phy->mt76->band_idx);

	ret = mt7996_mcu_set_rts_thresh(phy, 0x92b);
	if (ret)
		goto out;

	ret = mt7996_mcu_set_radio_en(phy, true);
	if (ret)
		goto out;

	ret = mt7996_mcu_set_chan_info(phy, UNI_CHANNEL_RX_PATH);
	if (ret)
		goto out;

	ret = mt7996_mcu_set_thermal_throttling(phy, MT7996_THERMAL_THROTTLE_MAX);
	if (ret)
		goto out;

	ret = mt7996_mcu_set_thermal_protect(phy, true);
	if (ret)
		goto out;


#ifdef CONFIG_MTK_DEBUG
	ret = mt7996_mcu_set_tx_power_ctrl(phy, UNI_TXPOWER_SKU_POWER_LIMIT_CTRL,
						dev->dbg.sku_disable ? 0 : phy->sku_limit_en);

	ret = mt7996_mcu_set_tx_power_ctrl(phy, UNI_TXPOWER_BACKOFF_POWER_LIMIT_CTRL,
						dev->dbg.sku_disable ? 0 : phy->sku_path_en);
#else
	ret = mt7996_mcu_set_tx_power_ctrl(phy, UNI_TXPOWER_SKU_POWER_LIMIT_CTRL,
						phy->sku_limit_en);
	ret = mt7996_mcu_set_tx_power_ctrl(phy, UNI_TXPOWER_BACKOFF_POWER_LIMIT_CTRL,
						phy->sku_path_en);
#endif
	if (ret)
		goto out;

	set_bit(MT76_STATE_RUNNING, &phy->mt76->state);

	ieee80211_queue_delayed_work(hw, &phy->mt76->mac_work,
				     MT7996_WATCHDOG_TIME);

	if (!running)
		mt7996_mac_reset_counters(phy);

out:
	return ret;
}

static int mt7996_start(struct ieee80211_hw *hw)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	int ret;

	dev->mt76.debug_lvl = debug_lvl;

	flush_work(&dev->init_work);

	mutex_lock(&dev->mt76.mutex);
	ret = mt7996_run(hw);
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

static void mt7996_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);

	cancel_delayed_work_sync(&phy->mt76->mac_work);

	mutex_lock(&dev->mt76.mutex);

	mt7996_mcu_set_radio_en(phy, false);

	clear_bit(MT76_STATE_RUNNING, &phy->mt76->state);

	mutex_unlock(&dev->mt76.mutex);
}

static inline int get_free_idx(u32 mask, u8 start, u8 end)
{
	return ffs(~mask & GENMASK(end, start));
}

static int get_omac_idx(enum nl80211_iftype type, u64 mask)
{
	int i;

	switch (type) {
	case NL80211_IFTYPE_MESH_POINT:
	case NL80211_IFTYPE_ADHOC:
	case NL80211_IFTYPE_STATION:
		/* prefer hw bssid slot 1-3 */
		i = get_free_idx(mask, HW_BSSID_1, HW_BSSID_3);
		if (i)
			return i - 1;

		if (type != NL80211_IFTYPE_STATION)
			break;

		i = get_free_idx(mask, EXT_BSSID_1, EXT_BSSID_MAX);
		if (i)
			return i - 1;

		if (~mask & BIT(HW_BSSID_0))
			return HW_BSSID_0;

		break;
	case NL80211_IFTYPE_MONITOR:
	case NL80211_IFTYPE_AP:
		/* ap uses hw bssid 0 and ext bssid */
		if (~mask & BIT(HW_BSSID_0))
			return HW_BSSID_0;

		i = get_free_idx(mask, EXT_BSSID_1, EXT_BSSID_MAX);
		if (i)
			return i - 1;

		break;
	default:
		WARN_ON(1);
		break;
	}

	return -1;
}

static void mt7996_init_bitrate_mask(struct ieee80211_vif *vif)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	int i;

	for (i = 0; i < ARRAY_SIZE(mvif->bitrate_mask.control); i++) {
		mvif->bitrate_mask.control[i].gi = NL80211_TXRATE_DEFAULT_GI;
		mvif->bitrate_mask.control[i].he_gi = 0xff;
		mvif->bitrate_mask.control[i].he_ltf = 0xff;
		mvif->bitrate_mask.control[i].legacy = GENMASK(31, 0);
		memset(mvif->bitrate_mask.control[i].ht_mcs, 0xff,
		       sizeof(mvif->bitrate_mask.control[i].ht_mcs));
		memset(mvif->bitrate_mask.control[i].vht_mcs, 0xff,
		       sizeof(mvif->bitrate_mask.control[i].vht_mcs));
		memset(mvif->bitrate_mask.control[i].he_mcs, 0xff,
		       sizeof(mvif->bitrate_mask.control[i].he_mcs));
	}
}

static int mt7996_add_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt76_txq *mtxq;
	u8 band_idx = phy->mt76->band_idx;
	int idx, ret = 0;

	mutex_lock(&dev->mt76.mutex);

	if (vif->type == NL80211_IFTYPE_MONITOR &&
	    is_zero_ether_addr(vif->addr))
		phy->monitor_vif = vif;

	mvif->mt76.idx = __ffs64(~dev->mt76.vif_mask);
	if (mvif->mt76.idx >= mt7996_max_interface_num(dev)) {
		ret = -ENOSPC;
		goto out;
	}

	idx = get_omac_idx(vif->type, phy->omac_mask);
	if (idx < 0) {
		ret = -ENOSPC;
		goto out;
	}
	else if (idx > HW_BSSID_3 || idx == HW_BSSID_0) {
		vif->netdev_features &= ~NETIF_F_HW_CSUM;
	}
	mvif->mt76.omac_idx = idx;
	mvif->phy = phy;
	mvif->mt76.band_idx = band_idx;
	mvif->mt76.wmm_idx = vif->type == NL80211_IFTYPE_AP ? 0 : 3;

	ret = mt7996_mcu_add_dev_info(phy, vif, true);
	if (ret)
		goto out;

	dev->mt76.vif_mask |= BIT_ULL(mvif->mt76.idx);
	phy->omac_mask |= BIT_ULL(mvif->mt76.omac_idx);

	idx = MT7996_WTBL_RESERVED - mvif->mt76.idx;

	INIT_LIST_HEAD(&mvif->sta.rc_list);
	INIT_LIST_HEAD(&mvif->sta.wcid.poll_list);
	mvif->sta.wcid.idx = idx;
	mvif->sta.wcid.phy_idx = band_idx;
	mvif->sta.wcid.hw_key_idx = -1;
	mvif->sta.wcid.tx_info |= MT_WCID_TX_INFO_SET;
	mt76_wcid_init(&mvif->sta.wcid);

	mt7996_mac_wtbl_update(dev, idx,
			       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);

	if (vif->txq) {
		mtxq = (struct mt76_txq *)vif->txq->drv_priv;
		mtxq->wcid = idx;
	}

	if (vif->type != NL80211_IFTYPE_AP &&
	    (!mvif->mt76.omac_idx || mvif->mt76.omac_idx > 3))
		vif->offload_flags = 0;
	vif->offload_flags |= IEEE80211_OFFLOAD_ENCAP_4ADDR;

	if (phy->mt76->chandef.chan->band != NL80211_BAND_2GHZ)
		mvif->mt76.basic_rates_idx = MT7996_BASIC_RATES_TBL + 4;
	else
		mvif->mt76.basic_rates_idx = MT7996_BASIC_RATES_TBL;

	mt7996_init_bitrate_mask(vif);

	mt7996_mcu_add_bss_info(phy, vif, true);
	/* defer the first STA_REC of BMC entry to BSS_CHANGED_BSSID for STA
	 * interface, since firmware only records BSSID when the entry is new
	 */
	if (vif->type != NL80211_IFTYPE_STATION)
		mt7996_mcu_add_sta(dev, vif, NULL, true, true);
	rcu_assign_pointer(dev->mt76.wcid[idx], &mvif->sta.wcid);

out:
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

static void mt7996_remove_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_sta *msta = &mvif->sta;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	int idx = msta->wcid.idx;

	mt7996_mcu_add_sta(dev, vif, NULL, false, false);
	mt7996_mcu_add_bss_info(phy, vif, false);

	if (vif == phy->monitor_vif)
		phy->monitor_vif = NULL;

	mt7996_mcu_add_dev_info(phy, vif, false);

	rcu_assign_pointer(dev->mt76.wcid[idx], NULL);

	mutex_lock(&dev->mt76.mutex);
	dev->mt76.vif_mask &= ~BIT_ULL(mvif->mt76.idx);
	phy->omac_mask &= ~BIT_ULL(mvif->mt76.omac_idx);
	mutex_unlock(&dev->mt76.mutex);

	spin_lock_bh(&dev->mt76.sta_poll_lock);
	if (!list_empty(&msta->wcid.poll_list))
		list_del_init(&msta->wcid.poll_list);
	spin_unlock_bh(&dev->mt76.sta_poll_lock);

	mt76_wcid_cleanup(&dev->mt76, &msta->wcid);
}

int mt7996_set_channel(struct mt7996_phy *phy)
{
	struct mt7996_dev *dev = phy->dev;
	int ret;

	cancel_delayed_work_sync(&phy->mt76->mac_work);

	mutex_lock(&dev->mt76.mutex);
	set_bit(MT76_RESET, &phy->mt76->state);

	mt76_set_channel(phy->mt76);

#if 0
	if (mt76_testmode_enabled(phy->mt76) || phy->mt76->test.bf_en) {
		mt7996_tm_update_channel(phy);
		goto out;
	}
#endif

	ret = mt7996_mcu_set_chan_info(phy, UNI_CHANNEL_SWITCH);
	if (ret)
		goto out;

	ret = mt7996_mcu_set_chan_info(phy, UNI_CHANNEL_RX_PATH);
	if (ret)
		goto out;

	ret = mt7996_dfs_init_radar_detector(phy);
	mt7996_mac_cca_stats_reset(phy);

	mt7996_mac_reset_counters(phy);
	phy->noise = 0;

out:
	clear_bit(MT76_RESET, &phy->mt76->state);
	mutex_unlock(&dev->mt76.mutex);

	mt76_txq_schedule_all(phy->mt76);

	ieee80211_queue_delayed_work(phy->mt76->hw,
				     &phy->mt76->mac_work,
				     MT7996_WATCHDOG_TIME);

	return ret;
}

static int mt7996_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			  struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			  struct ieee80211_key_conf *key)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_sta *msta = sta ? (struct mt7996_sta *)sta->drv_priv :
				  &mvif->sta;
	struct mt76_wcid *wcid = &msta->wcid;
	u8 *wcid_keyidx = &wcid->hw_key_idx;
	int idx = key->keyidx;
	int err = 0;

	/* The hardware does not support per-STA RX GTK, fallback
	 * to software mode for these.
	 */
	if ((vif->type == NL80211_IFTYPE_ADHOC ||
	     vif->type == NL80211_IFTYPE_MESH_POINT) &&
	    (key->cipher == WLAN_CIPHER_SUITE_TKIP ||
	     key->cipher == WLAN_CIPHER_SUITE_CCMP) &&
	    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		return -EOPNOTSUPP;

	/* fall back to sw encryption for unsupported ciphers */
	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_TKIP:
	case WLAN_CIPHER_SUITE_CCMP:
	case WLAN_CIPHER_SUITE_CCMP_256:
	case WLAN_CIPHER_SUITE_GCMP:
	case WLAN_CIPHER_SUITE_GCMP_256:
	case WLAN_CIPHER_SUITE_SMS4:
		break;
	case WLAN_CIPHER_SUITE_AES_CMAC:
	case WLAN_CIPHER_SUITE_BIP_CMAC_256:
	case WLAN_CIPHER_SUITE_BIP_GMAC_128:
	case WLAN_CIPHER_SUITE_BIP_GMAC_256:
		if (key->keyidx == 6 || key->keyidx == 7) {
			wcid_keyidx = &wcid->hw_key_idx2;
			key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIE;
			break;
		}
		fallthrough;
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&dev->mt76.mutex);

	if (cmd == SET_KEY && !sta && !mvif->mt76.cipher) {
		mvif->mt76.cipher = mt76_connac_mcu_get_cipher(key->cipher);
		mt7996_mcu_add_bss_info(phy, vif, true);
	}

	if (cmd == SET_KEY) {
		*wcid_keyidx = idx;
	} else {
		if (idx == *wcid_keyidx)
			*wcid_keyidx = -1;
		goto out;
	}

	mt76_wcid_key_setup(&dev->mt76, wcid, key);

	if (key->keyidx == 6 || key->keyidx == 7)
		err = mt7996_mcu_bcn_prot_enable(dev, vif, key);
	else
		err = mt7996_mcu_add_key(&dev->mt76, vif, key,
					 MCU_WMWA_UNI_CMD(STA_REC_UPDATE),
					 &msta->wcid, cmd);
out:
	mutex_unlock(&dev->mt76.mutex);

	return err;
}

static int mt7996_config(struct ieee80211_hw *hw, u32 changed)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	int ret;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		ret = mt7996_mcu_set_pp_en(phy, PP_USR_MODE,
					   phy->mt76->chandef.punctured);
		if (ret)
			return ret;

#if 0
		if (!mt76_testmode_enabled(phy->mt76) && !phy->mt76->test.bf_en) {
			ret = mt7996_mcu_edcca_enable(phy, true);
			if (ret)
				return ret;
		}
#endif
		ieee80211_stop_queues(hw);
		ret = mt7996_set_channel(phy);
		if (ret)
			return ret;
		ieee80211_wake_queues(hw);
	}

	if (changed & (IEEE80211_CONF_CHANGE_POWER |
		       IEEE80211_CONF_CHANGE_CHANNEL)) {
		ret = mt7996_mcu_set_txpower_sku(phy);
		if (ret) {
			dev_err(dev->mt76.dev,
				"config:  Failed to set txpower: %d\n", ret);
			return ret;
		}
	}

	mutex_lock(&dev->mt76.mutex);

	if (changed & IEEE80211_CONF_CHANGE_MONITOR) {
		bool enabled = !!(hw->conf.flags & IEEE80211_CONF_MONITOR);
		u32 total_flags = phy->mac80211_rxfilter_flags;
		u64 multicast = 0; /* not used by this driver currently. */

		phy->monitor_enabled = enabled;

		mt76_rmw_field(dev, MT_DMA_DCR0(phy->mt76->band_idx),
			       MT_DMA_DCR0_RXD_G5_EN, enabled || dev->rx_group_5_enable);

		__mt7996_configure_filter(hw, 0, &total_flags, multicast);
		mt7996_mcu_set_sniffer_mode(phy, enabled);
	}

	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static int
mt7996_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       unsigned int link_id, u16 queue,
	       const struct ieee80211_tx_queue_params *params)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	static const u8 mq_to_aci[] = {
		[IEEE80211_AC_VO] = 3,
		[IEEE80211_AC_VI] = 2,
		[IEEE80211_AC_BE] = 0,
		[IEEE80211_AC_BK] = 1,
	};

	/* firmware uses access class index */
	mvif->queue_params[mq_to_aci[queue]] = *params;
	/* no need to update right away, we'll get BSS_CHANGED_QOS */

	return 0;
}

static void __mt7996_configure_filter(struct ieee80211_hw *hw,
				      unsigned int changed_flags,
				      unsigned int *total_flags,
				      u64 multicast)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	u32 ctl_flags = MT_WF_RFCR1_DROP_ACK |
			MT_WF_RFCR1_DROP_BF_POLL |
			MT_WF_RFCR1_DROP_BA |
			MT_WF_RFCR1_DROP_CFEND |
			MT_WF_RFCR1_DROP_NO2ME_TF |
			MT_WF_RFCR1_DROP_NON_MUBAR_TF |
			MT_WF_RFCR1_DROP_RXS_BRP |
			MT_WF_RFCR1_DROP_TF_BFRP |
			MT_WF_RFCR1_DROP_CFACK;
	u32 flags = 0;
	bool is_promisc = *total_flags & FIF_CONTROL || phy->monitor_vif ||
		phy->monitor_enabled;

#define MT76_FILTER(_flag, _hw) do {					\
		flags |= *total_flags & FIF_##_flag;			\
		phy->rxfilter &= ~(_hw);				\
		phy->rxfilter |= !(flags & FIF_##_flag) * (_hw);	\
	} while (0)

	phy->mac80211_rxfilter_flags = *total_flags; /* save requested flags for later */

	phy->rxfilter &= ~(MT_WF_RFCR_DROP_OTHER_BSS |
			   MT_WF_RFCR_DROP_OTHER_BEACON |
			   MT_WF_RFCR_DROP_FRAME_REPORT |
			   MT_WF_RFCR_DROP_PROBEREQ |
			   MT_WF_RFCR_DROP_MCAST_FILTERED |
			   MT_WF_RFCR_DROP_MCAST |
			   MT_WF_RFCR_DROP_BCAST |
			   MT_WF_RFCR_DROP_DUPLICATE |
			   MT_WF_RFCR_DROP_A2_BSSID |
			   MT_WF_RFCR_RX_UNWANTED_CTL |
			   MT_WF_RFCR_SECOND_BCN_EN |
			   MT_WF_RFCR_RX_MGMT_FRAME_CTRL |
			   MT_WF_RFCR_RX_SAMEBSSIDPRORESP_CTRL |
			   MT_WF_RFCR_RX_DIFFBSSIDPRORESP_CTRL |
			   MT_WF_RFCR_RX_SAMEBSSIDBCN_CTRL |
			   MT_WF_RFCR_RX_SAMEBSSIDNULL_CTRL |
			   MT_WF_RFCR_RX_DIFFBSSIDNULL_CTRL |
			   //MT_WF_RFCR_IND_FILTER_EN_OF_31_23_BIT |
			   MT_WF_RFCR_DROP_STBC_MULTI);

	MT76_FILTER(OTHER_BSS, MT_WF_RFCR_DROP_OTHER_TIM |
			       MT_WF_RFCR_DROP_A3_BSSID);

	MT76_FILTER(FCSFAIL, MT_WF_RFCR_DROP_FCSFAIL);

	MT76_FILTER(CONTROL, MT_WF_RFCR_DROP_CTS |
			     MT_WF_RFCR_DROP_RTS |
			     MT_WF_RFCR_DROP_CTL_RSV);

	phy->rxfilter |= (MT_WF_RFCR_DROP_OTHER_UC |
			  MT_WF_RFCR_DROP_DIFFBSSIDMGT_CTRL);

	if (is_promisc) {
		phy->rxfilter &= ~MT_WF_RFCR_DROP_OTHER_UC;
		//phy->rxfilter |= MT_WF_RFCR_IND_FILTER_EN_OF_31_23_BIT;
		phy->rxfilter |= MT_WF_RFCR_RX_DATA_FRAME_CTRL;
		if (flags & FIF_CONTROL) {
			phy->rxfilter |= MT_WF_RFCR_RX_UNWANTED_CTL;
			phy->rxfilter |= MT_WF_RFCR_SECOND_BCN_EN;
			phy->rxfilter |= MT_WF_RFCR_RX_MGMT_FRAME_CTRL;
			phy->rxfilter |= MT_WF_RFCR_RX_SAMEBSSIDPRORESP_CTRL;
			phy->rxfilter |= MT_WF_RFCR_RX_DIFFBSSIDPRORESP_CTRL;
			phy->rxfilter |= MT_WF_RFCR_RX_SAMEBSSIDBCN_CTRL;
			phy->rxfilter |= MT_WF_RFCR_RX_SAMEBSSIDNULL_CTRL;
			phy->rxfilter |= MT_WF_RFCR_RX_DIFFBSSIDNULL_CTRL;
			phy->rxfilter &= ~(MT_WF_RFCR_DROP_DIFFBSSIDMGT_CTRL);
		}
	}

	*total_flags = flags;

	//dev_err(dev->mt76.dev, "apply-rxfilter, promisc: %d  rxfilter: 0x%x FIF_CONTROL: 0x%x monitor-vif: %p mon-enabled: %d\n",
	//	is_promisc, phy->rxfilter, flags & FIF_CONTROL, phy->monitor_vif,
	//	phy->monitor_enabled);
	mt76_wr(dev, MT_WF_RFCR(phy->mt76->band_idx), phy->rxfilter);

	if (is_promisc)
		mt76_clear(dev, MT_WF_RFCR1(phy->mt76->band_idx), ctl_flags);
	else
		mt76_set(dev, MT_WF_RFCR1(phy->mt76->band_idx), ctl_flags);
}

static void mt7996_configure_filter(struct ieee80211_hw *hw,
				    unsigned int changed_flags,
				    unsigned int *total_flags,
				    u64 multicast)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);

	mutex_lock(&dev->mt76.mutex);

	__mt7996_configure_filter(hw, changed_flags, total_flags, multicast);

	mutex_unlock(&dev->mt76.mutex);
}

static void
mt7996_update_bss_color(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif,
			struct cfg80211_he_bss_color *bss_color)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);

	switch (vif->type) {
	case NL80211_IFTYPE_AP: {
		struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;

		if (mvif->mt76.omac_idx > HW_BSSID_MAX)
			return;
		fallthrough;
	}
	case NL80211_IFTYPE_STATION:
		mt7996_mcu_update_bss_color(dev, vif, bss_color);
		break;
	default:
		break;
	}
}

static u8
mt7996_get_rates_table(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       bool beacon, bool mcast)
{
#define FR_RATE_IDX_OFDM_6M 0x004b
	struct mt76_vif *mvif = (struct mt76_vif *)vif->drv_priv;
	struct mt76_phy *mphy = hw->priv;
	u16 rate;
	u8 i, idx;

	rate = mt76_connac2_mac_tx_rate_val(mphy, vif, beacon, mcast);

	if (beacon) {
		struct mt7996_phy *phy = mphy->priv;

		if (mphy->dev->lpi_bcn_enhance)
			rate = FR_RATE_IDX_OFDM_6M;

		/* odd index for driver, even index for firmware */
		idx = MT7996_BEACON_RATES_TBL + 2 * phy->mt76->band_idx;
		if (phy->beacon_rate != rate)
			mt7996_mcu_set_fixed_rate_table(phy, idx, rate, beacon);

		return idx;
	}

	idx = FIELD_GET(MT_TX_RATE_IDX, rate);
	for (i = 0; i < ARRAY_SIZE(mt76_rates); i++)
		if ((mt76_rates[i].hw_value & GENMASK(7, 0)) == idx)
			return MT7996_BASIC_RATES_TBL + 2 * i;

	return mvif->basic_rates_idx;
}

static void
mt7996_update_mu_group(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       struct ieee80211_bss_conf *info)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	u8 band = mvif->mt76.band_idx;
	u32 *mu;

	mu = (u32 *)info->mu_group.membership;
	mt76_wr(dev, MT_WF_PHYRX_BAND_GID_TAB_VLD0(band), mu[0]);
	mt76_wr(dev, MT_WF_PHYRX_BAND_GID_TAB_VLD1(band), mu[1]);

	mu = (u32 *)info->mu_group.position;
	mt76_wr(dev, MT_WF_PHYRX_BAND_GID_TAB_POS0(band), mu[0]);
	mt76_wr(dev, MT_WF_PHYRX_BAND_GID_TAB_POS1(band), mu[1]);
	mt76_wr(dev, MT_WF_PHYRX_BAND_GID_TAB_POS2(band), mu[2]);
	mt76_wr(dev, MT_WF_PHYRX_BAND_GID_TAB_POS3(band), mu[3]);
}

static void mt7996_bss_info_changed(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_bss_conf *info,
				    u64 changed)
{
	struct mt76_vif *mvif = (struct mt76_vif *)vif->drv_priv;
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_dev *dev = mt7996_hw_dev(hw);

	mutex_lock(&dev->mt76.mutex);

	/* station mode uses BSSID to map the wlan entry to a peer,
	 * and then peer references bss_info_rfch to set bandwidth cap.
	 */
	if ((changed & BSS_CHANGED_BSSID && !is_zero_ether_addr(info->bssid)) ||
	    (changed & BSS_CHANGED_ASSOC && vif->cfg.assoc) ||
	    (changed & BSS_CHANGED_BEACON_ENABLED && info->enable_beacon)) {
		mt7996_mcu_add_bss_info(phy, vif, true);
		mt7996_mcu_add_sta(dev, vif, NULL, true,
				   !!(changed & BSS_CHANGED_BSSID));
	}

	if (changed & BSS_CHANGED_ERP_CTS_PROT)
		mt7996_mac_enable_rtscts(dev, vif, info->use_cts_prot);

	if (changed & BSS_CHANGED_ERP_SLOT) {
		int slottime = info->use_short_slot ? 9 : 20;

		if (slottime != phy->slottime) {
			phy->slottime = slottime;
			mt7996_mcu_set_timing(phy, vif);
		}
	}

	if (changed & BSS_CHANGED_MCAST_RATE)
		mvif->mcast_rates_idx =
			mt7996_get_rates_table(hw, vif, false, true);

	if (changed & BSS_CHANGED_BASIC_RATES)
		mvif->basic_rates_idx =
			mt7996_get_rates_table(hw, vif, false, false);

	/* ensure that enable txcmd_mode after bss_info */
	if (changed & (BSS_CHANGED_QOS | BSS_CHANGED_BEACON_ENABLED))
		mt7996_mcu_set_tx(dev, vif);

	if (changed & BSS_CHANGED_HE_OBSS_PD)
		mt7996_mcu_add_obss_spr(phy, vif, &info->he_obss_pd);

	if (changed & BSS_CHANGED_HE_BSS_COLOR)
		mt7996_update_bss_color(hw, vif, &info->he_bss_color);

	if (changed & (BSS_CHANGED_BEACON |
		       BSS_CHANGED_BEACON_ENABLED)) {
		mvif->beacon_rates_idx =
			mt7996_get_rates_table(hw, vif, true, false);

		mt7996_mcu_add_beacon(hw, vif, info->enable_beacon);
	}

	if (changed & (BSS_CHANGED_UNSOL_BCAST_PROBE_RESP |
		       BSS_CHANGED_FILS_DISCOVERY))
		mt7996_mcu_beacon_inband_discov(dev, vif, changed);

	if (changed & BSS_CHANGED_MU_GROUPS)
		mt7996_update_mu_group(hw, vif, info);

	if (changed & BSS_CHANGED_TXPOWER)
		mt7996_mcu_set_txpower_sku(phy);

	mutex_unlock(&dev->mt76.mutex);
}

static void
mt7996_channel_switch_beacon(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif,
			     struct cfg80211_chan_def *chandef)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);

	mutex_lock(&dev->mt76.mutex);
	mt7996_mcu_add_beacon(hw, vif, true);
	mutex_unlock(&dev->mt76.mutex);
}

int mt7996_mac_sta_add(struct mt76_dev *mdev, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta)
{
	struct mt7996_dev *dev = container_of(mdev, struct mt7996_dev, mt76);
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	u8 band_idx = mvif->phy->mt76->band_idx;
	int ret, idx;

	idx = mt76_wcid_alloc(dev->mt76.wcid_mask, MT7996_WTBL_STA);
	if (idx < 0)
		return -ENOSPC;

	INIT_LIST_HEAD(&msta->rc_list);
	INIT_LIST_HEAD(&msta->wcid.poll_list);
	msta->vif = mvif;
	msta->wcid.sta = 1;
	msta->wcid.idx = idx;
	msta->wcid.phy_idx = band_idx;
	msta->wcid.tx_info |= MT_WCID_TX_INFO_SET;

	ewma_avg_signal_init(&msta->avg_ack_signal);

	mt7996_mac_wtbl_update(dev, idx,
			       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);

	ret = mt7996_mcu_add_sta(dev, vif, sta, true, true);
	if (ret)
		return ret;

	return mt7996_mcu_add_rate_ctrl(dev, vif, sta, false);
}

void mt7996_mac_sta_remove(struct mt76_dev *mdev, struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta)
{
	struct mt7996_dev *dev = container_of(mdev, struct mt7996_dev, mt76);
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct mt76_wcid *wcid = &msta->wcid;
	int i;

	mt7996_mcu_add_sta(dev, vif, sta, false, false);

	mt7996_mac_wtbl_update(dev, msta->wcid.idx,
			       MT_WTBL_UPDATE_ADM_COUNT_CLEAR);

	for (i = 0; i < ARRAY_SIZE(msta->twt.flow); i++)
		mt7996_mac_twt_teardown_flow(dev, msta, i);

	spin_lock_bh(&mdev->sta_poll_lock);
	if (!list_empty(&msta->wcid.poll_list))
		list_del_init(&msta->wcid.poll_list);
	if (!list_empty(&msta->rc_list))
		list_del_init(&msta->rc_list);
	spin_unlock_bh(&mdev->sta_poll_lock);

	if (WARN_ON_ONCE(!idr_is_empty(&wcid->pktid))) {
		dev_err(dev->mt76.dev,
			"wcid->pktid is not empty in mac_sta_remove, will force status check to recover.\n");
		mt76_tx_status_check(mdev, true);
	}

	spin_lock_bh(&mdev->status_lock);
	if (WARN_ON(!list_empty(&wcid->list)))
		list_del_init(&wcid->list);
	spin_unlock_bh(&mdev->status_lock);
}

static void mt7996_tx(struct ieee80211_hw *hw,
		      struct ieee80211_tx_control *control,
		      struct sk_buff *skb)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt76_phy *mphy = hw->priv;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_vif *vif = info->control.vif;
	struct mt76_wcid *wcid = &dev->mt76.global_wcid;

	if (control->sta) {
		struct mt7996_sta *sta;

		sta = (struct mt7996_sta *)control->sta->drv_priv;
		wcid = &sta->wcid;
	}

	if (vif && !control->sta) {
		struct mt7996_vif *mvif;

		mvif = (struct mt7996_vif *)vif->drv_priv;
		wcid = &mvif->sta.wcid;
	}

	mtk_dbg(&dev->mt76, TXV, "mt7996-tx, wcid: %p wcid->idx: %d skb: %p, call mt76_tx\n",
		wcid, wcid->idx, skb);

	mt76_tx(mphy, control->sta, wcid, skb);
}

static int mt7996_set_rts_threshold(struct ieee80211_hw *hw, u32 val)
{
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	int ret;

	mutex_lock(&phy->dev->mt76.mutex);
	ret = mt7996_mcu_set_rts_thresh(phy, val);
	mutex_unlock(&phy->dev->mt76.mutex);

	return ret;
}

static int
mt7996_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    struct ieee80211_ampdu_params *params)
{
	enum ieee80211_ampdu_mlme_action action = params->action;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct ieee80211_sta *sta = params->sta;
	struct ieee80211_txq *txq = sta->txq[params->tid];
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	u16 tid = params->tid;
	u16 ssn = params->ssn;
	struct mt76_txq *mtxq;
	int ret = 0;

	if (!txq)
		return -EINVAL;

	mtxq = (struct mt76_txq *)txq->drv_priv;

	mutex_lock(&dev->mt76.mutex);
	switch (action) {
	case IEEE80211_AMPDU_RX_START:
		mt76_rx_aggr_start(&dev->mt76, &msta->wcid, tid, ssn,
				   params->buf_size);
		ret = mt7996_mcu_add_rx_ba(dev, params, true);
		mtk_dbg(&dev->mt76, BA, "ampdu-action, RX_START, tid: %d ssn: %d ret: %d\n",
			tid, ssn, ret);
		break;
	case IEEE80211_AMPDU_RX_STOP:
		mt76_rx_aggr_stop(&dev->mt76, &msta->wcid, tid);
		ret = mt7996_mcu_add_rx_ba(dev, params, false);
		mtk_dbg(&dev->mt76, BA, "ampdu-action, RX_STOP, tid: %d ssn: %d ret: %d\n",
			tid, ssn, ret);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		mtxq->aggr = true;
		mtxq->send_bar = false;
		ret = mt7996_mcu_add_tx_ba(dev, params, true);
		mtk_dbg(&dev->mt76, BA, "ampdu-action, TX_OPERATIONAL, tid: %d ssn: %d ret: %d\n",
			tid, ssn, ret);
		break;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		mtxq->aggr = false;
		clear_bit(tid, &msta->wcid.ampdu_state);
		ret = mt7996_mcu_add_tx_ba(dev, params, false);
		mtk_dbg(&dev->mt76, BA, "ampdu-action, TX_AMPDU_STOP_FLUSH(%d), tid: %d ssn: %d ret: %d\n",
			action, tid, ssn, ret);
		break;
	case IEEE80211_AMPDU_TX_START:
		set_bit(tid, &msta->wcid.ampdu_state);
		ret = IEEE80211_AMPDU_TX_START_IMMEDIATE;
		mtk_dbg(&dev->mt76, BA, "ampdu-action, TX_START, tid: %d ssn: %d ret: %d\n",
			tid, ssn, ret);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		mtxq->aggr = false;
		clear_bit(tid, &msta->wcid.ampdu_state);
		ret = mt7996_mcu_add_tx_ba(dev, params, false);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		mtk_dbg(&dev->mt76, BA, "ampdu-action, AMPDU_TX_STOP_CONT, tid: %d ssn: %d ret: %d\n",
			tid, ssn, ret);
		break;
	}
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

static int
mt7996_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       struct ieee80211_sta *sta)
{
	return mt76_sta_state(hw, vif, sta, IEEE80211_STA_NOTEXIST,
			      IEEE80211_STA_NONE);
}

static int
mt7996_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		  struct ieee80211_sta *sta)
{
	return mt76_sta_state(hw, vif, sta, IEEE80211_STA_NONE,
			      IEEE80211_STA_NOTEXIST);
}

static int
mt7996_get_stats(struct ieee80211_hw *hw,
		 struct ieee80211_low_level_stats *stats)
{
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt76_mib_stats *mib = &phy->mib;

	mutex_lock(&dev->mt76.mutex);

	stats->dot11RTSSuccessCount = mib->rts_cnt;
	stats->dot11RTSFailureCount = mib->rts_retries_cnt;
	stats->dot11FCSErrorCount = mib->fcs_err_cnt;
	stats->dot11ACKFailureCount = mib->ack_fail_cnt;

	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

u64 __mt7996_get_tsf(struct ieee80211_hw *hw, struct mt7996_vif *mvif)
{
#define FR_RATE_IDX_OFDM_6M 0x004b
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	union {
		u64 t64;
		u32 t32[2];
	} tsf;
	u16 n;

	lockdep_assert_held(&dev->mt76.mutex);

	n = mvif->mt76.omac_idx > HW_BSSID_MAX ? HW_BSSID_0
					       : mvif->mt76.omac_idx;
	/* TSF software read */
	mt76_rmw(dev, MT_LPON_TCR(phy->mt76->band_idx, n), MT_LPON_TCR_SW_MODE,
		 MT_LPON_TCR_SW_READ);
	tsf.t32[0] = mt76_rr(dev, MT_LPON_UTTR0(phy->mt76->band_idx));
	tsf.t32[1] = mt76_rr(dev, MT_LPON_UTTR1(phy->mt76->band_idx));

	return tsf.t64;
}

static u64
mt7996_get_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	u64 ret;

	mutex_lock(&dev->mt76.mutex);
	ret = __mt7996_get_tsf(hw, mvif);
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

static void
mt7996_set_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	       u64 timestamp)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	union {
		u64 t64;
		u32 t32[2];
	} tsf = { .t64 = timestamp, };
	u16 n;

	mutex_lock(&dev->mt76.mutex);

	n = mvif->mt76.omac_idx > HW_BSSID_MAX ? HW_BSSID_0
					       : mvif->mt76.omac_idx;
	mt76_wr(dev, MT_LPON_UTTR0(phy->mt76->band_idx), tsf.t32[0]);
	mt76_wr(dev, MT_LPON_UTTR1(phy->mt76->band_idx), tsf.t32[1]);
	/* TSF software overwrite */
	mt76_rmw(dev, MT_LPON_TCR(phy->mt76->band_idx, n), MT_LPON_TCR_SW_MODE,
		 MT_LPON_TCR_SW_WRITE);

	mutex_unlock(&dev->mt76.mutex);
}

static void
mt7996_offset_tsf(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		  s64 timestamp)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	union {
		u64 t64;
		u32 t32[2];
	} tsf = { .t64 = timestamp, };
	u16 n;

	mutex_lock(&dev->mt76.mutex);

	n = mvif->mt76.omac_idx > HW_BSSID_MAX ? HW_BSSID_0
					       : mvif->mt76.omac_idx;
	mt76_wr(dev, MT_LPON_UTTR0(phy->mt76->band_idx), tsf.t32[0]);
	mt76_wr(dev, MT_LPON_UTTR1(phy->mt76->band_idx), tsf.t32[1]);
	/* TSF software adjust*/
	mt76_rmw(dev, MT_LPON_TCR(phy->mt76->band_idx, n), MT_LPON_TCR_SW_MODE,
		 MT_LPON_TCR_SW_ADJUST);

	mutex_unlock(&dev->mt76.mutex);
}

static void
mt7996_set_coverage_class(struct ieee80211_hw *hw, s16 coverage_class)
{
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_dev *dev = phy->dev;

	mutex_lock(&dev->mt76.mutex);
	phy->coverage_class = max_t(s16, coverage_class, 0);
	mt7996_mac_set_coverage_class(phy);
	mutex_unlock(&dev->mt76.mutex);
}

static int
mt7996_set_antenna(struct ieee80211_hw *hw, u32 tx_ant, u32 rx_ant)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	int max_nss = hweight8(hw->wiphy->available_antennas_tx);
	u8 band_idx = phy->mt76->band_idx, shift = dev->chainshift[band_idx];

	if (!tx_ant || tx_ant != rx_ant || ffs(tx_ant) > max_nss)
		return -EINVAL;

	if ((BIT(hweight8(tx_ant)) - 1) != tx_ant)
		tx_ant = BIT(ffs(tx_ant) - 1) - 1;

	mutex_lock(&dev->mt76.mutex);

	phy->mt76->antenna_mask = tx_ant;

	/* restore to the origin chainmask which might have auxiliary path */
	if (hweight8(tx_ant) == max_nss && band_idx < MT_BAND2)
		phy->mt76->chainmask = ((dev->chainmask >> shift) &
					(BIT(dev->chainshift[band_idx + 1] - shift) - 1)) << shift;
	else if (hweight8(tx_ant) == max_nss)
		phy->mt76->chainmask = (dev->chainmask >> shift) << shift;
	else
		phy->mt76->chainmask = tx_ant << shift;

	mt76_set_stream_caps(phy->mt76, true);
	mt7996_set_stream_vht_txbf_caps(phy);
	mt7996_set_stream_he_eht_caps(phy);
	mt7996_mcu_set_txpower_sku(phy);

	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static void mt7996_sta_statistics(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct ieee80211_sta *sta,
				  struct station_info *sinfo)
{
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct rate_info *txrate = &msta->wcid.rate;

	if (txrate->legacy || txrate->flags) {
		if (txrate->legacy) {
			sinfo->txrate.legacy = txrate->legacy;
		} else {
			sinfo->txrate.mcs = txrate->mcs;
			sinfo->txrate.nss = txrate->nss;
			sinfo->txrate.bw = txrate->bw;
			sinfo->txrate.he_gi = txrate->he_gi;
			sinfo->txrate.he_dcm = txrate->he_dcm;
			sinfo->txrate.he_ru_alloc = txrate->he_ru_alloc;
			sinfo->txrate.eht_gi = txrate->eht_gi;
		}
		sinfo->txrate.flags = txrate->flags;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
	}
	sinfo->txrate.flags = txrate->flags;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);

	sinfo->tx_failed = msta->wcid.stats.tx_failed;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_FAILED);

	sinfo->tx_retries = msta->wcid.stats.tx_retries;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_RETRIES);

	sinfo->ack_signal = (s8)msta->ack_signal;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_ACK_SIGNAL);

	sinfo->avg_ack_signal = -(s8)ewma_avg_signal_read(&msta->avg_ack_signal);
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_ACK_SIGNAL_AVG);

	if (mtk_wed_device_active(&phy->dev->mt76.mmio.wed)) {
		sinfo->tx_bytes = msta->wcid.stats.tx_bytes;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BYTES64);

		sinfo->rx_bytes = msta->wcid.stats.rx_bytes;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_BYTES64);

		sinfo->tx_packets = msta->wcid.stats.tx_mpdu_ok;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_PACKETS);

		sinfo->rx_packets = msta->wcid.stats.rx_packets;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_PACKETS);
	}
}

static void mt7996_sta_rc_work(void *data, struct ieee80211_sta *sta)
{
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct mt7996_dev *dev = msta->vif->phy->dev;
	u32 *changed = data;

	spin_lock_bh(&dev->mt76.sta_poll_lock);
	msta->changed |= *changed;
	if (list_empty(&msta->rc_list))
		list_add_tail(&msta->rc_list, &dev->sta_rc_list);
	spin_unlock_bh(&dev->mt76.sta_poll_lock);
}

static void mt7996_sta_rc_update(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif,
				 struct ieee80211_link_sta *link_sta,
				 u32 changed)
{
	struct ieee80211_sta *sta = link_sta->sta;
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_dev *dev = phy->dev;

	if (!msta->vif) {
		dev_warn(dev->mt76.dev, "Un-initialized STA %pM wcid %d in rc_work\n",
			 sta->addr, msta->wcid.idx);
		return;
	}

	mt7996_sta_rc_work(&changed, sta);
	ieee80211_queue_work(hw, &dev->rc_work);
}

static int
mt7996_set_bitrate_mask(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			const struct cfg80211_bitrate_mask *mask)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_dev *dev = phy->dev;
	u32 changed = IEEE80211_RC_SUPP_RATES_CHANGED;

	mvif->bitrate_mask = *mask;

	/* if multiple rates across different preambles are given we can
	 * reconfigure this info with all peers using sta_rec command with
	 * the below exception cases.
	 * - single rate : if a rate is passed along with different preambles,
	 * we select the highest one as fixed rate. i.e VHT MCS for VHT peers.
	 * - multiple rates: if it's not in range format i.e 0-{7,8,9} for VHT
	 * then multiple MCS setting (MCS 4,5,6) is not supported.
	 */
	ieee80211_iterate_stations_atomic(hw, mt7996_sta_rc_work, &changed);
	ieee80211_queue_work(hw, &dev->rc_work);

	return 0;
}

static void mt7996_sta_set_4addr(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif,
				 struct ieee80211_sta *sta,
				 bool enabled)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;

	if (enabled)
		set_bit(MT_WCID_FLAG_4ADDR, &msta->wcid.flags);
	else
		clear_bit(MT_WCID_FLAG_4ADDR, &msta->wcid.flags);

	mt7996_mcu_wtbl_update_hdr_trans(dev, vif, sta);
}

static void mt7996_sta_set_decap_offload(struct ieee80211_hw *hw,
					 struct ieee80211_vif *vif,
					 struct ieee80211_sta *sta,
					 bool enabled)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;

	if (enabled)
		set_bit(MT_WCID_FLAG_HDR_TRANS, &msta->wcid.flags);
	else
		clear_bit(MT_WCID_FLAG_HDR_TRANS, &msta->wcid.flags);

	mt7996_mcu_wtbl_update_hdr_trans(dev, vif, sta);
}

static const char mt7996_gstrings_stats[][ETH_GSTRING_LEN] = {
	"tx_pkts_nic", /* from driver, phy tx-ok skb */
	"tx_bytes_nic", /* from driver, phy tx-ok bytes */
	"rx_pkts_nic", /* from driver, phy rx OK skb */
	"rx_bytes_nic", /* from driver, phy rx OK bytes */
	"tx_ampdu_cnt",
	"tx_stop_q_empty_cnt",
	"tx_mpdu_attempts",
	"tx_mpdu_success",
	"tx_rwp_fail_cnt",
	"tx_rwp_need_cnt",
	"tx_pkt_ebf_cnt",
	"tx_pkt_ibf_cnt",
	"tx_ampdu_len:0-1",
	"tx_ampdu_len:2-10",
	"tx_ampdu_len:11-19",
	"tx_ampdu_len:20-28",
	"tx_ampdu_len:29-37",
	"tx_ampdu_len:38-46",
	"tx_ampdu_len:47-55",
	"tx_ampdu_len:56-79",
	"tx_ampdu_len:80-103",
	"tx_ampdu_len:104-127",
	"tx_ampdu_len:128-151",
	"tx_ampdu_len:152-175",
	"tx_ampdu_len:176-199",
	"tx_ampdu_len:200-223",
	"tx_ampdu_len:224-247",
	"ba_miss_count",
	"tx_beamformer_ppdu_iBF",
	"tx_beamformer_ppdu_eBF",
	"tx_beamformer_rx_feedback_all",
	"tx_beamformer_rx_feedback_he",
	"tx_beamformer_rx_feedback_vht",
	"tx_beamformer_rx_feedback_ht",
	"tx_beamformer_rx_feedback_bw", /* zero based idx: 20, 40, 80, 160 */
	"tx_beamformer_rx_feedback_nc",
	"tx_beamformer_rx_feedback_nr",
	"tx_beamformee_ok_feedback_pkts",
	"tx_beamformee_feedback_trig",
	"tx_mu_beamforming",
	"tx_mu_mpdu",
	"tx_mu_successful_mpdu",
	"tx_su_successful_mpdu",
	"tx_msdu_pack_1",
	"tx_msdu_pack_2",
	"tx_msdu_pack_3",
	"tx_msdu_pack_4",
	"tx_msdu_pack_5",
	"tx_msdu_pack_6",
	"tx_msdu_pack_7",
	"tx_msdu_pack_8",

	/* rx counters */
	"rx_fifo_full_cnt",
	"rx_mpdu_cnt",
	"channel_idle_cnt",
	"rx_vector_mismatch_cnt",
	"rx_delimiter_fail_cnt",
	"rx_len_mismatch_cnt",
	"rx_ampdu_cnt",
	"rx_ampdu_bytes_cnt",
	"rx_ampdu_valid_subframe_cnt",
	"rx_ampdu_valid_subframe_b_cnt",
	"rx_pfdrop_cnt",
	"rx_vec_queue_overflow_drop_cnt",
	"rx_ba_cnt",

	/* driver rx counters */
	"d_rx_skb",
	"d_rx_rxd2_amsdu_err",
	"d_rx_null_channels",
	"d_rx_max_len_err",
	"d_rx_too_short",
	"d_rx_bad_ht_rix",
	"d_rx_bad_vht_rix",
	"d_rx_bad_mode",
	"d_rx_bad_bw",

	/* per vif counters */
	"v_tx_mpdu_attempts", /* counting any retries (all frames) */
	"v_tx_mpdu_fail",  /* frames that failed even after retry (all frames) */
	"v_tx_mpdu_retry", /* number of times frames were retried (all frames) */
	"v_tx_mpdu_ok", /* frames that succeeded, perhaps after retry (all frames) */

	"v_txo_tx_mpdu_attempts", /* counting any retries, txo frames */
	"v_txo_tx_mpdu_fail",  /* frames that failed even after retry, txo frames */
	"v_txo_tx_mpdu_retry", /* number of times frames were retried, txo frames */
	"v_txo_tx_mpdu_ok", /* frames that succeeded, perhaps after retry, txo frames */

	"v_tx_mode_cck",
	"v_tx_mode_ofdm",
	"v_tx_mode_ht",
	"v_tx_mode_ht_gf",
	"v_tx_mode_vht",
	"v_tx_mode_he_su",
	"v_tx_mode_he_ext_su",
	"v_tx_mode_he_tb",
	"v_tx_mode_he_mu",
	"v_tx_mode_eht_su",
	"v_tx_mode_eht_trig",
	"v_tx_mode_eht_mu",
	"v_tx_bw_20",
	"v_tx_bw_40",
	"v_tx_bw_80",
	"v_tx_bw_160",
	"v_tx_bw_320",
	"v_tx_mcs_0",
	"v_tx_mcs_1",
	"v_tx_mcs_2",
	"v_tx_mcs_3",
	"v_tx_mcs_4",
	"v_tx_mcs_5",
	"v_tx_mcs_6",
	"v_tx_mcs_7",
	"v_tx_mcs_8",
	"v_tx_mcs_9",
	"v_tx_mcs_10",
	"v_tx_mcs_11",
	"v_tx_mcs_12",
	"v_tx_mcs_13",
	"v_tx_nss_1",
	"v_tx_nss_2",
	"v_tx_nss_3",
	"v_tx_nss_4",

	/* per-vif rx counters */
	"v_rx_nss_1",
	"v_rx_nss_2",
	"v_rx_nss_3",
	"v_rx_nss_4",
	"v_rx_mode_cck",
	"v_rx_mode_ofdm",
	"v_rx_mode_ht",
	"v_rx_mode_ht_gf",
	"v_rx_mode_vht",
	"v_rx_mode_he_su",
	"v_rx_mode_he_ext_su",
	"v_rx_mode_he_tb",
	"v_rx_mode_he_mu",
	"v_rx_mode_eht_su",
	"v_rx_mode_eht_trig",
	"v_rx_mode_eht_mu",
	"v_rx_bw_20",
	"v_rx_bw_40",
	"v_rx_bw_80",
	"v_rx_bw_160",
	"v_rx_bw_320",
	"v_rx_bw_he_ru",
	"v_rx_ru_106",

	"v_rx_mcs_0",
	"v_rx_mcs_1",
	"v_rx_mcs_2",
	"v_rx_mcs_3",
	"v_rx_mcs_4",
	"v_rx_mcs_5",
	"v_rx_mcs_6",
	"v_rx_mcs_7",
	"v_rx_mcs_8",
	"v_rx_mcs_9",
	"v_rx_mcs_10",
	"v_rx_mcs_11",
	"v_rx_mcs_12",
	"v_rx_mcs_13",

	"rx_ampdu_len:0-1",
	"rx_ampdu_len:2-10",
	"rx_ampdu_len:11-19",
	"rx_ampdu_len:20-28",
	"rx_ampdu_len:29-37",
	"rx_ampdu_len:38-46",
	"rx_ampdu_len:47-55",
	"rx_ampdu_len:56-79",
	"rx_ampdu_len:80-103",
	"rx_ampdu_len:104-127",
	"rx_ampdu_len:128-151",
	"rx_ampdu_len:152-175",
	"rx_ampdu_len:176-199",
	"rx_ampdu_len:200-223",
	"rx_ampdu_len:224-247",
};

#define MT7996_SSTATS_LEN ARRAY_SIZE(mt7996_gstrings_stats)

/* Ethtool related API */
static
void mt7996_get_et_strings(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif,
			   u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS)
		memcpy(data, mt7996_gstrings_stats,
		       sizeof(mt7996_gstrings_stats));
}

static
int mt7996_get_et_sset_count(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif, int sset)
{
	if (sset == ETH_SS_STATS)
		return MT7996_SSTATS_LEN;

	return 0;
}

static void mt7996_ethtool_worker(void *wi_data, struct ieee80211_sta *sta)
{
	struct mt76_ethtool_worker_info *wi = wi_data;
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;

	if (msta->vif->mt76.idx != wi->idx)
		return;

	mt76_ethtool_worker(wi, &msta->wcid.stats, true);
}

static
void mt7996_get_et_stats(struct ieee80211_hw *hw,
			 struct ieee80211_vif *vif,
			 struct ethtool_stats *stats, u64 *data)
{
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt76_mib_stats *mib = &phy->mib;
	struct mt76_ethtool_worker_info wi = {
		.data = data,
		.idx = mvif->mt76.idx,
		.has_eht = true,
	};
	/* See mt7996_ampdu_stat_read_phy, etc */
	int i, ei = 0;

	mutex_lock(&dev->mt76.mutex);

	mt7996_mac_update_stats(phy);

	/* driver phy-wide stats */
	data[ei++] = mib->tx_pkts_nic;
	data[ei++] = mib->tx_bytes_nic;
	data[ei++] = mib->rx_pkts_nic;
	data[ei++] = mib->rx_bytes_nic;

	/* MIB stats from FW/HW */
	data[ei++] = mib->tx_ampdu_cnt;
	data[ei++] = mib->tx_stop_q_empty_cnt;
	data[ei++] = mib->tx_mpdu_attempts_cnt;
	data[ei++] = mib->tx_mpdu_success_cnt;
	data[ei++] = mib->tx_rwp_fail_cnt;
	data[ei++] = mib->tx_rwp_need_cnt;
	data[ei++] = mib->tx_bf_ebf_ppdu_cnt;
	data[ei++] = mib->tx_bf_ibf_ppdu_cnt;

	/* Tx ampdu stat */
	for (i = 0; i < 15 /*ARRAY_SIZE(bound)*/; i++)
		data[ei++] = phy->mt76->aggr_stats[i];
	data[ei++] = phy->mib.ba_miss_cnt;

	/* Tx Beamformer monitor */
	data[ei++] = mib->tx_bf_ibf_ppdu_cnt;
	data[ei++] = mib->tx_bf_ebf_ppdu_cnt;

	/* Tx Beamformer Rx feedback monitor */
	data[ei++] = mib->tx_bf_rx_fb_all_cnt;
	data[ei++] = mib->tx_bf_rx_fb_he_cnt;
	data[ei++] = mib->tx_bf_rx_fb_vht_cnt;
	data[ei++] = mib->tx_bf_rx_fb_ht_cnt;

	data[ei++] = mib->tx_bf_rx_fb_bw;
	data[ei++] = mib->tx_bf_rx_fb_nc_cnt;
	data[ei++] = mib->tx_bf_rx_fb_nr_cnt;

	/* Tx Beamformee Rx NDPA & Tx feedback report */
	data[ei++] = mib->tx_bf_fb_cpl_cnt;
	data[ei++] = mib->tx_bf_fb_trig_cnt;

	/* Tx SU & MU counters */
	data[ei++] = mib->tx_mu_bf_cnt;
	data[ei++] = mib->tx_mu_mpdu_cnt;
	data[ei++] = mib->tx_mu_acked_mpdu_cnt;
	data[ei++] = mib->tx_su_acked_mpdu_cnt;

	/* Tx amsdu info (pack-count histogram) */
	for (i = 0; i < ARRAY_SIZE(mib->tx_amsdu); i++)
		data[ei++] = mib->tx_amsdu[i];

	/* rx counters */
	data[ei++] = mib->rx_fifo_full_cnt;
	data[ei++] = mib->rx_mpdu_cnt;
	data[ei++] = mib->channel_idle_cnt;
	data[ei++] = mib->rx_vector_mismatch_cnt;
	data[ei++] = mib->rx_delimiter_fail_cnt;
	data[ei++] = mib->rx_len_mismatch_cnt;
	data[ei++] = mib->rx_ampdu_cnt;
	data[ei++] = mib->rx_ampdu_bytes_cnt;
	data[ei++] = mib->rx_ampdu_valid_subframe_cnt;
	data[ei++] = mib->rx_ampdu_valid_subframe_bytes_cnt;
	data[ei++] = mib->rx_pfdrop_cnt;
	data[ei++] = mib->rx_vec_queue_overflow_drop_cnt;
	data[ei++] = mib->rx_ba_cnt;

	/* rx stats from driver */
	data[ei++] = mib->rx_d_skb;
	data[ei++] = mib->rx_d_rxd2_amsdu_err;
	data[ei++] = mib->rx_d_null_channels;
	data[ei++] = mib->rx_d_max_len_err;
	data[ei++] = mib->rx_d_too_short;
	data[ei++] = mib->rx_d_bad_ht_rix;
	data[ei++] = mib->rx_d_bad_vht_rix;
	data[ei++] = mib->rx_d_bad_mode;
	data[ei++] = mib->rx_d_bad_bw;

	/* Add values for all stations owned by this vif */
	wi.initial_stat_idx = ei;
	ieee80211_iterate_stations_atomic(hw, mt7996_ethtool_worker, &wi);

	mutex_unlock(&dev->mt76.mutex);

	if (wi.sta_count == 0)
		return;

	ei += wi.worker_stat_count;
	if (ei != MT7996_SSTATS_LEN)
		dev_err(dev->mt76.dev, "ei: %d  MT7996_SSTATS_LEN: %d",
			ei, (int)MT7996_SSTATS_LEN);
}

static void
mt7996_twt_teardown_request(struct ieee80211_hw *hw,
			    struct ieee80211_sta *sta,
			    u8 flowid)
{
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);

	mutex_lock(&dev->mt76.mutex);
	mt7996_mac_twt_teardown_flow(dev, msta, flowid);
	mutex_unlock(&dev->mt76.mutex);
}

static int
mt7996_set_radar_background(struct ieee80211_hw *hw,
			    struct cfg80211_chan_def *chandef)
{
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mt7996_dev *dev = phy->dev;
	int ret = -EINVAL;
	bool running;

	mutex_lock(&dev->mt76.mutex);

	if (dev->mt76.region == NL80211_DFS_UNSET)
		goto out;

	if (dev->rdd2_phy && dev->rdd2_phy != phy) {
		/* rdd2 is already locked */
		ret = -EBUSY;
		goto out;
	}

	/* rdd2 already configured on a radar channel */
	running = dev->rdd2_phy &&
		  cfg80211_chandef_valid(&dev->rdd2_chandef) &&
		  !!(dev->rdd2_chandef.chan->flags & IEEE80211_CHAN_RADAR);

	if (!chandef || running ||
	    !(chandef->chan->flags & IEEE80211_CHAN_RADAR)) {
		ret = mt7996_mcu_rdd_background_enable(phy, NULL);
		if (ret)
			goto out;

		if (!running)
			goto update_phy;
	}

	ret = mt7996_mcu_rdd_background_enable(phy, chandef);
	if (ret)
		goto out;

update_phy:
	dev->rdd2_phy = chandef ? phy : NULL;
	if (chandef)
		dev->rdd2_chandef = *chandef;
out:
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

#ifdef CONFIG_NET_MEDIATEK_SOC_WED
static int
mt7996_net_fill_forward_path(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif,
			     struct ieee80211_sta *sta,
			     struct net_device_path_ctx *ctx,
			     struct net_device_path *path)
{
	struct mt7996_vif *mvif = (struct mt7996_vif *)vif->drv_priv;
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct mt7996_dev *dev = mt7996_hw_dev(hw);
	struct mt7996_phy *phy = mt7996_hw_phy(hw);
	struct mtk_wed_device *wed = &dev->mt76.mmio.wed;

	if (phy != &dev->phy && phy->mt76->band_idx == MT_BAND2)
		wed = &dev->mt76.mmio.wed_hif2;

	if (!mtk_wed_device_active(wed))
		return -ENODEV;

	if (msta->wcid.idx > MT7996_WTBL_STA)
		return -EIO;

	path->type = DEV_PATH_MTK_WDMA;
	path->dev = ctx->dev;
	path->mtk_wdma.wdma_idx = wed->wdma_idx;
	path->mtk_wdma.bss = mvif->mt76.idx;
	path->mtk_wdma.queue = 0;
	path->mtk_wdma.wcid = msta->wcid.idx;

	path->mtk_wdma.amsdu = mtk_wed_is_amsdu_supported(wed);
	ctx->dev = NULL;

	return 0;
}

#endif

const struct ieee80211_ops mt7996_ops = {
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx = ieee80211_emulate_switch_vif_chanctx,
	.tx = mt7996_tx,
	.start = mt7996_start,
	.stop = mt7996_stop,
	.add_interface = mt7996_add_interface,
	.remove_interface = mt7996_remove_interface,
	.config = mt7996_config,
	.conf_tx = mt7996_conf_tx,
	.configure_filter = mt7996_configure_filter,
	.bss_info_changed = mt7996_bss_info_changed,
	.sta_add = mt7996_sta_add,
	.sta_remove = mt7996_sta_remove,
	.sta_pre_rcu_remove = mt76_sta_pre_rcu_remove,
	.link_sta_rc_update = mt7996_sta_rc_update,
	.set_key = mt7996_set_key,
	.ampdu_action = mt7996_ampdu_action,
	.set_rts_threshold = mt7996_set_rts_threshold,
	.wake_tx_queue = mt76_wake_tx_queue,
	.sw_scan_start = mt76_sw_scan,
	.sw_scan_complete = mt76_sw_scan_complete,
	.release_buffered_frames = mt76_release_buffered_frames,
	.get_txpower = mt76_get_txpower,
	.channel_switch_beacon = mt7996_channel_switch_beacon,
	.get_stats = mt7996_get_stats,
	.get_et_sset_count = mt7996_get_et_sset_count,
	.get_et_stats = mt7996_get_et_stats,
	.get_et_strings = mt7996_get_et_strings,
	.get_tsf = mt7996_get_tsf,
	.set_tsf = mt7996_set_tsf,
	.offset_tsf = mt7996_offset_tsf,
	.get_survey = mt76_get_survey,
	.get_antenna = mt76_get_antenna,
	.set_antenna = mt7996_set_antenna,
	.set_bitrate_mask = mt7996_set_bitrate_mask,
	.set_coverage_class = mt7996_set_coverage_class,
	.sta_statistics = mt7996_sta_statistics,
	.sta_set_4addr = mt7996_sta_set_4addr,
	.sta_set_decap_offload = mt7996_sta_set_decap_offload,
	.add_twt_setup = mt7996_mac_add_twt_setup,
	.twt_teardown_request = mt7996_twt_teardown_request,
	CFG80211_TESTMODE_CMD(mt76_testmode_cmd)
	CFG80211_TESTMODE_DUMP(mt76_testmode_dump)
#ifdef CONFIG_MAC80211_DEBUGFS
	.sta_add_debugfs = mt7996_sta_add_debugfs,
#endif
	.set_radar_background = mt7996_set_radar_background,
#ifdef CONFIG_NET_MEDIATEK_SOC_WED
	.net_fill_forward_path = mt7996_net_fill_forward_path,
	.net_setup_tc = mt76_wed_net_setup_tc,
#endif
};
