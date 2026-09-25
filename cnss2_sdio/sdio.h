/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _CNSS_SDIO_H
#define _CNSS_SDIO_H

#include "main.h"
#ifdef CONFIG_SDIO_QCN
#define MESON_CORE_PATH "/data/vendor/tombstones/meson_core.bin"

struct cnss_sdio_data {
	struct cnss_plat_data *plat_priv;
	struct sdio_al_client_handle *al_client_handle;
	struct cnss_sdio_wlan_driver *ops;
	struct sdio_device_id *device_id;
	void *client_priv;
	struct delayed_work time_sync_work;
};

int cnss_sdio_init(struct cnss_plat_data *plat_priv);
int cnss_sdio_deinit(struct cnss_plat_data *plat_priv);
int cnss_sdio_register_driver_hdlr(struct cnss_sdio_data *sdio_info,
				   void *data);
int cnss_sdio_unregister_driver_hdlr(struct cnss_sdio_data *sdio_info);
int cnss_sdio_call_driver_probe(struct cnss_sdio_data *sdio_priv);
int cnss_sdio_call_driver_remove(struct cnss_sdio_data *sdio_priv);
int cnss_sdio_dev_shutdown(struct cnss_sdio_data *sdio_priv);
int cnss_sdio_dev_powerup(struct cnss_sdio_data *sdio_priv);
void cnss_sdio_fw_boot_timeout_hdlr(void *bus_priv);
void cnss_sdio_send_hang_event(struct cnss_sdio_data *sdio_priv, char *dump_file_path);
int cnss_sdio_call_driver_uevent(struct cnss_sdio_data *sdio_priv,
				enum cnss_driver_status status, void *data);
int cnss_sdio_set_therm_cdev_state(struct cnss_sdio_data *cnss_info,
				  unsigned long thermal_state, int tcdev_id);
int cnss_sdio_force_fw_assert_hdlr(struct cnss_sdio_data *cnss_info);

static inline struct cnss_sdio_data *cnss_get_sdio_priv(struct sdio_func *sdio_dev)
{
	return sdio_get_drvdata(sdio_dev);
}

static inline struct cnss_plat_data *cnss_sdio_priv_to_plat_priv(void *bus_priv)
{
	struct cnss_sdio_data *sdio_priv = bus_priv;

	return sdio_priv->plat_priv;
}

#else
inline int cnss_sdio_init(void *plat_priv)
{
	return -EINVAL;
}

inline int cnss_sdio_deinit(void *plat_priv)
{
	return -EINVAL;
}

inline int cnss_sdio_register_driver_hdlr(void *sdio_info,
				   void *data)
{
	return -EINVAL;
}

inline int cnss_sdio_unregister_driver_hdlr(void *sdio_info)
{
	return -EINVAL;
}

inline int cnss_sdio_call_driver_probe(void *sdio_priv)
{
	return -EINVAL;
}

inline int cnss_sdio_call_driver_remove(void *sdio_priv)
{
	return -EINVAL;
}

inline int cnss_sdio_dev_shutdown(struct cnss_sdio_data *sdio_priv)
{
	return -EINVAL;
}

inline int cnss_sdio_dev_powerup(struct cnss_sdio_data *sdio_priv)
{
	return -EINVAL;
}

int cnss_sdio_set_therm_cdev_state(struct cnss_sdio_data *cnss_info,
				  unsigned long thermal_state, int tcdev_id)
{
	return -EINVAL;
}

int cnss_sdio_force_fw_assert_hdlr(struct cnss_sdio_data *cnss_info)
{
	return -EINVAL;
}

#endif

void cnss_sdio_start_time_sync_update(struct cnss_plat_data *plat_priv);
void cnss_sdio_stop_time_sync_update(struct cnss_sdio_data *cnss_info);

int cnss_sdio_update_time_sync_period(struct cnss_sdio_data *sdio_info,
				      unsigned int time_sync_period);

#endif
