/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _CAIHONG_KERNEL_API_H
#define _CAIHONG_KERNEL_API_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct power_supply;

#if IS_REACHABLE(CONFIG_BATTERY_QCOM_BATTMGR_CAIHONG)
int qcom_battmgr_caihong_get_raw_capacity(struct power_supply *psy);
int qcom_battmgr_caihong_notify_fastchg_complete(struct power_supply *psy);
#else
static inline int
qcom_battmgr_caihong_get_raw_capacity(struct power_supply *psy)
{
	return -EOPNOTSUPP;
}

static inline int
qcom_battmgr_caihong_notify_fastchg_complete(struct power_supply *psy)
{
	return -EOPNOTSUPP;
}
#endif

#if IS_REACHABLE(CONFIG_TYPEC_MUX_WCD939X_USBSS)
int wcd939x_usbss_set_vooc_dpdm(bool enable);
#else
static inline int wcd939x_usbss_set_vooc_dpdm(bool enable)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _CAIHONG_KERNEL_API_H */
