/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _CNSS_BUS_H
#define _CNSS_BUS_H

#include "main.h"
#include "cnss_common.h"

enum cnss_dev_bus_type cnss_get_dev_bus_type(struct device *dev);
enum cnss_dev_bus_type cnss_get_bus_type(struct cnss_plat_data *plat_priv);
void *cnss_bus_dev_to_bus_priv(struct device *dev);
struct cnss_plat_data *cnss_bus_dev_to_plat_priv(struct device *dev);
int cnss_bus_init(struct cnss_plat_data *plat_priv);
void cnss_bus_deinit(struct cnss_plat_data *plat_priv);

int cnss_bus_call_driver_probe(struct cnss_plat_data *plat_priv);
int cnss_bus_register_driver_hdlr(struct cnss_plat_data *plat_priv, void *data);
int cnss_bus_unregister_driver_hdlr(struct cnss_plat_data *plat_priv);
int cnss_bus_dev_shutdown(struct cnss_plat_data *plat_priv);
int cnss_bus_dev_powerup(struct cnss_plat_data *plat_priv);
int cnss_bus_update_time_sync_period(struct cnss_plat_data *plat_priv,
				     unsigned int time_sync_period);
int cnss_bus_set_therm_cdev_state(struct cnss_plat_data *plat_priv,
				unsigned long thermal_state, int tcdev_id);
int cnss_bus_force_fw_assert_hdlr(struct cnss_plat_data *plat_priv);
#endif /* _CNSS_BUS_H */
