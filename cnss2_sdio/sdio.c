// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/platform_device.h>
#include "main.h"
#include "sdio.h"
#include "debug.h"
#include "bus.h"
#include "genl.h"
#include <linux/string.h>
#include <linux/slab.h>
#include <smc_client_driver.h>

#define TIME_SYNC_TIMEOUT_MS 200
#define TIME_SYNC_RETRY_IN_BUSY 1000
#define HANG_EVENT_COMPLETION_TIMEOUT 2000
#define SMC_POWER_TRANSITION_TIMEOUT 10000

static struct cnss_sdio_wlan_driver *cnss_driver_ops;
struct completion hang_event_complete;

static void cnss_sdio_time_sync_work_hdlr(struct work_struct *work)
{
	struct cnss_sdio_data *cnss_info =
		container_of(work, struct cnss_sdio_data, time_sync_work.work);
	struct cnss_plat_data *plat_priv = cnss_info->plat_priv;
	unsigned int time_sync_period_ms;
	unsigned long ret;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return;
	}

	time_sync_period_ms = plat_priv->ctrl_params.time_sync_period;
	if (!time_sync_period_ms) {
		cnss_pr_dbg("Skip time sync as time period is 0\n");
		return;
	}

	if (test_bit(CNSS_DEVICE_RESET, &plat_priv->driver_state) ||
	    test_bit(CNSS_DRIVER_LOADING, &plat_priv->driver_state) ||
	    test_bit(CNSS_DRIVER_UNLOADING, &plat_priv->driver_state) ||
	    test_bit(CNSS_DEV_REMOVED, &plat_priv->driver_state) ||
	    test_bit(CNSS_DRIVER_IDLE_RESTART, &plat_priv->driver_state) ||
	    test_bit(CNSS_DRIVER_IDLE_SHUTDOWN, &plat_priv->driver_state) ||
	    test_bit(CNSS_DRIVER_SHUTDOWN, &plat_priv->driver_state) ||
	    test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state) ||
	    test_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_priv->driver_state)) {
		time_sync_period_ms = TIME_SYNC_RETRY_IN_BUSY;
		cnss_pr_err("Critical driver state %lu, retry time sync in %d ms\n",
			    plat_priv->driver_state, time_sync_period_ms);
	} else {
		cnss_pr_dbg("Trigger time sync\n");
		cnss_sdio_wlan_time_sync();

		reinit_completion(&plat_priv->time_sync_notif);
		ret = wait_for_completion_timeout(
				&plat_priv->time_sync_notif,
				msecs_to_jiffies(TIME_SYNC_TIMEOUT_MS));
		if (!ret)
			cnss_pr_err("Time sync completion timeout\n");
	}

	schedule_delayed_work(&cnss_info->time_sync_work,
			      msecs_to_jiffies(time_sync_period_ms));
}

void cnss_sdio_start_time_sync_update(struct cnss_plat_data *plat_priv)
{
	struct cnss_sdio_data *cnss_info;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return;
	}

	cnss_info = plat_priv->bus_priv;
	if ((!cnss_info) ||
	    (!cnss_info->al_client_handle)) {
		cnss_pr_err("cnss_info is NULL\n");
		return;
	}

	cnss_sdio_time_sync_work_hdlr(&cnss_info->time_sync_work.work);
}
EXPORT_SYMBOL(cnss_sdio_start_time_sync_update);

void cnss_sdio_stop_time_sync_update(struct cnss_sdio_data *cnss_info)
{
	struct cnss_plat_data *plat_priv;

	if ((!cnss_info) ||
	    (!cnss_info->al_client_handle)) {
		cnss_pr_err("cnss_info is NULL\n");
		return;
	}

	plat_priv = cnss_info->plat_priv;
	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return;
	}

	complete(&plat_priv->time_sync_notif);
	cancel_delayed_work_sync(&cnss_info->time_sync_work);
}
EXPORT_SYMBOL(cnss_sdio_stop_time_sync_update);

