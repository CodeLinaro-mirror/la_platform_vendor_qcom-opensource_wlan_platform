// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/of_device.h>
#include <linux/devcoredump.h>
#include <linux/device.h>
#include <linux/reboot.h>
#include <linux/version.h>
#include <linux/compiler_attributes.h>
#include <linux/thermal.h>

#include "cnss_utils.h"
#include "main.h"
#include "sdio.h"
#include "bus.h"
#include "debug.h"
#include "genl.h"
#ifdef CONFIG_SDIO_QCN
#include <smc_uclient_driver.h>
#include <smc_client_driver.h>
#endif


#define CNSS_EVENT_PENDING		2989
#define CNSS_TIME_SYNC_PERIOD_DEFAULT   900000
#define CNSS_MIN_TIME_SYNC_PERIOD	2000
#define CNSS_TIME_SYNC_PERIOD_INVALID	0xFFFFFFFF

enum cnss_recovery_type {
	CNSS_WLAN_RECOVERY = 0x1,
};

static struct device *cnss_dev;
static struct cnss_plat_data *plat_env;
static bool cnss_allow_driver_loading;
static struct notifier_block smc_them_nb;
static struct smc_notif_info *smc_them_notif;
static bool smc_them_ready;

static void cnss_probe_after_smc_fn(struct work_struct *work);
static DECLARE_WORK(cnss_prb_after_smc_work, cnss_probe_after_smc_fn);

static const struct platform_device_id cnss_platform_id_table[] = {
	{ .name = "qcn7605_sdio", .driver_data = QCN7605_SDIO_DEVICE_ID, },
	{ },
};

static const struct of_device_id cnss_of_match_table[] = {
	{
		.compatible = "qcom,cnss-sdio",
		.data = (void *)&cnss_platform_id_table[0]},
	{ },
};
MODULE_DEVICE_TABLE(of, cnss_of_match_table);

struct cnss_driver_event {
	struct list_head list;
	enum cnss_driver_event_type type;
	bool sync;
	struct completion complete;
	int ret;
	void *data;
};

static char *cnss_driver_event_to_str(enum cnss_driver_event_type type)
{
	switch (type) {
	case CNSS_DRIVER_EVENT_REGISTER_DRIVER:
		return "REGISTER_DRIVER";
	case CNSS_DRIVER_EVENT_UNREGISTER_DRIVER:
		return "UNREGISTER_DRIVER";
	case CNSS_DRIVER_EVENT_DEVICE_UP:
		return "DEVICE_UP";
	case CNSS_DRIVER_EVENT_IDLE_RESTART:
		return "DEVICE_RESTART";
	case CNSS_DRIVER_EVENT_IDLE_SHUTDOWN:
		return "DEVICE_SHUTDOWN";
	case CNSS_DRIVER_EVENT_SHUTDOWN:
		return "DEVICE_SHUTDOWN";
	case CNSS_DRIVER_EVENT_REINIT:
		return "DEVICE_REINIT";
	case CNSS_DRIVER_EVENT_RECOVERY:
		return "CNSS_DRIVER_EVENT_RECOVERY";
	case CNSS_DRIVER_EVENT_FW_DOWN:
		return "CNSS_DRIVER_EVENT_FW_DOWN";
	case CNSS_DRIVER_EVENT_MAX:
		return "EVENT_MAX";
	}

	return "UNKNOWN";
};

static char *qcc_subsys_event_to_str(enum qcc_subsys_event event)
{
	switch (event) {
	case ENABLE_SUBSYS:
		return "ENABLE_SUBSYS";
	case DISABLE_SUBSYS:
		return "DISABLE_SUBSYS";
	case POWER_UP_SUCCESS:
		return "POWER_UP_SUCCESS";
	case POWER_UP_FAIL:
		return "POWER_UP_FAIL";
	case POWER_DOWN_SUCCESS:
		return "POWER_DOWN_SUCCESS";
	case POWER_DOWN_FAIL:
		return "POWER_DOWN_FAIL";
	case SSR_DUMP_BEGIN:
		return "SSR_DUMP_BEGIN";
	case SSR_DUMP_COMPLETED:
		return "SSR_DUMP_COMPLETED";
	case SSR_DUMP_FAILED:
		return "SSR_DUMP_FAILED";
	case TIME_SYNC_INIT:
		return "TIME_SYNC_INIT";
	case TIME_SYNC_COMPLETE:
		return "TIME_SYNC_COMPLETE";
	case TIME_SYNC_FAIL:
		return "TIME_SYNC_FAIL";
	case SMC_VERSION_SUCCESS:
		return "SMC_VERSION_SUCCESS";
	case SMC_VERSION_FAIL:
		return "SMC_VERSION_FAIL";
	case RAMDUMP_AVAILABLE:
		return "RAMDUMP_AVAILABLE";
	case SMC_VERSION_COMPLETE:
		return "SMC_VERSION_COMPLETE";
	case TECH_IP_INDUCE_BUG:
		return "TECH_IP_INDUCE_BUG";
	case TECH_IP_INDUCE_BUG_FAIL:
		return "TECH_IP_INDUCE_BUG_FAIL";
	case TECH_IP_BUG_IPC_SUCCESS:
		return "TECH_IP_BUG_IPC_SUCCESS";
	case TECH_IP_BUG_PEER_NOT_BOOTED:
		return "TECH_IP_BUG_PEER_NOT_BOOTED";
	case TECH_IP_BUG_PEER_IPC_NOT_READY:
		return "TECH_IP_BUG_PEER_IPC_NOT_READY";
	case TECH_IP_BUG_PEER_CLIENT_NOT_READY:
		return "TECH_IP_BUG_PEER_CLIENT_NOT_READY";
	case TECH_IP_BUG_RING_BUFFER_FULL:
		return "TECH_IP_BUG_RING_BUFFER_FULL";
	case TECH_IP_BUG_OUT_OF_MEMORY:
		return "TECH_IP_BUG_OUT_OF_MEMORY";
	case TECH_IP_BUG_IPC_INACTIVE:
		return "TECH_IP_BUG_IPC_INACTIVE";
	case TECH_IP_BUG_IPC_CLIENT_NOT_BOOTED:
		return "TECH_IP_BUG_IPC_CLIENT_NOT_BOOTED";
	case TECH_IP_BUG_INVALID_SUBSYSTEM:
		return "TECH_IP_BUG_INVALID_SUBSYSTEM";
	default:
		return "UNKNOWN";
	}
};

bool cnss_check_driver_loading_allowed(void)
{
	return cnss_allow_driver_loading;
}

static void cnss_set_plat_priv(struct platform_device *plat_dev,
			       struct cnss_plat_data *plat_priv)
{
	plat_env = plat_priv;
}

struct cnss_plat_data *cnss_get_plat_priv(struct platform_device *plat_dev)
{
	return plat_env;
}

static void cnss_clear_plat_priv(struct cnss_plat_data *plat_priv)
{
	plat_env = NULL;
}

static int cnss_set_device_name(struct cnss_plat_data *plat_priv)
{
	snprintf(plat_priv->device_name, sizeof(plat_priv->device_name),
		 "wlan");
	return 0;
}

static void cnss_pm_stay_awake(struct cnss_plat_data *plat_priv)
{
	if (atomic_inc_return(&plat_priv->pm_count) != 1)
		return;

	cnss_pr_dbg("PM stay awake, state: 0x%lx, count: %d\n",
		    plat_priv->driver_state,
		    atomic_read(&plat_priv->pm_count));
	pm_stay_awake(&plat_priv->plat_dev->dev);
}

static void cnss_pm_relax(struct cnss_plat_data *plat_priv)
{
	int r = atomic_dec_return(&plat_priv->pm_count);

	WARN_ON(r < 0);

	if (r != 0)
		return;

	cnss_pr_dbg("PM relax, state: 0x%lx, count: %d\n",
		    plat_priv->driver_state,
		    atomic_read(&plat_priv->pm_count));
	pm_relax(&plat_priv->plat_dev->dev);
}

bool cnss_is_dual_wlan_enabled(void)
{
	return IS_ENABLED(CONFIG_CNSS_SUPPORT_DUAL_DEV);
}

int cnss_driver_event_post(struct cnss_plat_data *plat_priv,
			   enum cnss_driver_event_type type,
			   u32 flags, void *data)
{
	struct cnss_driver_event *event;
	unsigned long irq_flags;
	int gfp = GFP_KERNEL;
	int ret = 0;

	if (!plat_priv)
		return -ENODEV;

	if (type >= CNSS_DRIVER_EVENT_MAX) {
		cnss_pr_err("Invalid Event type: %d, can't post", type);
		return -EINVAL;
	}

	if (in_interrupt() || irqs_disabled())
		gfp = GFP_ATOMIC;

	event = kzalloc(sizeof(*event), gfp);
	if (!event)
		return -ENOMEM;

	cnss_pm_stay_awake(plat_priv);

	event->type = type;
	event->data = data;
	init_completion(&event->complete);
	event->ret = CNSS_EVENT_PENDING;
	event->sync = !!(flags & CNSS_EVENT_SYNC);

	spin_lock_irqsave(&plat_priv->event_lock, irq_flags);
	list_add_tail(&event->list, &plat_priv->event_list);
	spin_unlock_irqrestore(&plat_priv->event_lock, irq_flags);

	queue_work(plat_priv->event_wq, &plat_priv->event_work);

	if (!(flags & CNSS_EVENT_SYNC))
		goto out;

	if (flags & CNSS_EVENT_UNKILLABLE)
		wait_for_completion(&event->complete);
	else if (flags & CNSS_EVENT_UNINTERRUPTIBLE)
		ret = wait_for_completion_killable(&event->complete);
	else
		ret = wait_for_completion_interruptible(&event->complete);

	cnss_pr_dbg("Completed event: %s(%d), state: 0x%lx, ret: %d/%d\n",
		    cnss_driver_event_to_str(type), type,
		    plat_priv->driver_state, ret, event->ret);
	spin_lock_irqsave(&plat_priv->event_lock, irq_flags);
	if (ret == -ERESTARTSYS && event->ret == CNSS_EVENT_PENDING) {
		event->sync = false;
		spin_unlock_irqrestore(&plat_priv->event_lock, irq_flags);
		ret = -EINTR;
		goto out;
	}
	spin_unlock_irqrestore(&plat_priv->event_lock, irq_flags);

	ret = event->ret;
	kfree(event);

out:
	cnss_pm_relax(plat_priv);
	return ret;
}

#ifdef CONFIG_SDIO_QCN
static int cnss_device_up(struct cnss_plat_data *plat_priv)
{
	if (!plat_priv)
		return -ENODEV;

	return cnss_bus_call_driver_probe(plat_priv);
}

