// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "bus.h"
#include "debug.h"
#include "sdio.h"

enum cnss_dev_bus_type cnss_get_dev_bus_type(struct device *dev)
{
	if (!dev)
		return CNSS_BUS_NONE;

	if (!dev->bus)
		return CNSS_BUS_NONE;

	if (memcmp(dev->bus->name, "sdio", 4) == 0)
		return CNSS_BUS_SDIO;
	else
		return CNSS_BUS_NONE;
}

enum cnss_dev_bus_type cnss_get_bus_type(struct cnss_plat_data *plat_priv)
{
	switch (plat_priv->device_id) {
	case QCN7605_SDIO_DEVICE_ID:
		return CNSS_BUS_SDIO;
	default:
		cnss_pr_err("Unknown device_id: 0x%lx\n", plat_priv->device_id);
		return CNSS_BUS_NONE;
	}
}

void *cnss_bus_dev_to_bus_priv(struct device *dev)
{
	if (!dev)
		return NULL;

	switch (cnss_get_dev_bus_type(dev)) {
	case CNSS_BUS_SDIO:
		return cnss_get_sdio_priv(dev_to_sdio_func(dev));
	default:
		return NULL;
	}
}

struct cnss_plat_data *cnss_bus_dev_to_plat_priv(struct device *dev)
{
	void *bus_priv;

	if (!dev)
		return cnss_get_plat_priv(NULL);

	bus_priv = cnss_bus_dev_to_bus_priv(dev);
	if (!bus_priv)
		return NULL;

	switch (cnss_get_dev_bus_type(dev)) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_priv_to_plat_priv(bus_priv);
	default:
		return NULL;
	}
}

int cnss_bus_init(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_init(plat_priv);
	default:
		cnss_pr_err("Unsupported bus type: %d\n",
			    plat_priv->bus_type);
		return -EINVAL;
	}
}

void cnss_bus_deinit(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		cnss_sdio_deinit(plat_priv);
		return;
	default:
		cnss_pr_err("Unsupported bus type: %d\n",
			    plat_priv->bus_type);
		return;
	}
}

int cnss_bus_dev_powerup(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_dev_powerup(plat_priv->bus_priv);
	default:
		cnss_pr_err("Unsupported bus type: %d\n",
			    plat_priv->bus_type);
		return -EINVAL;
	}
}

int cnss_bus_dev_shutdown(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_dev_shutdown(plat_priv->bus_priv);
	default:
		cnss_pr_err("Unsupported bus type: %d\n",
			    plat_priv->bus_type);
		return -EINVAL;
	}
}

int cnss_bus_call_driver_probe(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_call_driver_probe(plat_priv->bus_priv);
	default:
		cnss_pr_err("Unsupported bus type: %d\n",
			    plat_priv->bus_type);
		return -EINVAL;
	}
}

int cnss_bus_register_driver_hdlr(struct cnss_plat_data *plat_priv, void *data)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_register_driver_hdlr(plat_priv->bus_priv,
						      data);
	default:
		cnss_pr_err("Unsupported bus type: %d\n",
			    plat_priv->bus_type);
		return -EINVAL;
	}
}

int cnss_bus_unregister_driver_hdlr(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_unregister_driver_hdlr(plat_priv->bus_priv);
	default:
		cnss_pr_err("Unsupported bus type: %d\n",
			    plat_priv->bus_type);
		return -EINVAL;
	}
}

int cnss_bus_set_therm_cdev_state(struct cnss_plat_data *plat_priv,
				unsigned long thermal_state, int tcdev_id)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_set_therm_cdev_state(plat_priv->bus_priv,
						     thermal_state,
						     tcdev_id);
	default:
		cnss_pr_err("Unsupported bus type: %d\n", plat_priv->bus_type);
		return -EINVAL;
	}
}

/**
 * cnss_bus_update_time_sync_period() - Update time sync period for the bus
 * @plat_priv: Platform private data structure
 * @time_sync_period: New time sync period in milliseconds
 *
 * This function updates the time synchronization period for the specified bus.
 * It routes the call to the appropriate bus-specific implementation.
 *
 * Return: 0 on success, negative error code on failure
 */
int cnss_bus_update_time_sync_period(struct cnss_plat_data *plat_priv,
				     unsigned int time_sync_period)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_update_time_sync_period(plat_priv->bus_priv,
							time_sync_period);
	default:
		cnss_pr_err("Unsupported bus type: %d\n", plat_priv->bus_type);
		return -EINVAL;
	}
}

int cnss_bus_force_fw_assert_hdlr(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	switch (plat_priv->bus_type) {
	case CNSS_BUS_SDIO:
		return cnss_sdio_force_fw_assert_hdlr(plat_priv->bus_priv);
	default:
		cnss_pr_err("Unsupported bus type: %d\n", plat_priv->bus_type);
		return -EINVAL;
	}
}