int cnss_sdio_update_time_sync_period(struct cnss_sdio_data *cnss_info,
				      unsigned int time_sync_period)
{
	struct cnss_plat_data *plat_priv;

	if ((!cnss_info) ||
	    (!cnss_info->al_client_handle)) {
		cnss_pr_err("cnss_info is NULL\n");
		return -ENODEV;
	}

	plat_priv = cnss_info->plat_priv;
	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	/* To avoid race condition between stop and start */
	mutex_lock(&plat_priv->ts_lock);
	cnss_sdio_stop_time_sync_update(cnss_info);
	plat_priv->ctrl_params.time_sync_period = time_sync_period;
	schedule_delayed_work(&cnss_info->time_sync_work,
			      msecs_to_jiffies(time_sync_period));
	mutex_unlock(&plat_priv->ts_lock);

	return 0;
}

int cnss_sdio_call_driver_probe(struct cnss_sdio_data *sdio_priv)
{
	int ret = 0;
	struct cnss_plat_data *plat_priv = sdio_priv->plat_priv;

	cnss_pr_info("Host driver probe, driver_state: 0x%lx",
		plat_priv->driver_state);

	if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
		cnss_pr_info("Reboot is in progress, skip driver probe");
		return -EINVAL;
	}

	if ((!sdio_priv->al_client_handle) ||
	    (!sdio_priv->al_client_handle->func)) {
		ret = -ENODEV;
		goto out;
	}

	if (!sdio_priv->ops) {
		cnss_pr_err("driver_ops is NULL\n");
		ret = -EINVAL;
		goto out;
	}

	if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
		cnss_pr_err("Reboot is in progress, skip driver probe\n");
		return -EINVAL;
	}

	if (test_bit(CNSS_DRIVER_PROBING, &plat_priv->driver_state) ||
	    test_bit(CNSS_DRIVER_LOADING, &plat_priv->driver_state)) {
		ret = sdio_priv->ops->probe(sdio_priv->al_client_handle->func,
					    sdio_priv->device_id);
		if (ret) {
			cnss_pr_err("Failed to probe host driver, err = %d\n",
				    ret);
			goto out;
		}
		clear_bit(CNSS_DRIVER_PROBING, &plat_priv->driver_state);
		set_bit(CNSS_DRIVER_PROBED, &plat_priv->driver_state);
	} else if (test_bit(CNSS_DRIVER_IDLE_RESTART,
			    &plat_priv->driver_state)) {
		ret = sdio_priv->ops->idle_restart(
					sdio_priv->al_client_handle->func);
		if (ret) {
			cnss_pr_err("Failed to restart host driver, err = %d\n",
				    ret);
			goto out;
		}
		clear_bit(CNSS_DRIVER_IDLE_RESTART, &plat_priv->driver_state);
	} else if (test_bit(CNSS_DRIVER_REINIT, &plat_priv->driver_state) &&
		   test_bit(CNSS_DRIVER_PROBED, &plat_priv->driver_state)) {
		ret = sdio_priv->ops->reinit(
					sdio_priv->al_client_handle->func, sdio_priv->device_id);

		clear_bit(CNSS_DRIVER_FW_SSR_IN_PROGRESS, &plat_priv->driver_state);
		clear_bit(CNSS_DRIVER_SMC_SSR_IN_PROGRESS, &plat_priv->driver_state);

		if (ret) {
			cnss_pr_err("Failed to reinit host driver, err = %d, Reset SMC\n",
					ret);
			smc_recovery_api_for_client();
			goto out;
		}
		clear_bit(CNSS_DRIVER_RECOVERING, &plat_priv->driver_state);
		clear_bit(CNSS_DRIVER_REINIT, &plat_priv->driver_state);
	}

	return 0;

out:
	return ret;
}

int cnss_sdio_call_driver_remove(struct cnss_sdio_data *sdio_priv)
{
	struct cnss_plat_data *plat_priv = sdio_priv->plat_priv;
	int ret = 0;

	cnss_pr_info("Host driver remove, driver_state: 0x%lx",
		plat_priv->driver_state);

	if (!sdio_priv->ops) {
		cnss_pr_err("driver_ops is NULL\n");
		return -EINVAL;
	}

	/* stop the periodic time sync */
	cnss_sdio_stop_time_sync_update(sdio_priv);

	if (test_bit(CNSS_DRIVER_UNLOADING, &plat_priv->driver_state)) {
		sdio_priv->ops->remove(sdio_priv->al_client_handle->func);
		clear_bit(CNSS_DRIVER_PROBED, &plat_priv->driver_state);
		goto exit;
	} else if (test_bit(CNSS_DRIVER_IDLE_SHUTDOWN,
			    &plat_priv->driver_state)) {
		ret = sdio_priv->ops->idle_shutdown(
					sdio_priv->al_client_handle->func);
		if (ret == -EAGAIN) {
			clear_bit(CNSS_DRIVER_IDLE_SHUTDOWN,
				  &plat_priv->driver_state);
			return ret;
		}
		goto exit;
	} else if (test_bit(CNSS_DRIVER_SHUTDOWN, &plat_priv->driver_state)
			&& test_bit(CNSS_DRIVER_PROBED, &plat_priv->driver_state)) {
		/* Clearing pending SDIO transaction */
		qcn_sdio_purge_rw_buff();
		sdio_priv->ops->shutdown(
					sdio_priv->al_client_handle->func);
		goto exit;
	}

	cnss_pr_err("Failed to call driver ops\n");
	return -EINVAL;

exit:
	/* Clearing pending SDIO transaction*/
	qcn_sdio_purge_rw_buff();
	return ret;

}