int cnss_sdio_get_soc_info(struct cnss_soc_info *soc_info)
{
	SmcVersionInfo_t version;
	int ret;

	if (!soc_info)
		return -EINVAL;

	ret = get_smc_version_info(&version);

	if (ret) {
		cnss_pr_err("Failed to get SMC version info: %d\n", ret);
		return ret;
	}

	ret = strscpy(soc_info->fw_build_id, version.fw_versions.wlan, sizeof(soc_info->fw_build_id));

	if (ret < 0) {
		cnss_pr_err("Failed to copy firmware build ID: %d\n", ret);
		return ret;
	}

	return 0;
}

EXPORT_SYMBOL(cnss_sdio_get_soc_info);
#else
static inline int cnss_device_up(struct cnss_plat_data *plat_priv)
{
	return 0;
}
#endif

static int cnss_power_up_hdlr(struct cnss_plat_data *plat_priv)
{
	int ret;

	ret = cnss_bus_dev_powerup(plat_priv);
	if (ret)
		clear_bit(CNSS_DRIVER_IDLE_RESTART, &plat_priv->driver_state);

	return ret;
}

static int cnss_power_down_hdlr(struct cnss_plat_data *plat_priv)
{
	int ret;

	ret = cnss_bus_dev_shutdown(plat_priv);
	clear_bit(CNSS_DRIVER_IDLE_SHUTDOWN, &plat_priv->driver_state);

	return ret;
}

void cnss2_sdio_update_status(struct cnss_sdio_data *sdio_data)
{
	if (sdio_data && sdio_data->ops &&
			sdio_data->ops->update_status &&
			sdio_data->al_client_handle &&
			sdio_data->al_client_handle->func) {
		/* Clearing pending SDIO transaction */
		qcn_sdio_purge_rw_buff();

		sdio_data->ops->update_status(
			sdio_data->al_client_handle->func,
			CNSS_FW_DOWN);
	}
}

int cnss2_sdio_driver_shutdown(void)
{
	struct cnss_plat_data *plat_data = cnss_bus_dev_to_plat_priv(NULL);
	struct cnss_sdio_data *sdio_data = plat_data->bus_priv;
	int ret = 0;

	if (test_bit(CNSS_IN_REBOOT, &plat_data->driver_state)) {
		cnss_pr_info("Reboot is in progress, skip driver shutdown");
		return -EINVAL;
	}

	set_bit(CNSS_DRIVER_SHUTDOWN, &plat_data->driver_state);

	cnss2_sdio_update_status(sdio_data);

	ret = cnss_sdio_call_driver_remove(plat_data->bus_priv);
	if (ret) {
		cnss_pr_err("driver remove failed %d\n", ret);
		return ret;
	}

	return ret;
}

void cnss2_sdio_start_dump_collection(void)
{
	/*card release and claim is required to read the
	SW mode state in meta info before dump collection
	that intitiates the qti_client for dump collection.
	*/
	qcn_sdio_card_release();

	/* This delay is added to make sure before host tries to reset
	 * SDIOC registers, SDIO client HW is reset by SMC after SSR.
	 */
	msleep(50);
	qcn_sdio_card_claim();
}

int cnss2_sdio_wlan_shutdown(void)
{
	struct cnss_plat_data *plat_data = cnss_bus_dev_to_plat_priv(NULL);
	int ret = 0;

	if (test_bit(CNSS_IN_REBOOT, &plat_data->driver_state)) {
		cnss_pr_info("Reboot is in progress, skip wlan shutdown");
		return -EINVAL;
	}

	if (test_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state)) {
		cnss_pr_info("SSR dump in progress, ignore wlan shutdown");
		return -EINVAL;
	}

	if (plat_data->recovery_enabled) {
		clear_bit(CNSS_DRIVER_SHUTDOWN, &plat_data->driver_state);
		set_bit(CNSS_DEVICE_RESET, &plat_data->driver_state);
		qcn_sdio_card_release();
		ret = cnss_sdio_wlan_disable_subsys();
		if (ret)
			cnss_pr_err("wlan shutdown failed %d\n", ret);
		qcn_sw_mode_reset();
	} else
		panic("subsys-restart: Resetting the SoC wlan crashed\n");

	return ret;
}

static int cnss2_sdio_firmware_down(void)
{
	int ret = 0;

	ret = cnss2_sdio_driver_shutdown();
	if (ret)
		cnss_pr_err("driver shutdown failed ret %d", ret);
	return ret;
}

static void cnss_driver_event_work(struct work_struct *work)
{
	struct cnss_plat_data *plat_priv =
		container_of(work, struct cnss_plat_data, event_work);
	struct cnss_driver_event *event;
	unsigned long flags;
	int ret = 0;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL!\n");
		return;
	}

	cnss_pm_stay_awake(plat_priv);

	spin_lock_irqsave(&plat_priv->event_lock, flags);

	while (!list_empty(&plat_priv->event_list)) {
		event = list_first_entry(&plat_priv->event_list,
					 struct cnss_driver_event, list);
		list_del(&event->list);
		spin_unlock_irqrestore(&plat_priv->event_lock, flags);

		cnss_pr_dbg("Processing driver event: %s%s(%d), state: 0x%lx\n",
			    cnss_driver_event_to_str(event->type),
			    event->sync ? "-sync" : "", event->type,
			    plat_priv->driver_state);

		switch (event->type) {
		case CNSS_DRIVER_EVENT_REGISTER_DRIVER:
			ret = cnss_bus_register_driver_hdlr(plat_priv,
							    event->data);
			break;
		case CNSS_DRIVER_EVENT_UNREGISTER_DRIVER:
			ret = cnss_bus_unregister_driver_hdlr(plat_priv);
			break;
		case CNSS_DRIVER_EVENT_DEVICE_UP:
			ret = qcn_sdio_card_claim();
			if (ret) {
				cnss_pr_err("Failed to claim SDIO card: %d\n", ret);
				break;
			}
			ret = cnss_device_up(plat_priv);
			break;
		case CNSS_DRIVER_EVENT_IDLE_SHUTDOWN:
			set_bit(CNSS_DRIVER_IDLE_SHUTDOWN,
				&plat_priv->driver_state);
			ret = cnss_power_down_hdlr(plat_priv);
			break;
		case CNSS_DRIVER_EVENT_IDLE_RESTART:
			set_bit(CNSS_DRIVER_IDLE_RESTART,
				&plat_priv->driver_state);
			ret = cnss_power_up_hdlr(plat_priv);
			break;
		case CNSS_DRIVER_EVENT_RECOVERY:
			set_bit(CNSS_DRIVER_RECOVERING,
				&plat_priv->driver_state);
			ret = cnss2_sdio_schedule_recovery();
			break;
		case CNSS_DRIVER_EVENT_FW_DOWN:
			ret = cnss2_sdio_firmware_down();
			break;
		default:
			cnss_pr_err("Invalid driver event type: %d",
				    event->type);
			kfree(event);
			spin_lock_irqsave(&plat_priv->event_lock, flags);
			continue;
		}

		spin_lock_irqsave(&plat_priv->event_lock, flags);
		if (event->sync) {
			event->ret = ret;
			complete(&event->complete);
			continue;
		}
		spin_unlock_irqrestore(&plat_priv->event_lock, flags);

		kfree(event);

		spin_lock_irqsave(&plat_priv->event_lock, flags);
	}
	spin_unlock_irqrestore(&plat_priv->event_lock, flags);

	cnss_pm_relax(plat_priv);
}

static int cnss_reboot_notifier(struct notifier_block *nb,
				unsigned long action,
				void *data)
{
	struct cnss_plat_data *plat_priv =
		container_of(nb, struct cnss_plat_data, reboot_nb);
	struct cnss_sdio_data *sdio_data = NULL;

	cnss_pr_err("CNSS: reboot notifier fired, action=%lu\n", action);

	/* Ensure only one path executes (sysfs or notifier) */
	if (test_and_set_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
		cnss_pr_err("CNSS: shutdown already in progress, skipping notifier\n");
		return NOTIFY_OK;
	}

	if (plat_priv->bus_priv) {
		sdio_data = plat_priv->bus_priv;
		qcn_sdio_card_release();
		cnss2_sdio_update_status(sdio_data);
	}

	complete_all(&plat_priv->power_up_complete);
	cnss_sdio_wlan_disable_subsys();
	cnss_pr_err("CNSS: reboot notifier completed\n");

	return NOTIFY_OK;
}

static int cnss_event_work_init(struct cnss_plat_data *plat_priv)
{
	spin_lock_init(&plat_priv->event_lock);
	plat_priv->event_wq = alloc_workqueue("cnss_driver_event",
					      WQ_UNBOUND, 1);
	if (!plat_priv->event_wq) {
		cnss_pr_err("Failed to create event workqueue!\n");
		return -EFAULT;
	}

	INIT_WORK(&plat_priv->event_work, cnss_driver_event_work);
	INIT_LIST_HEAD(&plat_priv->event_list);

	return 0;
}

static void cnss_event_work_deinit(struct cnss_plat_data *plat_priv)
{
	destroy_workqueue(plat_priv->event_wq);
}

static inline u32
cnss_dt_type(struct cnss_plat_data *plat_priv)
{
	bool is_converged_dt = of_property_read_bool(
		plat_priv->plat_dev->dev.of_node, "qcom,converged-dt");
	bool is_multi_wlan_xchg;

	if (is_converged_dt)
		return CNSS_DTT_CONVERGED;

	is_multi_wlan_xchg = of_property_read_bool(
		plat_priv->plat_dev->dev.of_node, "qcom,multi-wlan-exchg");

	if (is_multi_wlan_xchg)
		return CNSS_DTT_MULTIEXCHG;
	return CNSS_DTT_LEGACY;
}

static int cnss_wlan_device_init(struct cnss_plat_data *plat_priv)
{
	int ret = 0;

	ret = cnss_bus_init(plat_priv);
	return ret;
}

int cnss2_sdio_schedule_recovery(void)
{
	struct cnss_plat_data *plat_data = cnss_bus_dev_to_plat_priv(NULL);
	int ret = 0;

	if (test_bit(CNSS_DEVICE_RESET, &plat_data->driver_state) ||
	    test_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state) ||
	    test_bit(CNSS_IN_REBOOT, &plat_data->driver_state)) {
		cnss_pr_info("SSR/Reboot in progress, ignore recovery 0x%x",
			     plat_data->driver_state);
		clear_bit(CNSS_DRIVER_RECOVERING, &plat_data->driver_state);
		return ret;
	}

	ret = cnss2_sdio_driver_shutdown();
	if (ret)
		cnss_pr_err("driver shutdown failed ret %d", ret);

	ret = cnss2_sdio_wlan_shutdown();
	if (ret) {
		cnss_pr_err("wlan device shutdown failed ret %d", ret);
		clear_bit(CNSS_DRIVER_RECOVERING, &plat_data->driver_state);
	}

	return ret;
}

void cnss_schedule_recovery(struct device *dev,
				   enum cnss_recovery_reason reason)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);

	if (test_bit(CNSS_DRIVER_RECOVERING, &plat_priv->driver_state)) {
		cnss_pr_info("Recovery already in progress state 0x%x",
			     plat_priv->driver_state);
		return;
	}
	cnss_driver_event_post(plat_priv,
				CNSS_DRIVER_EVENT_RECOVERY,
				0, NULL);
}

EXPORT_SYMBOL(cnss_schedule_recovery);

u64 cnss_get_tsf_irq_ts(struct device *dev)
{
	u32 tsf_lo = 0, tsf_hi = 0;
	u64 tsf_data = 0;
	void __iomem *tsf_base;
	struct cnss_plat_data *plat_data;

	plat_data = cnss_bus_dev_to_plat_priv(NULL);
	if (!plat_data) {
		cnss_pr_err("plat_priv is NULL!\n");
		return 0;
	}

	tsf_base = plat_data->ts_reg_addr;
	if (!tsf_base) {
		cnss_pr_err("NULL register address\n");
		return 0;
	}

	tsf_data = readl_relaxed(tsf_base + TSF_IRQ_TS_OP_OFFSET);
	if (!(tsf_data & TSF_IRQ_TS_OP_VALID)) {
		cnss_pr_err("Invalid TSF IRQ timestamp\n");
		return 0;
	}

	if (tsf_data & TSF_IRQ_TS_OP_OVERFLOW)
		cnss_pr_dbg("tsf irq timestamp overflow detected\n");
	// Read lower 32 bits
	tsf_lo = readl_relaxed(tsf_base + TSF_IRQ_TS_LO_OFFSET);
	// Read upper 24 bits
	tsf_hi = readl_relaxed(tsf_base + TSF_IRQ_TS_HI_OFFSET) &
		 TSF_IRQ_TS_HI_MASK;
	// Combine to form 64-bit TSF value
	tsf_data = ((u64)tsf_hi << 32) | tsf_lo;
	cnss_pr_dbg("tsf_lo=%u tsf_hi=%u tsf_data=%llu",
		    tsf_lo, tsf_hi, tsf_data);

	return tsf_data;
}
EXPORT_SYMBOL(cnss_get_tsf_irq_ts);

static void cnss_get_tsf_ts_info(struct cnss_plat_data *plat_priv)
{
	struct platform_device *plat_dev;
	struct device *dev;
	struct resource *res;
	resource_size_t addr_len;
	void __iomem *base_addr;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL!\n");
		return;
	}

	plat_dev = plat_priv->plat_dev;
	if (!plat_dev) {
		cnss_pr_err("plat_dev is NULL!\n");
		return;
	}

	res = platform_get_resource_byname(plat_dev, IORESOURCE_MEM,
					   TSF_IRQ_TS);
	if (!res) {
		cnss_pr_dbg("TSF_IRQ_TS address is not present\n");
		return;
	}

	dev = &plat_dev->dev;
	addr_len = resource_size(res);
	base_addr = devm_ioremap(dev, res->start, addr_len);
	if (!base_addr) {
		cnss_pr_err("Failed to get ts base address\n");
		return;
	}
	plat_priv->ts_reg_addr = base_addr;

	cnss_pr_dbg("TSF_IRQ_TS mapped successfully, length %pa\n", &addr_len);
}

#if IS_ENABLED(CONFIG_WCNSS_MEM_PRE_ALLOC)
static void cnss_initialize_mem_pool(unsigned long device_id)
{
	cnss_initialize_prealloc_pool(device_id);
}
static void cnss_deinitialize_mem_pool(void)
{
	cnss_deinitialize_prealloc_pool();
}
#else
static void cnss_initialize_mem_pool(unsigned long device_id)
{
}
static void cnss_deinitialize_mem_pool(void)
{
}
#endif

#ifdef CONFIG_SDIO_QCN
static int cnss_smc_notifier_nb(struct notifier_block *nb,
				unsigned long code,
				void *data)
{
	struct cnss_plat_data *plat_data;
	struct cnss_sdio_data *sdio_data;
	plat_data = cnss_bus_dev_to_plat_priv(NULL);
	sdio_data = plat_data->bus_priv;
	char *path;
	path = MESON_CORE_PATH;
	int ret = 0;

	cnss_pr_info("SMC notifier: event %s\n", qcc_subsys_event_to_str(code));
	switch (code) {

	case POWER_UP_SUCCESS:
		complete(&plat_data->power_up_complete);
		if (test_bit(CNSS_DRIVER_REINIT, &plat_data->driver_state)) {
			cnss_driver_event_post(plat_data,
					       CNSS_DRIVER_EVENT_DEVICE_UP,
					       0, NULL);
			if (plat_data->smc_crash_state == 1) {
				plat_data->smc_crash_state = 0;
			}
		}
		atomic_set(&plat_data->power_up_retry_cnt,
			   CNSS_SMC_EVENT_RETRY_COUNT);
		break;

	case POWER_UP_FAIL:
		if (test_bit(CNSS_DRIVER_SMC_SSR_IN_PROGRESS, &plat_data->driver_state)) {
			complete(&plat_data->power_up_complete);
			cnss_pr_info("SMC SSR is in progress, ignore power up fail\n");
		} else if (test_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state) ||
			test_bit(CNSS_IN_REBOOT, &plat_data->driver_state)) {
			complete(&plat_data->power_up_complete);
			cnss_pr_info("SSR/Reboot is in progress, ignore power up fail\n");
		} else if (atomic_read(&plat_data->power_up_retry_cnt)) {
			atomic_dec(&plat_data->power_up_retry_cnt);
			cnss_sdio_wlan_enable_subsys();
		} else {
			cnss_pr_err("Timed out turning on Meson, reset SMC\n");
			complete(&plat_data->power_up_complete);
			smc_recovery_api_for_client();
		}
		break;

	case SSR_DUMP_BEGIN:
		cnss_pm_stay_awake(plat_data);
		complete(&plat_data->fw_assert_complete);
		if (test_bit(CNSS_IN_REBOOT, &plat_data->driver_state)) {
			cnss_pr_err("Reboot in progress, ignore SSR 0x%x",
				    plat_data->driver_state);
			return ret;
		}
		if (!test_bit(CNSS_DRIVER_FW_SSR_IN_PROGRESS, &plat_data->driver_state) &&
		   !test_bit(CNSS_DRIVER_SMC_SSR_IN_PROGRESS, &plat_data->driver_state)) {
			cnss2_sdio_update_status(sdio_data);
			cnss_driver_event_post(plat_data,
					CNSS_DRIVER_EVENT_FW_DOWN,
					0, NULL);
		}
		set_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state);
		set_bit(CNSS_DRIVER_FW_SSR_IN_PROGRESS, &plat_data->driver_state);
		cnss2_sdio_start_dump_collection();
		break;

	case SSR_DUMP_COMPLETED:
		cnss_sdio_send_hang_event(sdio_data, path);
		fallthrough;
	case SSR_DUMP_FAILED:
		if (test_bit(CNSS_IN_REBOOT, &plat_data->driver_state)) {
			cnss_pr_err("Reboot in progress, ignore SSR 0x%x",
				    plat_data->driver_state);
			return ret;
		}
		clear_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state);
		cnss2_sdio_wlan_shutdown();
		cnss_pm_relax(plat_data);
		break;

	case POWER_DOWN_SUCCESS:
		complete(&plat_data->power_down_complete);
		if (test_bit(CNSS_DEVICE_RESET, &plat_data->driver_state)) {
			cnss_pr_info("set CNSS_DRIVER_REINIT\n");
			clear_bit(CNSS_DEVICE_RESET, &plat_data->driver_state);
			set_bit(CNSS_DRIVER_REINIT, &plat_data->driver_state);
			ret = cnss_sdio_wlan_enable_subsys();
		}
		atomic_set(&plat_data->power_down_retry_cnt,
			   CNSS_SMC_EVENT_RETRY_COUNT);
		break;

	case POWER_DOWN_FAIL:
		if (test_bit(CNSS_DRIVER_SMC_SSR_IN_PROGRESS, &plat_data->driver_state)) {
			complete(&plat_data->power_down_complete);
			clear_bit(CNSS_DEVICE_RESET, &plat_data->driver_state);
			cnss_pr_info("SMC SSR is in progress, ignore power down fail\n");
		} else if (test_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state) ||
			test_bit(CNSS_IN_REBOOT, &plat_data->driver_state)) {
			complete(&plat_data->power_down_complete);
			cnss_pr_info("SSR/Reboot is in progress, ignore power down fail\n");
		} else if (atomic_read(&plat_data->power_down_retry_cnt)) {
			atomic_dec(&plat_data->power_down_retry_cnt);
			cnss_sdio_wlan_disable_subsys();
		} else {
			cnss_pr_err("Timed out turning off Meson, reset SMC\n");
			complete(&plat_data->power_down_complete);
			smc_recovery_api_for_client();
		}
		break;

	default:
		cnss_pr_err("SMC notifier: event %s not handled\n", qcc_subsys_event_to_str(code));
		break;
	}
	return NOTIFY_OK;
}

static void cnss_probe_after_smc_fn(struct work_struct *work)
{
	int ret = 0;

	device_release_driver(cnss_dev);
	ret = device_attach(cnss_dev);
	if (ret)
		cnss_pr_err("cnss driver attach failed %d", ret);
	else
		cnss_pr_info("cnss driver probed after smc ready");
}