/**
 * cnss_sdio_wlan_register_driver() - cnss wlan register API
 * @driver: sdio wlan driver interface from wlan driver.
 *
 * wlan sdio function driver uses this API to register callback
 * functions to cnss_sido platform driver. The callback will
 * be invoked by corresponding wrapper function of this cnss
 * platform driver.
 */
int cnss_sdio_wlan_register_driver(struct cnss_sdio_wlan_driver *driver_ops)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	struct cnss_sdio_data *cnss_info;
	int ret = 0;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		cnss_driver_ops = driver_ops;
		return 0;
	}

	cnss_info = plat_priv->bus_priv;
	if ((!cnss_info) ||
	    (!cnss_info->al_client_handle)) {
	    //(!cnss_info->al_client_handle->func)) {
		cnss_pr_err("cnss_info is NULL\n");
		return -ENODEV;
	}

	if (cnss_info->ops) {
		cnss_pr_err("Driver has already registered\n");
		return -EEXIST;
	}
	cnss_info->ops = driver_ops;

	ret = cnss_driver_event_post(plat_priv,
				     CNSS_DRIVER_EVENT_REGISTER_DRIVER,
				     0,
				     driver_ops);
	return ret;
}
EXPORT_SYMBOL(cnss_sdio_wlan_register_driver);

int cnss_sdio_dev_shutdown(struct cnss_sdio_data *sdio_priv)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	int ret;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}


	/*  Early check */
	if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
		cnss_pr_err("Reboot in progress, ignoring shutdown\n");
		return 0;
	}

	ret = cnss_sdio_call_driver_remove(sdio_priv);
	if (ret) {
		cnss_pr_err("driver remove failed %d", ret);
		return ret;
	}

	qcn_sdio_card_release();

	atomic_set(&plat_priv->power_down_retry_cnt, CNSS_SMC_EVENT_RETRY_COUNT);
	reinit_completion(&plat_priv->power_down_complete);

	/*  CRITICAL FIX: mid-flight race protection */
	if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
		cnss_pr_err("Reboot started, skipping wlan_disable\n");
		return 0;
	}

	cnss_pr_info("Disabling WIFI\n");
	ret = cnss_sdio_wlan_disable_subsys();
	set_bit(CNSS_POWER_OFF, &plat_priv->driver_state);

	ret = wait_for_completion_timeout(
				&plat_priv->power_down_complete,
				msecs_to_jiffies(SMC_POWER_TRANSITION_TIMEOUT));

	if (!ret) {
		cnss_pr_err("Timed out turning off Meson");

		if (test_bit(CNSS_IN_REBOOT,
		     &plat_priv->driver_state)) {
			cnss_pr_err("Timeout ignored due to reboot\n");
			return 0;
		}

		BUG();
	}

	return 0;
}