static int cnss_smc_them_notifier_nb(struct notifier_block *nb,
				     unsigned long code,
				     void *data)
{
	struct cnss_plat_data *plat_data = NULL;
	struct cnss_sdio_data *sdio_data;
	int ret = 0;
	enum smc_shutdown_reason reason = 0;

	if (data != NULL)
		reason = *(enum smc_shutdown_reason *)data;
	cnss_pr_err("SMC notifier: Themisto event %lu\n", code);
	if (smc_them_ready) {
		plat_data = cnss_bus_dev_to_plat_priv(NULL);
		if (plat_data == NULL) {
			cnss_pr_err("plat_data is NULL\n");
			return 0;
		}
	}

	/** Do not add any blocking call in SMC notification */
	switch (code) {
	case QCOM_SMC_BEFORE_POWERUP:
		if (smc_them_ready) {
			if (test_bit(CNSS_DRIVER_SHUTDOWN, &plat_data->driver_state)) {
				cnss_pr_err("set CNSS_DRIVER_REINIT");
				clear_bit(CNSS_DRIVER_SHUTDOWN, &plat_data->driver_state);
				set_bit(CNSS_DRIVER_REINIT, &plat_data->driver_state);
			}
		}
		break;

	case QCOM_SMC_AFTER_POWERUP:
		if (!smc_them_ready) {
			smc_them_ready = 1;
			schedule_work(&cnss_prb_after_smc_work);
			return 0;
		}
		if (test_bit(CNSS_DRIVER_REINIT, &plat_data->driver_state)) {
			ret = cnss_sdio_wlan_enable_subsys();
		}
		break;

	case QCOM_SMC_RAM_DUMP:
		cnss_pr_err(" QCOM_SMC_RAM_DUMP event received\n");
		if (plat_data)
			plat_data->smc_crash_state = 1;
		break;

	case QCOM_SMC_BEFORE_SHUTDOWN:
		if (test_bit(CNSS_POWER_OFF, &plat_data->driver_state)) {
			cnss_pr_info("Ignore SMC shutdown, driver state 0x%x",
				      plat_data->driver_state);
			return 0;
		}
		if (reason == SMC_SHUTDOWN_CRASH) {
			sdio_data = plat_data->bus_priv;
			if (!test_bit(CNSS_DRIVER_FW_SSR_IN_PROGRESS, &plat_data->driver_state) &&
				!test_bit(CNSS_DRIVER_SMC_SSR_IN_PROGRESS, &plat_data->driver_state)) {
				cnss2_sdio_update_status(sdio_data);
				cnss_driver_event_post(plat_data,
					CNSS_DRIVER_EVENT_FW_DOWN,
					0, NULL);
			}
			set_bit(CNSS_DRIVER_SHUTDOWN, &plat_data->driver_state);
			set_bit(CNSS_DRIVER_SMC_SSR_IN_PROGRESS, &plat_data->driver_state);
			clear_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state);
			clear_bit(CNSS_DEVICE_RESET, &plat_data->driver_state);
			qcn_sdio_card_release();
			qcn_sw_mode_reset();
		} else if (reason == SMC_SHUTDOWN_GRACEFUL) {
			cnss_idle_shutdown(&plat_data->plat_dev->dev);
		}
		break;

	case QCOM_SMC_AFTER_SHUTDOWN:
		break;

	default:
		cnss_pr_err("SMC notifier: event %lu not handled\n", code);
		break;
	}
	return NOTIFY_OK;
}

static void cnss_them_smc_register(void)
{
	int ret = 0;

	smc_them_nb.notifier_call = cnss_smc_them_notifier_nb;

	/* Register for SMC themisto events */
	smc_them_notif = smc_register_for_smc_event(SMC_STATE_EVENT,
						    &smc_them_nb);
	if (IS_ERR_OR_NULL(smc_them_notif)) {
		ret = PTR_ERR(smc_them_notif);
		cnss_pr_err("Failed to register smc notifier, err = %d\n", ret);
	}
}

static void cnss_them_smc_unregister(void)
{
	if (smc_them_notif) {
		smc_unregister_for_smc_event(smc_them_notif, &smc_them_nb);
		smc_them_nb.notifier_call = NULL;
		smc_them_notif = NULL;
	}
}

int cnss_sdio_wlan_time_sync(void)
{
	return smc_time_sync();
}
EXPORT_SYMBOL(cnss_sdio_wlan_time_sync);

static int cnss_smc_tsf_notifier_cb(struct notifier_block *nb,
				    unsigned long event,
				    void *data)
{
	struct cnss_plat_data *plat_priv;
	struct cnss_sdio_data *sdio_info;

	plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	sdio_info = plat_priv->bus_priv;
	if ((!sdio_info) || (!sdio_info->ops) ||
	    (!sdio_info->al_client_handle)) {
		cnss_pr_err("cnss_info is NULL\n");
		return -ENODEV;
	}

	cnss_pr_dbg("SMC TSF notifier: event %lu\n", event);
	switch (event) {
	case TIME_SYNC_COMPLETE:
		cnss_pr_dbg("Time sync successful");
		break;
	case TIME_SYNC_FAIL:
		cnss_pr_dbg("Time sync failed");
		break;
	default:
		cnss_pr_dbg("%s, received invalid time sync event\n", __func__);
		break;
	}

	complete(&plat_priv->time_sync_notif);

	return NOTIFY_OK;
}

static int cnss_register_smc(struct cnss_plat_data *plat_priv)
{
	struct smc_notif_info *smc_notif;
	int ret = 0;

	plat_priv->smc_nb.notifier_call = cnss_smc_notifier_nb;

	/* Register for Meson WLAN events to SMC */
	smc_notif = smc_register_for_smc_event(QCC_WLAN_EVENT, &plat_priv->smc_nb);
	if (IS_ERR_OR_NULL(smc_notif)) {
		ret = PTR_ERR(smc_notif);
		cnss_pr_err("Failed to register smc notifier, err = %d\n", ret);
	}

	plat_priv->smc_notif = smc_notif;

	/* Register for Time sync event callback from SMC */
	plat_priv->smc_tsf_nb.notifier_call = cnss_smc_tsf_notifier_cb;

	smc_notif = smc_register_for_smc_event(SMC_TIME_SYNC_EVENT,
					       &plat_priv->smc_tsf_nb);
	if (IS_ERR_OR_NULL(smc_notif)) {
		ret = PTR_ERR(smc_notif);
		cnss_pr_err("Failed to register smc TSF notifier, err = %d\n", ret);
	}

	plat_priv->smc_tsf_notif = smc_notif;

	//TODO: Add one state bit if required

	return ret;
}

static int cnss_unregister_smc(struct cnss_plat_data *plat_priv)
{
	if (plat_priv->smc_notif) {
		//cnss_sdio_wlan_disable_subsys();
		smc_unregister_for_smc_event(plat_priv->smc_notif, &plat_priv->smc_nb);
		plat_priv->smc_nb.notifier_call = NULL;
		plat_priv->smc_notif = NULL;
	}

	if (plat_priv->smc_tsf_notif) {
		smc_unregister_for_smc_event(plat_priv->smc_tsf_notif, &plat_priv->smc_tsf_nb);
		plat_priv->smc_tsf_nb.notifier_call = NULL;
		plat_priv->smc_tsf_notif = NULL;
	}

	return 0;
}

int cnss_sdio_wlan_enable_subsys(void)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);

	if (test_bit(CNSS_DRIVER_PROBING, &plat_priv->driver_state)) {
		cnss_pr_err("Wifi Enable already in progress");
		return 0;
	}

	cnss_pr_info("Enabling WiFi, state 0x%x", plat_priv->driver_state);
	return smc_enable_subsys(SUBSYS_WLAN);
}
EXPORT_SYMBOL(cnss_sdio_wlan_enable_subsys);

int cnss_sdio_wlan_disable_subsys(void)
{
	struct cnss_plat_data *plat_data;
	plat_data = cnss_bus_dev_to_plat_priv(NULL);

	if (!test_bit(CNSS_DRIVER_SHUTDOWN, &plat_data->driver_state)) {
		return smc_disable_subsys(SUBSYS_WLAN);
	}

	cnss_pr_info("Disabling WiFi, state 0x%x", plat_data->driver_state);

	if (test_bit(CNSS_SSR_DUMP_IN_PROGRESS, &plat_data->driver_state)) {
		cnss_pr_info("SSR dump in progress, ignore disable WiFi");
		return 0;
	}

	return smc_disable_subsys(SUBSYS_WLAN);
}

EXPORT_SYMBOL(cnss_sdio_wlan_disable_subsys);

int cnss_idle_shutdown(struct device *dev)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	cnss_pr_info("Doing idle shutdown\n");

	if (test_bit(CNSS_DRIVER_SHUTDOWN, &plat_priv->driver_state)) {
		cnss_pr_info("SSR is in progress, ignore idle shutdown state 0x%x",
			     plat_priv->driver_state);
		return -EBUSY;
	}

	return cnss_driver_event_post(plat_priv,
				      CNSS_DRIVER_EVENT_IDLE_SHUTDOWN,
				      CNSS_EVENT_SYNC_UNINTERRUPTIBLE, NULL);
}
EXPORT_SYMBOL(cnss_idle_shutdown);

int cnss_idle_restart(struct device *dev)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	cnss_pr_info("Doing idle restart\n");

	if (test_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
		cnss_pr_dbg("Reboot or shutdown is in progress, ignore idle restart\n");
		return -EINVAL;
	}

	return cnss_driver_event_post(plat_priv,
				      CNSS_DRIVER_EVENT_IDLE_RESTART,
				      CNSS_EVENT_SYNC_UNINTERRUPTIBLE, NULL);
}
EXPORT_SYMBOL(cnss_idle_restart);

#else
static inline int cnss_register_smc(struct cnss_plat_data *plat_priv)
{
	return 0;
}

static inline int cnss_unregister_smc(struct cnss_plat_data *plat_priv)
{
	return 0;
}

int cnss_sdio_wlan_enable_subsys(void)
{
	return 0;
}

int cnss_sdio_wlan_time_sync(void)
{
	return 0;
}
#endif

static int cnss2_sdio_tcdev_get_max_state(struct thermal_cooling_device *tcdev,
				    unsigned long *thermal_state)
{
	struct cnss_thermal_cdev *cnss_tcdev = NULL;

	if (!tcdev || !tcdev->devdata) {
		cnss_pr_err("tcdev or tcdev->devdata is null!\n");
		return -EINVAL;
	}

	cnss_tcdev = tcdev->devdata;
	*thermal_state = cnss_tcdev->max_thermal_state;

	return 0;
}

static int cnss2_sdio_tcdev_get_cur_state(struct thermal_cooling_device *tcdev,
				    unsigned long *thermal_state)
{
	struct cnss_thermal_cdev *cnss_tcdev = NULL;

	if (!tcdev || !tcdev->devdata) {
		cnss_pr_err("tcdev or tcdev->devdata is null!\n");
		return -EINVAL;
	}

	cnss_tcdev = tcdev->devdata;
	*thermal_state = cnss_tcdev->curr_thermal_state;

	return 0;
}

static int cnss2_sdio_tcdev_set_cur_state(struct thermal_cooling_device *tcdev,
				    unsigned long thermal_state)
{
	struct cnss_thermal_cdev *cnss_tcdev = NULL;
	struct cnss_plat_data *plat_priv =  cnss_get_plat_priv(NULL);
	int ret = 0;

	if (!tcdev || !tcdev->devdata) {
		cnss_pr_err("tcdev or tcdev->devdata is null!\n");
		return -EINVAL;
	}

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL!\n");
		return -ENODEV;
	}

	cnss_tcdev = tcdev->devdata;

	if (thermal_state > cnss_tcdev->max_thermal_state)
		return -EINVAL;

	cnss_pr_vdbg("Cooling device set current state: %ld,for cdev id %d",
		     thermal_state, cnss_tcdev->tcdev_id);

	mutex_lock(&plat_priv->tcdev_lock);
	ret = cnss_bus_set_therm_cdev_state(plat_priv,
					    thermal_state,
					    cnss_tcdev->tcdev_id);
	if (!ret)
		cnss_tcdev->curr_thermal_state = thermal_state;
	mutex_unlock(&plat_priv->tcdev_lock);
	if (ret) {
		cnss_pr_err("Setting Current Thermal State Failed: %d,for cdev id %d",
			    ret, cnss_tcdev->tcdev_id);
		return ret;
	}

	return 0;
}

static struct thermal_cooling_device_ops cnss2_sdio_cooling_ops = {
	.get_max_state = cnss2_sdio_tcdev_get_max_state,
	.get_cur_state = cnss2_sdio_tcdev_get_cur_state,
	.set_cur_state = cnss2_sdio_tcdev_set_cur_state,
};

int cnss2_sdio_thermal_cdev_register(struct device *dev, unsigned long max_state,
			       int tcdev_id)
{
	struct cnss_plat_data *priv = cnss_get_plat_priv(NULL);
	struct cnss_thermal_cdev *cnss_tcdev = NULL;
	char cdev_node_name[THERMAL_NAME_LENGTH] = "";
	struct device_node *dev_node;
	int ret = 0;

	if (!priv) {
		cnss_pr_err("Platform driver is not initialized!\n");
		return -ENODEV;
	}

	cnss_tcdev = kzalloc(sizeof(*cnss_tcdev), GFP_KERNEL);
	if (!cnss_tcdev) {
		cnss_pr_err("Failed to allocate cnss_tcdev object!\n");
		return -ENOMEM;
	}

	cnss_tcdev->tcdev_id = tcdev_id;
	cnss_tcdev->max_thermal_state = max_state;

	snprintf(cdev_node_name, THERMAL_NAME_LENGTH,
		 "qcom,cnss_cdev%d", tcdev_id);

	dev_node = of_find_node_by_name(NULL, cdev_node_name);
	if (!dev_node) {
		cnss_pr_err("Failed to get cooling device node\n");
		ret = -EINVAL;
		goto error;
	}

	cnss_pr_dbg("tcdev node->name=%s\n", dev_node->name);

	if (of_find_property(dev_node, "#cooling-cells", NULL)) {
		cnss_tcdev->tcdev = thermal_of_cooling_device_register(dev_node,
					cdev_node_name, cnss_tcdev, &cnss2_sdio_cooling_ops);
		of_node_put(dev_node);
		if (IS_ERR_OR_NULL(cnss_tcdev->tcdev)) {
			ret = PTR_ERR(cnss_tcdev->tcdev);
			cnss_pr_err("Cooling device register failed: %d, for cdev id %d\n",
				    ret, cnss_tcdev->tcdev_id);
			goto error;
		} else {
			cnss_pr_dbg("Cooling device registered for cdev id %d",
				    cnss_tcdev->tcdev_id);
			mutex_lock(&priv->tcdev_lock);
			list_add(&cnss_tcdev->tcdev_list,
				 &priv->cnss_tcdev_list);
			mutex_unlock(&priv->tcdev_lock);
		}
	} else {
		cnss_pr_dbg("Cooling device registration not supported");
		ret = -EOPNOTSUPP;
		of_node_put(dev_node);
		goto error;
	}
	return ret;

error:
	kfree(cnss_tcdev);
	cnss_tcdev = NULL;
	return ret;
}
EXPORT_SYMBOL(cnss2_sdio_thermal_cdev_register);

void cnss2_sdio_thermal_cdev_unregister(struct device *dev, int tcdev_id)
{
	struct cnss_plat_data *priv = cnss_get_plat_priv(NULL);
	struct cnss_thermal_cdev *cnss_tcdev = NULL;

	if (!priv) {
		cnss_pr_err("Platform driver is not initialized!\n");
		return;
	}

	mutex_lock(&priv->tcdev_lock);
	while (!list_empty(&priv->cnss_tcdev_list)) {
		cnss_tcdev = list_first_entry(&priv->cnss_tcdev_list,
					      struct cnss_thermal_cdev,
					      tcdev_list);
		thermal_cooling_device_unregister(cnss_tcdev->tcdev);
		list_del(&cnss_tcdev->tcdev_list);
		kfree(cnss_tcdev);
	}
	mutex_unlock(&priv->tcdev_lock);
}
EXPORT_SYMBOL(cnss2_sdio_thermal_cdev_unregister);

int cnss2_sdio_get_curr_therm_cdev_state(struct device *dev,
				unsigned long *thermal_state, int tcdev_id)
{
	struct cnss_plat_data *priv = cnss_get_plat_priv(NULL);
	struct cnss_thermal_cdev *cnss_tcdev = NULL;

	if (!priv) {
		cnss_pr_err("Platform driver is not initialized!\n");
		return -ENODEV;
	}

	mutex_lock(&priv->tcdev_lock);
	list_for_each_entry(cnss_tcdev, &priv->cnss_tcdev_list, tcdev_list) {
		if (cnss_tcdev->tcdev_id != tcdev_id)
			continue;

		*thermal_state = cnss_tcdev->curr_thermal_state;
		mutex_unlock(&priv->tcdev_lock);
		cnss_pr_dbg("Cooling device current state: %ld, for cdev id %d",
			    cnss_tcdev->curr_thermal_state, tcdev_id);
		return 0;
	}
	mutex_unlock(&priv->tcdev_lock);
	cnss_pr_dbg("Cooling device ID not found: %d", tcdev_id);
	return -EINVAL;
}
EXPORT_SYMBOL(cnss2_sdio_get_curr_therm_cdev_state);

static void cnss_init_time_sync_period_default(struct cnss_plat_data *plat_priv)
{
	plat_priv->ctrl_params.time_sync_period = CNSS_TIME_SYNC_PERIOD_DEFAULT;
	plat_priv->ctrl_params.time_sync_period_vote[TIME_SYNC_VOTE_WLAN] =
						CNSS_TIME_SYNC_PERIOD_INVALID;
	plat_priv->ctrl_params.time_sync_period_vote[TIME_SYNC_VOTE_CNSS] =
						CNSS_TIME_SYNC_PERIOD_INVALID;
}

/**
 * cnss_wlan_gpio_handle_response() - Handle response from ADSP
 * @gpio_data: WLAN GPIO data
 * @data: Response data
 * @len: Response length
 */
static void
cnss_wlan_gpio_handle_response(struct cnss_wlan_gpio_data *gpio_data,
			       u8 *data, size_t len)
{
	struct wlan_gpio_response *resp;

	if (len < sizeof(*resp)) {
		cnss_pr_err("Invalid response length: %zu\n", len);
		return;
	}

	resp = (struct wlan_gpio_response *)data;

	cnss_pr_info("WLAN GPIO response: rsp_type=%d, status=%d\n",
		     resp->rsp_type, resp->status);

	mutex_lock(&gpio_data->reg_lock);

	switch (resp->rsp_type) {
	case WLAN_GPIO_RSP_REGISTER:
		if (resp->status == WLAN_GPIO_STATUS_SUCCESS)
			gpio_data->gpio_registered = true;

		complete(&gpio_data->gpio_reg_complete);
		break;

	case WLAN_GPIO_RSP_DEREGISTER:
		if (resp->status == WLAN_GPIO_STATUS_SUCCESS)
			gpio_data->gpio_registered = false;

		complete(&gpio_data->gpio_dereg_complete);
		break;

	default:
		cnss_pr_err("Unknown response type: %d\n", resp->rsp_type);
		break;
	}

	mutex_unlock(&gpio_data->reg_lock);
}

/**
 * cnss_wlan_gpio_rx_work() - RX work handler
 * @work: Work structure
 *
 * Processes received messages from ADSP.
 */
static void cnss_wlan_gpio_rx_work(struct work_struct *work)
{
	struct cnss_wlan_gpio_data *gpio_data =
		container_of(work, struct cnss_wlan_gpio_data, rx_work);
	struct cnss_wlan_gpio_buf *buf, *tmp;
	unsigned long flags;
	LIST_HEAD(local_list);
	u8 rsp_type;

	/* Move all pending buffers to local list under lock */
	spin_lock_irqsave(&gpio_data->rx_lock, flags);
	list_splice_init(&gpio_data->rx_list, &local_list);
	spin_unlock_irqrestore(&gpio_data->rx_lock, flags);

	/* Process buffers without holding the lock */
	list_for_each_entry_safe(buf, tmp, &local_list, node) {
		rsp_type = buf->buf[0];

		cnss_pr_dbg("Processing WLAN GPIO response rsp_type=%d len=%zu\n",
			    rsp_type, buf->len);

		switch (rsp_type) {
		case WLAN_GPIO_RSP_REGISTER:
		case WLAN_GPIO_RSP_DEREGISTER:
			cnss_wlan_gpio_handle_response(gpio_data, buf->buf,
						       buf->len);
			break;

		default:
			cnss_pr_err("Unknown WLAN GPIO response type: %d\n",
				    rsp_type);
			break;
		}

		list_del(&buf->node);
		kfree(buf);
	}
}

/**
 * cnss_wlan_gpio_send() - Send message to ADSP
 * @plat_priv: Platform private data
 * @data: Message data
 * @len: Message length
 *
 * Returns: 0 on success, negative error code on failure
 */
static int cnss_wlan_gpio_send(struct cnss_plat_data *plat_priv,
			       void *data, size_t len)
{
	struct cnss_wlan_gpio_data *gpio_data = plat_priv->wlan_gpio_data;
	struct rpmsg_device *rpdev;
	int rc;

	if (WARN_ON(!gpio_data)) {
		cnss_pr_err("Invalid GPIO data\n");
		return -ENODEV;
	}

	down_read(&gpio_data->rpdev_sem);

	rpdev = gpio_data->rpdev;
	if (!rpdev || !gpio_data->channel_up) {
		up_read(&gpio_data->rpdev_sem);
		cnss_pr_err("GPIO channel is down\n");
		return -ENOTCONN;
	}

	rc = rpmsg_send(rpdev->ept, data, len);
	up_read(&gpio_data->rpdev_sem);

	if (rc < 0)
		cnss_pr_err("Failed to send WLAN GPIO message, rc=%d\n", rc);
	else
		cnss_pr_dbg("Sent WLAN GPIO message: %*ph\n", (int)len, data);

	return rc;
}

/* TSF GPIO register/deregister timeout value */
#define GPIO_REG_DEREG_TIMEOUT_MS 1000

/**
 * cnss_wlan_gpio_register() - Register WLAN GPIO with ADSP
 *
 * Returns: 0 on success, negative error code on failure
 */