int cnss_sdio_dev_powerup(struct cnss_sdio_data *sdio_priv)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	int ret;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
		cnss_pr_err("Reboot in progress, ignoring powerup \n");
		return 0;
	}

	atomic_set(&plat_priv->power_up_retry_cnt, CNSS_SMC_EVENT_RETRY_COUNT);
	reinit_completion(&plat_priv->power_up_complete);

	cnss_pr_info("Enabling WIFI\n");
	clear_bit(CNSS_POWER_OFF, &plat_priv->driver_state);
	ret = cnss_sdio_wlan_enable_subsys();

	ret = wait_for_completion_timeout(
				&plat_priv->power_up_complete,
				msecs_to_jiffies(SMC_POWER_TRANSITION_TIMEOUT));

	if (!ret) {
		cnss_pr_err("Timed out turning on Meson");

		if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
			cnss_pr_err("Reboot in progress, ignoring powerup \n");
			return 0;
		}

		BUG();
	}

	cnss_pr_info("CNSS SDIO Card resume");
	qcn_sdio_card_claim();

	return cnss_sdio_call_driver_probe(sdio_priv);
}
/**
 * cnss_sdio_wlan_unregister_driver() - cnss wlan unregister API
 * @driver: sdio wlan driver interface from wlan driver.
 *
 * wlan sdio function driver uses this API to detach it from cnss_sido
 * platform driver.
 */
void cnss_sdio_wlan_unregister_driver(struct cnss_sdio_wlan_driver *driver_ops)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return;
	}

	cnss_driver_event_post(plat_priv, CNSS_DRIVER_EVENT_UNREGISTER_DRIVER,
			       CNSS_EVENT_SYNC_UNINTERRUPTIBLE, NULL);
}
EXPORT_SYMBOL(cnss_sdio_wlan_unregister_driver);

struct sdio_al_client_handle *cnss_sdio_wlan_get_sdio_al_client_handle(
				struct sdio_func *func)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	struct cnss_sdio_data *cnss_info = plat_priv->bus_priv;

	return cnss_info->al_client_handle;
}
EXPORT_SYMBOL(cnss_sdio_wlan_get_sdio_al_client_handle);

struct sdio_al_channel_handle *cnss_sdio_wlan_register_sdio_al_channel(
			     struct sdio_al_channel_data *channel_data)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	struct cnss_sdio_data *cnss_info = plat_priv->bus_priv;

	return sdio_al_register_channel(cnss_info->al_client_handle,
					channel_data);
}
EXPORT_SYMBOL(cnss_sdio_wlan_register_sdio_al_channel);

void cnss_sdio_wlan_unregister_sdio_al_channel(
				     struct sdio_al_channel_handle *ch_handle)
{
	sdio_al_deregister_channel(ch_handle);
}
EXPORT_SYMBOL(cnss_sdio_wlan_unregister_sdio_al_channel);

int cnss_sdio_register_driver_hdlr(struct cnss_sdio_data *cnss_info,
				   void *data)
{
	struct cnss_plat_data *plat_priv = cnss_info->plat_priv;
	int ret;

	set_bit(CNSS_DRIVER_LOADING, &plat_priv->driver_state);
	ret = cnss_sdio_dev_powerup(plat_priv->bus_priv);
	//qcn_sdio_card_state(false);
	clear_bit(CNSS_DRIVER_LOADING, &plat_priv->driver_state);

	return ret;
}

int cnss_sdio_unregister_driver_hdlr(struct cnss_sdio_data *cnss_info)
{
	struct cnss_plat_data *plat_priv = cnss_info->plat_priv;

	set_bit(CNSS_DRIVER_UNLOADING, &plat_priv->driver_state);
	//qcn_sdio_card_state(false);
	cnss_sdio_dev_shutdown(plat_priv->bus_priv);
	clear_bit(CNSS_DRIVER_UNLOADING, &plat_priv->driver_state);

	cnss_info->ops = NULL;
	return 0;
}

int cnss_sdio_set_therm_cdev_state(struct cnss_sdio_data *cnss_info,
				  unsigned long thermal_state, int tcdev_id)
{
	if (!cnss_info) {
		cnss_pr_err("cnss_info is NULL!\n");
		return -ENODEV;
	}

	if (!cnss_info->ops || !cnss_info->ops->set_therm_cdev_state) {
		cnss_pr_err("driver_ops or set_therm_cdev_state is NULL\n");
		return -EINVAL;
	}

	return cnss_info->ops->set_therm_cdev_state(cnss_info->al_client_handle->func,
							thermal_state, tcdev_id);
}

int cnss_sdio_force_fw_assert_hdlr(struct cnss_sdio_data *cnss_info)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state) ||
		test_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_priv->driver_state)) {
		cnss_pr_info("SSR/Reboot is in progress, skip force assert\n");
		return -EINVAL;
	}

	return smc_smc_subsys_induce_bug(SUBSYS_WLAN);
}