int cnss_wlan_gpio_register(void)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	struct cnss_wlan_gpio_data *gpio_data;
	struct wlan_gpio_register_cmd cmd;
	unsigned long ret;
	int rc;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	gpio_data = plat_priv->wlan_gpio_data;
	if (!gpio_data) {
		cnss_pr_err("WLAN GPIO data not initialized\n");
		return -ENODEV;
	}

	mutex_lock(&gpio_data->reg_lock);

	if (gpio_data->gpio_registered) {
		mutex_unlock(&gpio_data->reg_lock);
		cnss_pr_err("GPIO already registered\n");
		return -EBUSY;
	}

	init_completion(&gpio_data->gpio_reg_complete);

	cmd.cmd_type = WLAN_GPIO_CMD_REGISTER;

	rc = cnss_wlan_gpio_send(plat_priv, &cmd, sizeof(cmd));

	mutex_unlock(&gpio_data->reg_lock);

	if (!rc) {
		ret = wait_for_completion_timeout(
				&gpio_data->gpio_reg_complete,
				msecs_to_jiffies(GPIO_REG_DEREG_TIMEOUT_MS));
		mutex_lock(&gpio_data->reg_lock);
		if (!ret) {
			cnss_pr_err("GPIO registration timeout\n");
			rc = -ETIMEDOUT;
		} else if (!gpio_data->gpio_registered) {
			rc = -EINVAL;
		}
		mutex_unlock(&gpio_data->reg_lock);
	}

	return rc;
}
EXPORT_SYMBOL(cnss_wlan_gpio_register);

/**
 * cnss_wlan_gpio_deregister() - Deregister WLAN GPIO from ADSP
 *
 * Returns: 0 on success, negative error code on failure
 */
int cnss_wlan_gpio_deregister(void)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	struct cnss_wlan_gpio_data *gpio_data;
	struct wlan_gpio_deregister_cmd cmd;
	unsigned long ret;
	int rc;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	gpio_data = plat_priv->wlan_gpio_data;
	if (!gpio_data) {
		cnss_pr_err("WLAN GPIO data not initialized\n");
		return -ENODEV;
	}

	mutex_lock(&gpio_data->reg_lock);

	if (!gpio_data->gpio_registered) {
		mutex_unlock(&gpio_data->reg_lock);
		cnss_pr_err("GPIO not registered\n");
		return -EINVAL;
	}

	init_completion(&gpio_data->gpio_dereg_complete);

	cmd.cmd_type = WLAN_GPIO_CMD_DEREGISTER;

	rc = cnss_wlan_gpio_send(plat_priv, &cmd, sizeof(cmd));

	mutex_unlock(&gpio_data->reg_lock);

	if (!rc) {
		ret = wait_for_completion_timeout(
				&gpio_data->gpio_dereg_complete,
				msecs_to_jiffies(GPIO_REG_DEREG_TIMEOUT_MS));
		mutex_lock(&gpio_data->reg_lock);
		if (!ret) {
			cnss_pr_err("GPIO deregistration timeout\n");
			rc = -ETIMEDOUT;
		} else if (gpio_data->gpio_registered) {
			rc = -EINVAL;
		}
		mutex_unlock(&gpio_data->reg_lock);
	}

	return rc;
}
EXPORT_SYMBOL(cnss_wlan_gpio_deregister);

/**
 * cnss_wlan_gpio_rpmsg_callback() - RPMsg RX callback
 * @rpdev: RPMsg device
 * @data: Received data
 * @len: Data length
 * @priv: Private data
 * @addr: Source address
 *
 * Called when a message is received from ADSP.
 */
static int cnss_wlan_gpio_rpmsg_callback(struct rpmsg_device *rpdev,
					 void *data, int len,
					 void *priv, u32 addr)
{
	struct cnss_wlan_gpio_data *gpio_data = dev_get_drvdata(&rpdev->dev);
	struct cnss_wlan_gpio_buf *buf;
	unsigned long flags;

	if (!gpio_data) {
		cnss_pr_err("Invalid GPIO data\n");
		return -EINVAL;
	}

	if (len < 1) {
		cnss_pr_err("Invalid message length: %d\n", len);
		return -EINVAL;
	}

	/* Check for integer overflow before allocation */
	if (len > (SIZE_MAX - sizeof(*buf))) {
		cnss_pr_err("Message length too large: %d\n", len);
		return -EINVAL;
	}

	/*
	 * RPMsg callbacks typically run in process context and can sleep,
	 * so GFP_KERNEL is appropriate here instead of GFP_ATOMIC.
	 */
	buf = kzalloc(sizeof(*buf) + len, GFP_KERNEL);
	if (!buf) {
		cnss_pr_err("Failed to create WLAN GPIO buffer\n");
		return -ENOMEM;
	}

	buf->len = len;
	memcpy(buf->buf, data, len);

	spin_lock_irqsave(&gpio_data->rx_lock, flags);
	list_add_tail(&buf->node, &gpio_data->rx_list);
	spin_unlock_irqrestore(&gpio_data->rx_lock, flags);

	if (!queue_work(gpio_data->rx_wq, &gpio_data->rx_work)) {
		/* Work already queued, buffer will be processed */
		cnss_pr_dbg("RX work already queued, buffer will be processed\n");
	}

	return 0;
}

/**
 * cnss_wlan_gpio_rpmsg_remove() - RPMsg channel remove callback
 * @rpdev: RPMsg device
 *
 * Called when the "wlan_gpio_chnl" channel is removed (e.g., during SSR).
 */
static void cnss_wlan_gpio_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct cnss_wlan_gpio_data *gpio_data = dev_get_drvdata(&rpdev->dev);

	if (!gpio_data) {
		cnss_pr_err("Invalid GPIO data\n");
		return;
	}

	down_write(&gpio_data->rpdev_sem);
	gpio_data->channel_up = false;
	gpio_data->rpdev = NULL;
	up_write(&gpio_data->rpdev_sem);

	/* Flush workqueue to ensure no pending RX work */
	if (gpio_data->rx_wq)
		flush_workqueue(gpio_data->rx_wq);

	/* Clear registration state under mutex protection */
	mutex_lock(&gpio_data->reg_lock);
	gpio_data->gpio_registered = false;
	mutex_unlock(&gpio_data->reg_lock);

	cnss_pr_info("WLAN GPIO channel removed\n");
}

/**
 * cnss_wlan_gpio_rpmsg_probe() - RPMsg channel probe callback
 * @rpdev: RPMsg device
 *
 * Called when the "wlan_gpio_chnl" channel is established with ADSP.
 */
static int cnss_wlan_gpio_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	struct cnss_wlan_gpio_data *gpio_data;

	if (!plat_priv || !plat_priv->wlan_gpio_data) {
		cnss_pr_err("Invalid platform data\n");
		return -ENODEV;
	}

	gpio_data = plat_priv->wlan_gpio_data;

	down_write(&gpio_data->rpdev_sem);
	dev_set_drvdata(&rpdev->dev, gpio_data);
	gpio_data->rpdev = rpdev;
	gpio_data->channel_up = true;
	up_write(&gpio_data->rpdev_sem);

	cnss_pr_info("WLAN GPIO channel probed successfully\n");

	return 0;
}

static struct rpmsg_driver cnss_wlan_gpio_rpmsg_driver = {
	.id_table = NULL, /* Will be set during init */
	.probe = cnss_wlan_gpio_rpmsg_probe,
	.remove = cnss_wlan_gpio_rpmsg_remove,
	.callback = cnss_wlan_gpio_rpmsg_callback,
	.drv = {
		.name = "cnss_wlan_gpio_rpmsg",
	},
};

/**
 * cnss_wlan_gpio_init() - Initialize WLAN GPIO channel
 * @plat_priv: Platform private data
 *
 * Allocates and initializes WLAN GPIO data structures.
 * Reads channel name from DT property "qcom,wlan_gpio_channel".
 */
static int cnss_wlan_gpio_init(struct cnss_plat_data *plat_priv)
{
	struct cnss_wlan_gpio_data *gpio_data;
	struct device_node *np = plat_priv->plat_dev->dev.of_node;
	const char *channel_name;
	int ret;

	/* Read channel name from device tree */
	ret = of_property_read_string(np, "qcom,wlan_gpio_channel",
				      &channel_name);
	if (ret) {
		cnss_pr_err("Failed to read wlan_gpio_channel from DT ret=%d\n",
			    ret);
		return ret;
	}

	/* Validate channel name length before allocation */
	if (strlen(channel_name) >= RPMSG_NAME_SIZE) {
		cnss_pr_err("GPIO channel name too long: %zu (max %d)\n",
			    strlen(channel_name), RPMSG_NAME_SIZE - 1);
		return -EINVAL;
	}

	gpio_data = kzalloc(sizeof(*gpio_data), GFP_KERNEL);
	if (!gpio_data) {
		cnss_pr_err("Failed to create WLAN GPIO data buffer\n");
		return -ENOMEM;
	}

	/* Safe to copy - we already validated the length */
	strscpy(gpio_data->channel_name, channel_name, RPMSG_NAME_SIZE);

	/* Set up the embedded match table with the channel name */
	strscpy(gpio_data->rpmsg_match[0].name, channel_name, RPMSG_NAME_SIZE);
	/* Second entry is already zeroed by kzalloc */

	/* Set the match table in the driver */
	cnss_wlan_gpio_rpmsg_driver.id_table = gpio_data->rpmsg_match;

	gpio_data->rx_wq = alloc_workqueue("cnss_wlan_gpio_rx", WQ_UNBOUND, 1);
	if (!gpio_data->rx_wq) {
		cnss_pr_err("Failed to create WLAN GPIO RX workqueue\n");
		ret = -ENOMEM;
		goto err_free_gpio_data;
	}

	init_rwsem(&gpio_data->rpdev_sem);
	mutex_init(&gpio_data->reg_lock);
	INIT_WORK(&gpio_data->rx_work, cnss_wlan_gpio_rx_work);
	INIT_LIST_HEAD(&gpio_data->rx_list);
	spin_lock_init(&gpio_data->rx_lock);
	gpio_data->channel_up = false;
	gpio_data->gpio_registered = false;

	plat_priv->wlan_gpio_data = gpio_data;

	cnss_pr_info("WLAN GPIO channel initialized with name: %s\n",
		     gpio_data->channel_name);

	return 0;

err_free_gpio_data:
	kfree(gpio_data);
	return ret;
}

/**
 * cnss_wlan_gpio_deinit() - Deinitialize WLAN GPIO channel
 * @plat_priv: Platform private data
 */
static void cnss_wlan_gpio_deinit(struct cnss_plat_data *plat_priv)
{
	struct cnss_wlan_gpio_data *gpio_data = plat_priv->wlan_gpio_data;
	struct cnss_wlan_gpio_buf *buf, *tmp;
	unsigned long flags;

	if (!gpio_data) {
		cnss_pr_err("Invalid GPIO data\n");
		return;
	}

	/* Flush and destroy workqueue */
	if (gpio_data->rx_wq) {
		flush_workqueue(gpio_data->rx_wq);
		destroy_workqueue(gpio_data->rx_wq);
		gpio_data->rx_wq = NULL;
	}

	/* Free any pending RX buffers */
	spin_lock_irqsave(&gpio_data->rx_lock, flags);
	list_for_each_entry_safe(buf, tmp, &gpio_data->rx_list, node) {
		list_del(&buf->node);
		kfree(buf);
	}
	spin_unlock_irqrestore(&gpio_data->rx_lock, flags);

	kfree(gpio_data);
	plat_priv->wlan_gpio_data = NULL;

	cnss_pr_info("WLAN GPIO channel deinitialized\n");
}

void cnss_fw_ready_ind_event(struct device *dev)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return;
	}

	cnss_sdio_start_time_sync_update(plat_priv);
}
EXPORT_SYMBOL(cnss_fw_ready_ind_event);

int cnss2_sdio_force_fw_assert(struct device *dev)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	int ret;

	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return -ENODEV;
	}

	reinit_completion(&plat_priv->fw_assert_complete);

	cnss_bus_force_fw_assert_hdlr(plat_priv);

	ret = wait_for_completion_timeout(&plat_priv->fw_assert_complete,
					  msecs_to_jiffies(15000));

	if (!ret) {
		cnss_pr_err("timeout waiting for smc induce bug, reset SMC\n");
		smc_recovery_api_for_client();
	}

	return 0;
}
EXPORT_SYMBOL(cnss2_sdio_force_fw_assert);

static ssize_t recovery_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cnss_plat_data *plat_priv = dev_get_drvdata(dev);
	unsigned int recovery = 0;

	if (!plat_priv)
		return -ENODEV;

	if (sscanf(buf, "%du", &recovery) != 1) {
		cnss_pr_err("Invalid recovery sysfs command\n");
		return -EINVAL;
	}

	plat_priv->recovery_enabled = !!(recovery & CNSS_WLAN_RECOVERY);

	cnss_pr_dbg("%s WLAN recovery, count is %zu\n",
		    plat_priv->recovery_enabled ? "Enable" : "Disable", count);

	return count;
}

static ssize_t recovery_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct cnss_plat_data *plat_priv = dev_get_drvdata(dev);
	u32 buf_size = PAGE_SIZE;
	u32 curr_len = 0;
	u32 buf_written = 0;

	if (!plat_priv)
		return -ENODEV;

	buf_written = scnprintf(buf, buf_size,
				"Usage: echo [recovery_bitmap] > /sys/kernel/cnss/recovery\n"
				"BIT0 -- wlan fw recovery\n"
				"---------------------------------\n");
	curr_len += buf_written;

	buf_written = scnprintf(buf + curr_len, buf_size - curr_len,
				"WLAN recovery %s[%d]\n",
				plat_priv->recovery_enabled ? "Enabled" : "Disabled",
				plat_priv->recovery_enabled);
	curr_len += buf_written;

	return curr_len;
}

static ssize_t shutdown_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cnss_plat_data *plat_priv = dev_get_drvdata(dev);
	struct cnss_sdio_data *sdio_data = NULL;

	cnss_pr_info("Received shutdown notification\n");
	if (plat_priv) {
		if (test_and_set_bit(CNSS_IN_REBOOT, &plat_priv->driver_state)) {
			cnss_pr_info("Reboot already in progress, skip shutdown store\n");
			return count;
		}
		if (plat_priv->bus_priv)
			sdio_data = plat_priv->bus_priv;
		qcn_sdio_card_release();
		cnss2_sdio_update_status(sdio_data);
		complete_all(&plat_priv->power_up_complete);
		cnss_sdio_wlan_disable_subsys();
		cnss_pr_info("Shutdown notification handled\n");
	}

	return count;
}

static ssize_t time_sync_period_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct cnss_plat_data *plat_priv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%u ms\n",
			 plat_priv->ctrl_params.time_sync_period);
}

/**
 * cnss_get_min_time_sync_period_by_vote() - Get minimum time sync period
 * @plat_priv: Platform data structure
 *
 * Result: return minimum time sync period present in vote from wlan and sys
 */
static uint32_t cnss_get_min_time_sync_period_by_vote(struct cnss_plat_data *plat_priv)
{
	unsigned int i, min_time_sync_period = CNSS_TIME_SYNC_PERIOD_DEFAULT;
	unsigned int time_sync_period;

	for (i = 0; i < TIME_SYNC_VOTE_MAX; i++) {
		time_sync_period = plat_priv->ctrl_params.time_sync_period_vote[i];
		if (min_time_sync_period > time_sync_period)
			min_time_sync_period = time_sync_period;
	}

	return min_time_sync_period;
}

static ssize_t time_sync_period_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct cnss_plat_data *plat_priv = dev_get_drvdata(dev);
	unsigned int time_sync_period = 0;
	int ret;

	if (!plat_priv)
		return -ENODEV;

	if (sscanf(buf, "%du", &time_sync_period) != 1) {
		cnss_pr_err("Invalid time sync sysfs command\n");
		return -EINVAL;
	}

	if (time_sync_period < CNSS_MIN_TIME_SYNC_PERIOD) {
		cnss_pr_err("Time sync period %u ms is small (min: %u ms)\n",
			    time_sync_period, CNSS_MIN_TIME_SYNC_PERIOD);
		return -EINVAL;
	}

	if (plat_priv->ctrl_params.time_sync_period == time_sync_period) {
		cnss_pr_err("Time sync already enabled with same period: %d\n",
			    time_sync_period);
		return -EINVAL;
	}

	plat_priv->ctrl_params.time_sync_period_vote[TIME_SYNC_VOTE_CNSS] =
		time_sync_period;
	time_sync_period = cnss_get_min_time_sync_period_by_vote(plat_priv);

	ret = cnss_bus_update_time_sync_period(plat_priv, time_sync_period);
	if (ret) {
		cnss_pr_err("Failed to update time sync period, err = %d\n",
			    ret);
		return ret;
	}

	return count;
}

/**
 * cnss_update_time_sync_period() - Set time sync period given by driver
 * @dev: device structure
 * @time_sync_period: time sync period value
 *
 * Update time sync period vote of driver and set minimum of time sync period
 * from stored vote through wlan and sys config
 * Result: return 0 for success, error in case of invalid value and no dev
 */