void cnss_sdio_send_hang_event(struct cnss_sdio_data *sdio_priv, char *dump_file_path)
{
    int ret;
    void *buff = NULL;
    u8 type = CNSS_GENL_MSG_TYPE_HANG_EVENT;
    u32 total_size = 0;


    cnss_pr_err("Entering cnss_sdio_send_hang_event (file-only mode)...");

    if (!sdio_priv) {
	cnss_pr_err("Invalid sdio_priv pointer.");
	return;
    }

    if (!dump_file_path) {
	cnss_pr_err("No dump file path provided for hang event.");
	return;
    }

	reinit_completion(&hang_event_complete);

    // Send zero-length message via Generic Netlink which act as an
    // event with no data to cnss-daemon
    ret = cnss_genl_send_msg(buff, type, dump_file_path, total_size);
    if (ret < 0) {
	pr_err("Failed to send zero-length message: %d\n", ret);
    } else {
	pr_info("Zero-length message sent successfully\n");
    }

	ret = wait_for_completion_timeout(&hang_event_complete,
						msecs_to_jiffies(HANG_EVENT_COMPLETION_TIMEOUT));
	if (!ret) {
		cnss_pr_err("Failed to save meson dumps\n");
	}

	cnss_pr_info("hang event process complete\n");

	return;
}

int cnss_sdio_call_driver_uevent(struct cnss_sdio_data *sdio_priv,
				enum cnss_driver_status status, void *data)
{
	struct cnss_uevent_data uevent_data;
	struct cnss_sdio_wlan_driver *driver_ops;

	driver_ops = sdio_priv->ops;
	if (!driver_ops || !driver_ops->update_event) {
		cnss_pr_dbg("Hang event driver ops is NULL\n");
		return -EINVAL;
	}

	cnss_pr_dbg("Calling driver uevent: %d\n", status);

	uevent_data.status = status;
	uevent_data.data = data;

	return driver_ops->update_event(sdio_priv->al_client_handle->func, &uevent_data);
}

static int cnss_sdio_probe(struct sdio_al_client_handle *pal_cli_handle)
{
	struct cnss_sdio_data *sdio_info = pal_cli_handle->client_priv;
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	struct sdio_device_id	*device_id;
	int ret;

	if (!plat_priv) {
		cnss_pr_err("CNSS SDIO AL plat_priv is NULL\n");
		return -ENODEV;
	}

	if (!sdio_info || !sdio_info->ops) {
		cnss_pr_err("CNSS SDIO AL Probe failed. Host not available.\n");
		return -ENODEV;
	}

	if (!pal_cli_handle->func) {
		cnss_pr_err("CNSS SDIO AL Probe pal_cli_handle->func is NULL\n");
		return -ENODEV;
	}

	device_id = devm_kzalloc(&plat_priv->plat_dev->dev,
				 sizeof(struct sdio_device_id),
				 GFP_KERNEL);
	device_id->class = pal_cli_handle->func->class;
	device_id->vendor = pal_cli_handle->func->vendor;
	device_id->device = pal_cli_handle->func->device;
	sdio_info->device_id = device_id;

	init_completion(&hang_event_complete);

	cnss_pr_info("CNSS SDIO AL Probe for device Id: 0x%x ssr_state %d \n",
			     pal_cli_handle->func->device, plat_priv->ssr_state);
	clear_bit(CNSS_DEV_REMOVED, &plat_priv->driver_state);
	plat_priv->device_id = pal_cli_handle->func->device;
	INIT_DELAYED_WORK(&sdio_info->time_sync_work,
			  cnss_sdio_time_sync_work_hdlr);
	if (plat_priv->ssr_state == SSR_STATE_IDLE) {
		set_bit(CNSS_DRIVER_PROBING, &plat_priv->driver_state);
		plat_priv->ssr_state = SSR_STATE_IDLE;
		ret = sdio_info->ops->probe(sdio_info->al_client_handle->func,
					    sdio_info->device_id);
		cnss_pr_info("Driver Probed\n");
		if (ret) {
			cnss_pr_err("Failed to probe host driver, err = %d\n",
				    ret);
			return 0;
		}

		clear_bit(CNSS_DRIVER_PROBING, &plat_priv->driver_state);
		set_bit(CNSS_DRIVER_PROBED, &plat_priv->driver_state);
	}
	return 0;
}

static int cnss_sdio_remove(struct sdio_al_client_handle *pal_cli_handle)
{
	struct cnss_sdio_data *sdio_info = pal_cli_handle->client_priv;
	struct cnss_plat_data *plat_priv = sdio_info->plat_priv;

	if (pal_cli_handle->func)
		cnss_pr_err(
		"SDIO AL remove for device Id: 0x%x in driver state %lu ssr_state %d\n",
		pal_cli_handle->func->device,
		plat_priv->driver_state, plat_priv->ssr_state);
		set_bit(CNSS_DEV_REMOVED, &plat_priv->driver_state);

	if (test_bit(CNSS_DRIVER_PROBED, &plat_priv->driver_state)) {
		cnss_pr_info("Triggering driver_ops remove\n");
		sdio_info->ops->remove(sdio_info->al_client_handle->func);
		clear_bit(CNSS_DRIVER_PROBED, &plat_priv->driver_state);
	}

	devm_kfree(&plat_priv->plat_dev->dev, (void *)sdio_info->device_id);

	return 0;
}

static int cnss_sdio_pm(struct sdio_al_client_handle *pal_cli_handle, enum sdio_al_lpm_event event)
{
	struct cnss_sdio_data *sdio_info = pal_cli_handle->client_priv;
	struct sdio_func *func = sdio_info->al_client_handle->func;
	int ret = 0;

	if (!sdio_info->ops) {
		cnss_pr_err("Ignore LPM event\n");
		goto err;
	}

	cnss_pr_info("Processing LPM event %d\n", event);

	switch (event) {
	case LPM_ENTER:
		ret = sdio_info->ops->suspend ? sdio_info->ops->suspend(&func->dev) : -EINVAL;
		break;
	case LPM_EXIT:
		ret = sdio_info->ops->resume ? sdio_info->ops->resume(&func->dev) : -EINVAL;
		break;
	case LPM_RUNTIME_SUSPEND_ENTER:
		ret = sdio_info->ops->runtime_suspend ? sdio_info->ops->runtime_suspend(&func->dev) : -EINVAL;
		break;
	case LPM_RUNTIME_SUSPEND_EXIT:
		ret = sdio_info->ops->runtime_resume ? sdio_info->ops->runtime_resume(&func->dev) : -EINVAL;
		break;
	case LPM_RESET:
		cnss_schedule_recovery(&func->dev, CNSS_REASON_DEFAULT);
		break;
	default:
		cnss_pr_err("Invalid LPM event\n");
		goto err;
	}

	return ret;
err:
	return -EINVAL;
}

struct sdio_al_client_data al_cli_data = {
	.name = "SDIO_AL_CLIENT_WLAN",
	.probe = cnss_sdio_probe,
	.remove = cnss_sdio_remove,
	.lpm_notify_cb = cnss_sdio_pm,
	.fw_assert_cb = cnss2_sdio_force_fw_assert,
};

int cnss_sdio_init(struct cnss_plat_data *plat_priv)
{
	struct cnss_sdio_data *sdio_info;
	struct sdio_al_client_handle *al_client_handle;
	int ret = 0;

	if (sdio_al_is_ready()) {
		cnss_pr_err("sdio_al not ready, defer probe\n");
		ret = -EPROBE_DEFER;
		goto out;
	}

	if (!qti_client_is_ready()) {
		cnss_pr_err("qti_client not ready, defer probe\n");
		ret = -EPROBE_DEFER;
		goto out;
	}

	al_client_handle = sdio_al_register_client(&al_cli_data);
	if (!al_client_handle) {
		cnss_pr_err("sdio al registration failed!\n");
		ret = -ENODEV;
		goto out;
	}
	sdio_info = devm_kzalloc(&plat_priv->plat_dev->dev, sizeof(*sdio_info),
				 GFP_KERNEL);
	if (!sdio_info) {
		ret = -ENOMEM;
		goto out;
	}
	al_client_handle->client_priv = sdio_info;
	sdio_info->al_client_handle = al_client_handle;
	sdio_info->plat_priv = plat_priv;
	if (cnss_driver_ops)
		sdio_info->ops = cnss_driver_ops;
	plat_priv->bus_priv = sdio_info;

out:
	return ret;
}

int cnss_sdio_deinit(struct cnss_plat_data *plat_priv)
{
	struct cnss_sdio_data *sdio_info = plat_priv->bus_priv;

	sdio_al_deregister_client(sdio_info->al_client_handle);
	return 0;
}