int cnss_update_time_sync_period(struct device *dev, uint32_t time_sync_period)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	int ret;

	if (!plat_priv)
		return -ENODEV;

	if (time_sync_period < CNSS_MIN_TIME_SYNC_PERIOD) {
		cnss_pr_err("Time sync period %u ms is small (min: %u ms)\n",
			    time_sync_period, CNSS_MIN_TIME_SYNC_PERIOD);
		return -EINVAL;
	}

	if (plat_priv->ctrl_params.time_sync_period == time_sync_period) {
		cnss_pr_err("Time sync already enabled with same period: %d\n",
			    time_sync_period);
		return -EINVAL;
	}

	plat_priv->ctrl_params.time_sync_period_vote[TIME_SYNC_VOTE_WLAN] =
		time_sync_period;
	time_sync_period = cnss_get_min_time_sync_period_by_vote(plat_priv);

	ret = cnss_bus_update_time_sync_period(plat_priv, time_sync_period);
	if (ret) {
		cnss_pr_err("Failed to update time sync period, err = %d\n",
			    ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(cnss_update_time_sync_period);

/**
 * cnss_reset_time_sync_period() - Reset time sync period
 * @dev: device structure
 *
 * Update time sync period vote of driver as default and reset minimum of time
 * sync period from stored vote through wlan and sys config
 *
 * Result: return 0 for success, error in case of no dev
 */
int cnss_reset_time_sync_period(struct device *dev)
{
	struct cnss_plat_data *plat_priv = cnss_bus_dev_to_plat_priv(NULL);
	unsigned int time_sync_period = 0;
	int ret;

	if (!plat_priv)
		return -ENODEV;

	if (plat_priv->ctrl_params.time_sync_period ==
	    CNSS_TIME_SYNC_PERIOD_DEFAULT) {
		cnss_pr_err("Time sync already reset to period: %d\n",
			    CNSS_TIME_SYNC_PERIOD_DEFAULT);
		return -EINVAL;
	}

	/* Driver vote is set to default in case of reset */
	plat_priv->ctrl_params.time_sync_period_vote[TIME_SYNC_VOTE_WLAN] =
						CNSS_TIME_SYNC_PERIOD_DEFAULT;
	time_sync_period = cnss_get_min_time_sync_period_by_vote(plat_priv);

	ret = cnss_bus_update_time_sync_period(plat_priv, time_sync_period);
	if (ret) {
		cnss_pr_err("Failed to update time sync period, err = %d\n",
			    ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(cnss_reset_time_sync_period);

static DEVICE_ATTR_RW(time_sync_period);
static DEVICE_ATTR_WO(shutdown);
static DEVICE_ATTR_RW(recovery);

static struct attribute *cnss_attrs[] = {
	&dev_attr_time_sync_period.attr,
	&dev_attr_shutdown.attr,
	&dev_attr_recovery.attr,
	NULL,
};

static struct attribute_group cnss_attr_group = {
	.attrs = cnss_attrs,
};

static int cnss_create_sysfs_link(struct cnss_plat_data *plat_priv)
{
	struct device *dev = &plat_priv->plat_dev->dev;
	int ret;
	char cnss_name[CNSS_FS_NAME_SIZE];
	char shutdown_name[32];

	if (cnss_is_dual_wlan_enabled()) {
		snprintf(cnss_name, CNSS_FS_NAME_SIZE,
			 CNSS_FS_NAME "_%d", plat_priv->plat_idx);
		snprintf(shutdown_name, sizeof(shutdown_name),
			 "shutdown_wlan_%d", plat_priv->plat_idx);
	} else {
		snprintf(cnss_name, CNSS_FS_NAME_SIZE, CNSS_FS_NAME);
		snprintf(shutdown_name, sizeof(shutdown_name),
			 "shutdown_wlan");
	}

	ret = sysfs_create_link(kernel_kobj, &dev->kobj, cnss_name);
	if (ret) {
		cnss_pr_err("Failed to create cnss link, err = %d\n",
			    ret);
		goto out;
	}

	/* This is only for backward compatibility. */
	ret = sysfs_create_link(kernel_kobj, &dev->kobj, shutdown_name);
	if (ret) {
		cnss_pr_err("Failed to create shutdown_wlan link, err = %d\n",
			    ret);
		goto rm_cnss_link;
	}

	return 0;

rm_cnss_link:
	sysfs_remove_link(kernel_kobj, cnss_name);
out:
	return ret;
}

static void cnss_remove_sysfs_link(struct cnss_plat_data *plat_priv)
{
	char cnss_name[CNSS_FS_NAME_SIZE];
	char shutdown_name[32];

	if (cnss_is_dual_wlan_enabled()) {
		snprintf(cnss_name, CNSS_FS_NAME_SIZE,
			 CNSS_FS_NAME "_%d", plat_priv->plat_idx);
		snprintf(shutdown_name, sizeof(shutdown_name),
			 "shutdown_wlan_%d", plat_priv->plat_idx);
	} else {
		snprintf(cnss_name, CNSS_FS_NAME_SIZE, CNSS_FS_NAME);
		snprintf(shutdown_name, sizeof(shutdown_name),
			 "shutdown_wlan");
	}

	sysfs_remove_link(kernel_kobj, shutdown_name);
	sysfs_remove_link(kernel_kobj, cnss_name);
}

static int cnss_create_sysfs(struct cnss_plat_data *plat_priv)
{
	int ret = 0;

	ret = devm_device_add_group(&plat_priv->plat_dev->dev,
				    &cnss_attr_group);
	if (ret) {
		cnss_pr_err("Failed to create cnss device group, err = %d\n",
			    ret);
		goto out;
	}

	cnss_create_sysfs_link(plat_priv);

	return 0;
out:
	return ret;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0))
union cnss_device_group_devres {
	const struct attribute_group *group;
};

static void devm_cnss_group_remove(struct device *dev, void *res)
{
	union cnss_device_group_devres *devres = res;
	const struct attribute_group *group = devres->group;

	cnss_pr_dbg("%s: removing group %p\n", __func__, group);
	sysfs_remove_group(&dev->kobj, group);
}

static int devm_cnss_group_match(struct device *dev, void *res, void *data)
{
	return ((union cnss_device_group_devres *)res) == data;
}

static void cnss_remove_sysfs(struct cnss_plat_data *plat_priv)
{
	cnss_remove_sysfs_link(plat_priv);
	WARN_ON(devres_release(&plat_priv->plat_dev->dev,
			       devm_cnss_group_remove, devm_cnss_group_match,
			       (void *)&cnss_attr_group));
}
#else
static void cnss_remove_sysfs(struct cnss_plat_data *plat_priv)
{
	cnss_remove_sysfs_link(plat_priv);
	device_remove_group(&plat_priv->plat_dev->dev, &cnss_attr_group);
}
#endif

static int cnss_probe(struct platform_device *plat_dev)
{
	int ret = 0;
	struct cnss_plat_data *plat_priv;
	const struct of_device_id *of_id;
	const struct platform_device_id *device_id;

	cnss_dev = &plat_dev->dev;

	/*
	 * If SMC is not ready then return success and unbind/bind
	 * driver later on smc ready. If we return EPROBE_DEFER, there
	 * is no specific time when kernel will call probe again.
	 */
	if (!smc_them_ready) {
		cnss_pr_err("Probe deferred");
		return 0;
	}

	if (cnss_get_plat_priv(plat_dev)) {
		cnss_pr_err("Driver is already initialized!\n");
		ret = -EEXIST;
		goto out;
	}

	of_id = of_match_device(cnss_of_match_table, &plat_dev->dev);
	if (!of_id || !of_id->data) {
		cnss_pr_err("Failed to find of match device!\n");
		ret = -ENODEV;
		goto out;
	}

	device_id = of_id->data;

	plat_priv = devm_kzalloc(&plat_dev->dev, sizeof(*plat_priv),
				 GFP_KERNEL);
	if (!plat_priv) {
		ret = -ENOMEM;
		goto out;
	}

	plat_priv->plat_dev = plat_dev;
	plat_priv->dev_node = NULL;
	plat_priv->ssr_state = SSR_STATE_IDLE;
	plat_priv->smc_crash_state = 0;
	plat_priv->device_id = device_id->driver_data;
	plat_priv->dt_type = cnss_dt_type(plat_priv);
	init_completion(&plat_priv->power_up_complete);
	init_completion(&plat_priv->power_down_complete);
	init_completion(&plat_priv->fw_assert_complete);
	mutex_init(&plat_priv->tcdev_lock);
	INIT_LIST_HEAD(&plat_priv->cnss_tcdev_list);
	cnss_pr_info("Probing platform driver from dt type: %d\n",
		     plat_priv->dt_type);

	plat_priv->bus_type = cnss_get_bus_type(plat_priv);
	cnss_set_plat_priv(plat_dev, plat_priv);
	cnss_set_device_name(plat_priv);

	cnss_init_time_sync_period_default(plat_priv);
	init_completion(&plat_priv->time_sync_notif);

	ret = cnss_create_sysfs(plat_priv);
	if (ret)
		goto reset_ctx;

	ret = cnss_event_work_init(plat_priv);
	if (ret)
		goto remove_sysfs;

	ret = cnss_debugfs_create(plat_priv);
	if (ret)
		goto deinit_event_work;

	ret = cnss_register_smc(plat_priv);
	if (ret)
		goto destroy_debugfs;

	/* Initialize WLAN GPIO channel */
	ret = cnss_wlan_gpio_init(plat_priv);
	if (ret) {
		cnss_pr_err("Failed to initialize WLAN GPIO channel, ret=%d\n",
			    ret);
		/* Continue without GPIO support - not a fatal error */
	} else {
		/* Register RPMsg driver for WLAN GPIO channel */
		ret = register_rpmsg_driver(&cnss_wlan_gpio_rpmsg_driver);
		if (ret) {
			cnss_pr_err("Failed to register WLAN GPIO RPMsg driver, ret=%d\n",
				    ret);
			cnss_wlan_gpio_deinit(plat_priv);
		}
	}

	/* Make sure all platform related init are done before
	 * device power on and bus init.
	 */
	ret = cnss_wlan_device_init(plat_priv);
	if (ret)
		goto unregister_rpmsg;

	atomic_set(&plat_priv->power_up_retry_cnt, CNSS_SMC_EVENT_RETRY_COUNT);
	atomic_set(&plat_priv->power_down_retry_cnt, CNSS_SMC_EVENT_RETRY_COUNT);

	cnss_sdio_wlan_enable_subsys();

	ret = wait_for_completion_timeout(&plat_priv->power_up_complete,
					  msecs_to_jiffies(20000));
	if (!ret) {
		cnss_pr_err("Timed out turning on Meson");
		ret = -EPROBE_DEFER;
		goto deinit_device;
	}

	platform_set_drvdata(plat_dev, plat_priv);
	cnss_get_tsf_ts_info(plat_priv);

	ret = device_init_wakeup(&plat_dev->dev, true);
	if (ret)
		cnss_pr_err("Failed to init platform device wakeup source, err = %d\n",
			    ret);

	plat_priv->reboot_nb.notifier_call = cnss_reboot_notifier;
	plat_priv->reboot_nb.priority = INT_MAX;
	ret = register_reboot_notifier(&plat_priv->reboot_nb);
	if (ret)
		cnss_pr_err("Failed to register reboot notifier, err = %d\n",
			    ret);

	cnss_pr_info("Platform driver probed successfully.\n");

	return 0;

deinit_device:
	cnss_sdio_deinit(plat_priv);
unregister_rpmsg:
	if (plat_priv->wlan_gpio_data) {
		unregister_rpmsg_driver(&cnss_wlan_gpio_rpmsg_driver);
		cnss_wlan_gpio_deinit(plat_priv);
	}
	cnss_unregister_smc(plat_priv);
destroy_debugfs:
	cnss_debugfs_destroy(plat_priv);
deinit_event_work:
	cnss_event_work_deinit(plat_priv);
remove_sysfs:
	cnss_remove_sysfs(plat_priv);
reset_ctx:
	platform_set_drvdata(plat_dev, NULL);
	cnss_clear_plat_priv(plat_priv);
out:
	return ret;
}

static void cnss_remove(struct platform_device *plat_dev)
{
	struct cnss_plat_data *plat_priv = platform_get_drvdata(plat_dev);

	if (!plat_priv)
		return;

	device_init_wakeup(&plat_dev->dev, false);
	unregister_reboot_notifier(&plat_priv->reboot_nb);
	cnss_genl_exit();
	cnss_bus_deinit(plat_priv);

	/* Unregister and deinitialize WLAN GPIO channel */
	if (plat_priv->wlan_gpio_data) {
		unregister_rpmsg_driver(&cnss_wlan_gpio_rpmsg_driver);
		cnss_wlan_gpio_deinit(plat_priv);
	}

	cnss_unregister_smc(plat_priv);
	cnss_debugfs_destroy(plat_priv);
	cnss_event_work_deinit(plat_priv);
	cnss_remove_sysfs(plat_priv);
	platform_set_drvdata(plat_dev, NULL);
	cnss_clear_plat_priv(plat_priv);

	return;
}

static struct platform_driver cnss_platform_driver = {
	.probe  = cnss_probe,
	.remove = cnss_remove,
	.driver = {
		.name = "cnss2",
		.of_match_table = cnss_of_match_table,
#ifdef CONFIG_CNSS_ASYNC
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
#endif
	},
};

static bool cnss_check_compatible_node(void)
{
	struct device_node *dn = NULL;

	for_each_matching_node(dn, cnss_of_match_table) {
		if (of_device_is_available(dn)) {
			cnss_allow_driver_loading = true;
			return true;
		}
	}

	return false;
}

/**
 * cnss_is_valid_dt_node_found - Check if valid device tree node present
 *
 * Valid device tree node means a node with "compatible" property from the
 * device match table and "status" property is not disabled.
 *
 * Return: true if valid device tree node found, false if not found
 */
static bool cnss_is_valid_dt_node_found(void)
{
	struct device_node *dn = NULL;

	for_each_matching_node(dn, cnss_of_match_table) {
		if (of_device_is_available(dn))
			break;
	}

	if (dn)
		return true;

	return false;
}

static unsigned long cnss_get_device_id_from_dt(void)
{
	struct device_node *dn = NULL;
	const struct of_device_id *match;
	const struct platform_device_id *platform_id;
	unsigned long device_id = 0;

	for_each_matching_node(dn, cnss_of_match_table) {
		if (of_device_is_available(dn)) {
			match = of_match_node(cnss_of_match_table, dn);
			if (match && match->data) {
				platform_id = (const struct platform_device_id *)match->data;
				device_id = platform_id->driver_data;
				cnss_pr_info("Found device in DT: %s, device_id: 0x%lx\n",
					     platform_id->name, device_id);
				of_node_put(dn);
				break;
			}
		}
	}

	return device_id;
}

static int __init cnss_initialize(void)
{
	int ret = 0;
	unsigned long device_id;

	if (!cnss_is_valid_dt_node_found())
		return -ENODEV;

	if (!cnss_check_compatible_node())
		return ret;

	/* Extract device_id from device tree */
	device_id = cnss_get_device_id_from_dt();
	if (device_id)
		cnss_initialize_mem_pool(device_id);

	cnss_debug_init();
	ret = platform_driver_register(&cnss_platform_driver);
	if (ret) {
		cnss_debug_deinit();
		cnss_deinitialize_mem_pool();
	}

	ret = cnss_genl_init();
	if (ret < 0)
		cnss_pr_err("CNSS genl init failed %d\n", ret);

	cnss_them_smc_register();
	return ret;
}

static void __exit cnss_exit(void)
{
	cnss_them_smc_unregister();
	cnss_genl_exit();
	platform_driver_unregister(&cnss_platform_driver);
	cnss_debug_deinit();
	cnss_deinitialize_mem_pool();
}

module_init(cnss_initialize);
module_exit(cnss_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CNSS2_SDIO Platform Driver");
