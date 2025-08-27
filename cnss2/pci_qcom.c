// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved. */

#include "pci_platform.h"
#include "debug.h"
#include "linux/of_address.h"

static struct cnss_msi_config msi_config = {
	.total_vectors = 32,
	.total_users = MSI_USERS,
	.users = (struct cnss_msi_user[]) {
		{ .name = "MHI", .num_vectors = 3, .base_vector = 0 },
		{ .name = "CE", .num_vectors = 10, .base_vector = 3 },
		{ .name = "WAKE", .num_vectors = 1, .base_vector = 13 },
		{ .name = "DP", .num_vectors = 18, .base_vector = 14 },
	},
};

/**
 * cnss_pci_is_sync_probe(): check whether wlan device
 * is powered with scmi way.
 *
 * For upstream PCIe ECAM driver, wlan powerup/PCIe enumeration
 * is controlled by low level GearVM system with scmi way.
 * This API is used to distinguish downstream/upstream PCIe
 * driver case.
 *
 * Return: true for scmi way, false for non-scmi way
 */
static bool cnss_is_fw_managed_pwr(struct cnss_pci_data *pci_priv)
{
	struct cnss_plat_data *plat_priv;

	if (!pci_priv) {
		cnss_pr_err("pci_priv is NULL\n");
		return false;
	}

	plat_priv = pci_priv->plat_priv;
	if (!plat_priv) {
		cnss_pr_err("plat_priv is NULL\n");
		return false;
	}

	return plat_priv->is_fw_managed_pwr;
}

int _cnss_pci_enumerate(struct cnss_plat_data *plat_priv, u32 rc_num)
{
	if (plat_priv->is_fw_managed_pwr)
		return 0;
	else
		return msm_pcie_enumerate(rc_num);
}

int cnss_pci_assert_perst(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;

	if (cnss_is_fw_managed_pwr(pci_priv))
		return -EOPNOTSUPP;
	else
		return msm_pcie_pm_control(MSM_PCIE_HANDLE_LINKDOWN,
				pci_dev->bus->number, pci_dev, NULL,
				PM_OPTIONS_DEFAULT);
}

#if IS_ENABLED(CONFIG_CNSS2_FMD_FEATURE_ENABLE)
int cnss_pci_fmd_enable(struct cnss_pci_data *pci_priv)
{
	if (cnss_is_fw_managed_pwr(pci_priv))
		return -EOPNOTSUPP;
	else
		return msm_pcie_fmd_enable(pci_priv->pci_dev);
}
#else
int cnss_pci_fmd_enable(struct cnss_pci_data *pci_priv)
{
	return -EOPNOTSUPP;
}
#endif

int cnss_pci_disable_pc(struct cnss_pci_data *pci_priv, bool vote)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;

	if (cnss_is_fw_managed_pwr(pci_priv))
		return 0;
	else
		return msm_pcie_pm_control(vote ? MSM_PCIE_DISABLE_PC :
					MSM_PCIE_ENABLE_PC,
					pci_dev->bus->number, pci_dev, NULL,
					PM_OPTIONS_DEFAULT);
}

int cnss_pci_set_link_bandwidth(struct cnss_pci_data *pci_priv,
				u16 link_speed, u16 link_width)
{
	if (cnss_is_fw_managed_pwr(pci_priv))
		return 0;
	else
		return msm_pcie_set_link_bandwidth(pci_priv->pci_dev,
						link_speed, link_width);
}

int cnss_pci_set_max_link_speed(struct cnss_pci_data *pci_priv,
				u32 rc_num, u16 link_speed)
{
	if (cnss_is_fw_managed_pwr(pci_priv))
		return 0;
	else
		return msm_pcie_set_target_link_speed(rc_num,
						link_speed, false);
}

/**
 * _cnss_pci_prevent_l1() - Prevent PCIe L1 and L1 sub-states
 * @pci_priv: driver PCI bus context pointer
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to prevent PCIe link enter L1 and L1 sub-states. The APIs should also
 * bring link out of L1 or L1 sub-states if any and avoid synchronization
 * issues if any.
 *
 * Return: 0 for success, negative value for error
 */
static int _cnss_pci_prevent_l1(struct cnss_pci_data *pci_priv)
{
	return msm_pcie_prevent_l1(pci_priv->pci_dev);
}

/**
 * _cnss_pci_allow_l1() - Allow PCIe L1 and L1 sub-states
 * @pci_priv: driver PCI bus context pointer
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to allow PCIe link enter L1 and L1 sub-states. The APIs should avoid
 * synchronization issues if any.
 *
 * Return: 0 for success, negative value for error
 */
static void _cnss_pci_allow_l1(struct cnss_pci_data *pci_priv)
{
	msm_pcie_allow_l1(pci_priv->pci_dev);
}

/**
 * cnss_pci_set_link_up() - Power on or resume PCIe link
 * @pci_priv: driver PCI bus context pointer
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to Power on or resume PCIe link.
 *
 * Return: 0 for success, negative value for error
 */
static int cnss_pci_set_link_up(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	enum msm_pcie_pm_opt pm_ops = MSM_PCIE_RESUME;
	u32 pm_options = PM_OPTIONS_DEFAULT;
	int ret;

	ret = msm_pcie_pm_control(pm_ops, pci_dev->bus->number, pci_dev,
				  NULL, pm_options);
	if (ret)
		cnss_pr_err("Failed to resume PCI link with default option, err = %d\n",
			    ret);

	return ret;
}

/**
 * cnss_pci_set_link_down() - Power off or suspend PCIe link
 * @pci_priv: driver PCI bus context pointer
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to power off or suspend PCIe link.
 *
 * Return: 0 for success, negative value for error
 */
static int cnss_pci_set_link_down(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	enum msm_pcie_pm_opt pm_ops;
	u32 pm_options = PM_OPTIONS_DEFAULT;
	int ret;

	if (pci_priv->drv_connected_last) {
		cnss_pr_vdbg("Use PCIe DRV suspend\n");
		pm_ops = MSM_PCIE_DRV_SUSPEND;
	} else {
		pm_ops = MSM_PCIE_SUSPEND;
	}

	ret = msm_pcie_pm_control(pm_ops, pci_dev->bus->number, pci_dev,
				  NULL, pm_options);
	if (ret)
		cnss_pr_err("Failed to suspend PCI link with default option, err = %d\n",
			    ret);

	return ret;
}

void cnss_pci_update_drv_supported(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *root_port = pcie_find_root_port(pci_priv->pci_dev);
	struct cnss_plat_data *plat_priv = pci_priv->plat_priv;
	struct device_node *root_of_node;
	bool drv_supported = false;

	if (plat_priv && plat_priv->is_fw_managed_pwr) {
		pci_priv->drv_supported = false;
		return;
	}

	if (!root_port) {
		cnss_pr_err("PCIe DRV is not supported as root port is null\n");
		pci_priv->drv_supported = false;
		return;
	}

	root_of_node = root_port->dev.of_node;

	if (root_of_node->parent) {
		drv_supported = of_property_read_bool(root_of_node->parent,
						      "qcom,drv-supported") ||
				of_property_read_bool(root_of_node->parent,
						      "qcom,drv-name");
	}

	cnss_pr_dbg("PCIe DRV is %s\n",
		    drv_supported ? "supported" : "not supported");
	pci_priv->drv_supported = drv_supported;

	if (drv_supported) {
		plat_priv->cap.cap_flag |= CNSS_HAS_DRV_SUPPORT;
		cnss_set_feature_list(plat_priv, CNSS_DRV_SUPPORT_V01);
	}
}

static void cnss_pci_event_cb(struct msm_pcie_notify *notify)
{
	struct pci_dev *pci_dev;
	struct cnss_pci_data *pci_priv;
	struct device *dev;
	struct cnss_plat_data *plat_priv = NULL;
	int ret = 0;

	if (!notify)
		return;

	pci_dev = notify->user;
	if (!pci_dev)
		return;

	pci_priv = cnss_get_pci_priv(pci_dev);
	if (!pci_priv)
		return;
	dev = &pci_priv->pci_dev->dev;

	switch (notify->event) {
	case MSM_PCIE_EVENT_LINK_RECOVER:
		cnss_pr_dbg("PCI link recover callback\n");

		plat_priv = pci_priv->plat_priv;
		if (!plat_priv) {
			cnss_pr_err("plat_priv is NULL\n");
			return;
		}

		plat_priv->ctrl_params.quirks |= BIT(LINK_DOWN_SELF_RECOVERY);

		ret = msm_pcie_pm_control(MSM_PCIE_HANDLE_LINKDOWN,
					  pci_dev->bus->number, pci_dev, NULL,
					  PM_OPTIONS_DEFAULT);
		if (ret)
			cnss_pci_handle_linkdown(pci_priv);
		break;
	case MSM_PCIE_EVENT_LINKDOWN:
		cnss_pr_dbg("PCI link down event callback\n");
		cnss_pci_handle_linkdown(pci_priv);
		break;
	case MSM_PCIE_EVENT_WAKEUP:
		cnss_pr_dbg("PCI Wake up event callback\n");
		if ((cnss_pci_get_monitor_wake_intr(pci_priv) &&
		     cnss_pci_get_auto_suspended(pci_priv)) ||
		     dev->power.runtime_status == RPM_SUSPENDING) {
			cnss_pci_set_monitor_wake_intr(pci_priv, false);
			cnss_pci_pm_request_resume(pci_priv);
		}
		complete(&pci_priv->wake_event_complete);
		break;
	case MSM_PCIE_EVENT_DRV_CONNECT:
		cnss_pr_dbg("DRV subsystem is connected\n");
		cnss_pci_set_drv_connected(pci_priv, 1);
		break;
	case MSM_PCIE_EVENT_DRV_DISCONNECT:
		cnss_pr_dbg("DRV subsystem is disconnected\n");
		if (cnss_pci_get_auto_suspended(pci_priv))
			cnss_pci_pm_request_resume(pci_priv);
		cnss_pci_set_drv_connected(pci_priv, 0);
		break;
	default:
		cnss_pr_err("Received invalid PCI event: %d\n", notify->event);
	}
}

int cnss_reg_pci_event(struct cnss_pci_data *pci_priv)
{
	int ret = 0;
	struct msm_pcie_register_event *pci_event;

	if (cnss_is_fw_managed_pwr(pci_priv))
		return ret;

	pci_event = &pci_priv->msm_pci_event;
	pci_event->events = MSM_PCIE_EVENT_LINK_RECOVER |
			    MSM_PCIE_EVENT_LINKDOWN |
			    MSM_PCIE_EVENT_WAKEUP;

	if (cnss_pci_get_drv_supported(pci_priv))
		pci_event->events = pci_event->events |
			MSM_PCIE_EVENT_DRV_CONNECT |
			MSM_PCIE_EVENT_DRV_DISCONNECT;

	pci_event->user = pci_priv->pci_dev;
	pci_event->mode = MSM_PCIE_TRIGGER_CALLBACK;
	pci_event->callback = cnss_pci_event_cb;
	pci_event->options = MSM_PCIE_CONFIG_NO_RECOVERY;

	ret = msm_pcie_register_event(pci_event);
	if (ret)
		cnss_pr_err("Failed to register MSM PCI event, err = %d\n",
			    ret);

	return ret;
}

void cnss_dereg_pci_event(struct cnss_pci_data *pci_priv)
{
	if (!cnss_is_fw_managed_pwr(pci_priv))
		msm_pcie_deregister_event(&pci_priv->msm_pci_event);
}

int cnss_wlan_adsp_pc_enable(struct cnss_pci_data *pci_priv,
			     bool control)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	int ret = 0;
	u32 pm_options = PM_OPTIONS_DEFAULT;
	struct cnss_plat_data *plat_priv = pci_priv->plat_priv;

	if (plat_priv && plat_priv->is_fw_managed_pwr)
		return ret;

	if (!cnss_pci_get_drv_supported(pci_priv))
		return 0;

	if (plat_priv->adsp_pc_enabled == control) {
		cnss_pr_dbg("ADSP power collapse already %s\n",
			    control ? "Enabled" : "Disabled");
		return 0;
	}

	if (control)
		pm_options &= ~MSM_PCIE_CONFIG_NO_DRV_PC;
	else
		pm_options |= MSM_PCIE_CONFIG_NO_DRV_PC;

	ret = msm_pcie_pm_control(MSM_PCIE_DRV_PC_CTRL, pci_dev->bus->number,
				  pci_dev, NULL, pm_options);
	if (ret)
		return ret;

	cnss_pr_dbg("%s ADSP power collapse\n", control ? "Enable" : "Disable");
	plat_priv->adsp_pc_enabled = control;
	return 0;
}

static int cnss_set_pci_link_status(struct cnss_pci_data *pci_priv,
				    enum pci_link_status status)
{
	u16 link_speed, link_width = pci_priv->def_link_width;
	u16 one_lane = PCI_EXP_LNKSTA_NLW_X1 >> PCI_EXP_LNKSTA_NLW_SHIFT;
	int ret;

	cnss_pr_vdbg("Set PCI link status to: %u\n", status);

	switch (status) {
	case PCI_GEN1:
		link_speed = PCI_EXP_LNKSTA_CLS_2_5GB;
		if (!link_width)
			link_width = one_lane;
		break;
	case PCI_GEN2:
		link_speed = PCI_EXP_LNKSTA_CLS_5_0GB;
		if (!link_width)
			link_width = one_lane;
		break;
	case PCI_DEF:
		link_speed = pci_priv->def_link_speed;
		if (!link_speed || !link_width) {
			cnss_pr_err("PCI link speed or width is not valid\n");
			return -EINVAL;
		}
		break;
	default:
		cnss_pr_err("Unknown PCI link status config: %u\n", status);
		return -EINVAL;
	}

	ret = cnss_pci_set_link_bandwidth(pci_priv, link_speed, link_width);
	if (!ret)
		pci_priv->cur_link_speed = link_speed;

	return ret;
}

int cnss_set_pci_link(struct cnss_pci_data *pci_priv, bool link_up)
{
	int ret = 0, retry = 0;
	struct cnss_plat_data *plat_priv;
	int sw_ctrl_gpio;

	plat_priv = pci_priv->plat_priv;
	sw_ctrl_gpio = plat_priv->pinctrl_info.sw_ctrl_gpio;

	cnss_pr_vdbg("%s PCI link\n", link_up ? "Resuming" : "Suspending");

	if (plat_priv && plat_priv->is_fw_managed_pwr)
		return ret;

	if (link_up) {
retry:
		ret = cnss_pci_set_link_up(pci_priv);
		if (ret && retry++ < LINK_TRAINING_RETRY_MAX_TIMES) {
			cnss_pr_dbg("Retry PCI link training #%d\n", retry);
			cnss_pr_dbg("Value of SW_CTRL GPIO: %d\n",
				    cnss_get_input_gpio_value(plat_priv, sw_ctrl_gpio));
			if (pci_priv->pci_link_down_ind)
				msleep(LINK_TRAINING_RETRY_DELAY_MS * retry);
			goto retry;
		}
	} else {
		/* Since DRV suspend cannot be done in Gen 3, set it to
		 * Gen 2 if current link speed is larger than Gen 2.
		 */

		cnss_pci_get_link_status(pci_priv);
		if (pci_priv->drv_connected_last &&
		    pci_priv->cur_link_speed > PCI_EXP_LNKSTA_CLS_5_0GB)
			cnss_set_pci_link_status(pci_priv, PCI_GEN2);

		ret = cnss_pci_set_link_down(pci_priv);
	}

	if (pci_priv->drv_connected_last) {
		if ((link_up && !ret) || (!link_up && ret))
			cnss_set_pci_link_status(pci_priv, PCI_DEF);
	}

	return ret;
}

int cnss_pci_prevent_l1(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct cnss_pci_data *pci_priv = cnss_get_pci_priv(pci_dev);
	int ret = 0;

	if (!pci_priv) {
		cnss_pr_err("pci_priv is NULL\n");
		return -ENODEV;
	}

	if (cnss_is_fw_managed_pwr(pci_priv))
		return ret;

	mutex_lock(&pci_priv->bus_lock);
	ret = __cnss_pci_prevent_l1(dev);
	mutex_unlock(&pci_priv->bus_lock);

	return ret;
}
EXPORT_SYMBOL(cnss_pci_prevent_l1);

int __cnss_pci_prevent_l1(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct cnss_pci_data *pci_priv = cnss_get_pci_priv(pci_dev);
	int ret = 0;

	if (!pci_priv) {
		cnss_pr_err("pci_priv is NULL\n");
		return -ENODEV;
	}

	if (cnss_is_fw_managed_pwr(pci_priv))
		return ret;

	if (pci_priv->pci_link_state == PCI_LINK_DOWN) {
		cnss_pr_err("PCIe link is in suspend state\n");
		return -EIO;
	}

	if (pci_priv->pci_link_down_ind) {
		cnss_pr_err("PCIe link is down\n");
		return -EIO;
	}

	ret = _cnss_pci_prevent_l1(pci_priv);
	if (ret == -EIO) {
		cnss_pr_err("Failed to prevent PCIe L1, considered as link down\n");
		cnss_pci_link_down(dev);
	}

	return ret;
}

void cnss_pci_allow_l1(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct cnss_pci_data *pci_priv = cnss_get_pci_priv(pci_dev);

	if (!pci_priv) {
		cnss_pr_err("pci_priv is NULL\n");
		return;
	}

	if (cnss_is_fw_managed_pwr(pci_priv))
		return;

	mutex_lock(&pci_priv->bus_lock);
	__cnss_pci_allow_l1(dev);
	mutex_unlock(&pci_priv->bus_lock);
}
EXPORT_SYMBOL(cnss_pci_allow_l1);

void __cnss_pci_allow_l1(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct cnss_pci_data *pci_priv = cnss_get_pci_priv(pci_dev);

	if (!pci_priv) {
		cnss_pr_err("pci_priv is NULL\n");
		return;
	}

	if (cnss_is_fw_managed_pwr(pci_priv))
		return;

	if (pci_priv->pci_link_state == PCI_LINK_DOWN) {
		cnss_pr_err("PCIe link is in suspend state\n");
		return;
	}

	if (pci_priv->pci_link_down_ind) {
		cnss_pr_err("PCIe link is down\n");
		return;
	}

	_cnss_pci_allow_l1(pci_priv);
}

bool cnss_pci_is_sync_probe(struct cnss_plat_data *plat_priv)
{
	if (plat_priv->is_fw_managed_pwr)
		return false;
	else
		return true;
}

int cnss_pci_get_msi_assignment(struct cnss_pci_data *pci_priv)
{
	pci_priv->msi_config = &msi_config;

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)) && \
    (LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0))
static int
cnss_pci_smmu_dev_fault_handler(struct iommu_fault *fault,  void *data)
{
	struct cnss_pci_data *pci_priv = data;

	cnss_fatal_err("SMMU fault happened with IOVA 0x%llx\n",
		       fault->event.addr);

	if (!pci_priv) {
		cnss_pr_err("pci_priv is NULL\n");
		return -ENODEV;
	}

	pci_priv->is_smmu_fault = true;
	cnss_pci_update_status(pci_priv, CNSS_FW_DOWN);
	cnss_force_fw_assert(&pci_priv->pci_dev->dev);

	/* IOMMU driver requires -ENOSYS to print debug info. */
	return -ENOSYS;
}

static
void cnss_register_iommu_fault_handler(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;

	iommu_register_device_fault_handler(&pci_dev->dev,
					    cnss_pci_smmu_dev_fault_handler,
					    pci_priv);
}
#else
static int cnss_pci_smmu_fault_handler(struct iommu_domain *domain,
				       struct device *dev, unsigned long iova,
				       int flags, void *handler_token)
{
	struct cnss_pci_data *pci_priv = handler_token;

	cnss_fatal_err("SMMU fault happened with IOVA 0x%lx\n", iova);

	if (!pci_priv) {
		cnss_pr_err("pci_priv is NULL\n");
		return -ENODEV;
	}

	pci_priv->is_smmu_fault = true;
	cnss_pci_update_status(pci_priv, CNSS_FW_DOWN);
	cnss_force_fw_assert(&pci_priv->pci_dev->dev);

	/* IOMMU driver requires -ENOSYS to print debug info. */
	return -ENOSYS;
}

static
void cnss_register_iommu_fault_handler(struct cnss_pci_data *pci_priv)
{
	iommu_set_fault_handler(pci_priv->iommu_domain,
				cnss_pci_smmu_fault_handler, pci_priv);
}
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
int cnss_pci_get_iommu_addr(struct cnss_pci_data *pci_priv,
			    struct device_node *iommu_group_node)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	struct device_node *of_node;
	const u32 *maps;
	const u32 *end;
	int size;

	of_node = of_find_node_by_name(pci_dev->dev.of_node,
				       "cnss_pci0_iommu_region_partition");
	if (!of_node)
		return -EINVAL;

	maps = of_get_property(of_node, "iommu-addresses", &size);
	if (!maps) {
		of_node_put(of_node);
		return -EINVAL;
	}

	end = maps + size / sizeof(u32);

	pci_priv->smmu_iova_start = 0;

	while (maps < end) {
		phys_addr_t iova;
		size_t length;

		/*
		 * Skip the device phandle and if required later, we can
		 * check if the device phandle matches with pci_dev->dev.of_node
		 */
		maps++;

		maps = of_translate_dma_region(pci_dev->dev.of_node, maps,
					       &iova, &length);

		/*
		 * Assuming a single contiguous DMA address range
		 */
		if (!pci_priv->smmu_iova_start)
			pci_priv->smmu_iova_start = length;
		else
			pci_priv->smmu_iova_len =
					iova - pci_priv->smmu_iova_start;
	}

	of_node_put(of_node);

	return (pci_priv->smmu_iova_start && pci_priv->smmu_iova_len) ?
		0 : -EINVAL;
}
#else
int cnss_pci_get_iommu_addr(struct cnss_pci_data *pci_priv,
			    struct device_node *iommu_group_node)
{
	u32 addr_win[2];
	int ret;

	ret = of_property_read_u32_array(iommu_group_node,
					 "qcom,iommu-dma-addr-pool",
					 addr_win, ARRAY_SIZE(addr_win));

	pci_priv->smmu_iova_start = addr_win[0];
	pci_priv->smmu_iova_len = addr_win[1];

	return ret;
}
#endif

int cnss_pci_init_smmu(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	struct cnss_plat_data *plat_priv = pci_priv->plat_priv;
	struct device_node *of_node;
	struct resource *res;
	const char *iommu_dma_type;
	int ret = 0;

	of_node = of_parse_phandle(pci_dev->dev.of_node, "qcom,iommu-group", 0);
	if (!of_node)
		return ret;

	cnss_pr_dbg("Initializing SMMU\n");

	pci_priv->iommu_domain = iommu_get_domain_for_dev(&pci_dev->dev);
	ret = of_property_read_string(of_node, "qcom,iommu-dma",
				      &iommu_dma_type);
	if (!ret && !strcmp("fastmap", iommu_dma_type)) {
		cnss_pr_dbg("Enabling SMMU S1 stage\n");
		pci_priv->smmu_s1_enable = true;
		cnss_register_iommu_fault_handler(pci_priv);
		cnss_register_iommu_fault_handler_irq(pci_priv);
	}

	ret = cnss_pci_get_iommu_addr(pci_priv, of_node);
	if (ret) {
		cnss_pr_err("Invalid SMMU size window, err = %d\n", ret);
		of_node_put(of_node);
		return ret;
	}

	cnss_pr_dbg("smmu_iova_start: %pa, smmu_iova_len: 0x%zx\n",
		    &pci_priv->smmu_iova_start,
		    pci_priv->smmu_iova_len);

	res = platform_get_resource_byname(plat_priv->plat_dev, IORESOURCE_MEM,
					   "smmu_iova_ipa");
	if (res) {
		pci_priv->smmu_iova_ipa_start = res->start;
		pci_priv->smmu_iova_ipa_current = res->start;
		pci_priv->smmu_iova_ipa_len = resource_size(res);
		cnss_pr_dbg("smmu_iova_ipa_start: %pa, smmu_iova_ipa_len: 0x%zx\n",
			    &pci_priv->smmu_iova_ipa_start,
			    pci_priv->smmu_iova_ipa_len);
	}

	pci_priv->iommu_geometry = of_property_read_bool(of_node,
							 "qcom,iommu-geometry");
	cnss_pr_dbg("iommu_geometry: %d\n", pci_priv->iommu_geometry);

	of_node_put(of_node);

	return 0;
}

int _cnss_pci_get_reg_dump(struct cnss_pci_data *pci_priv,
			   u8 *buf, u32 len)
{
	if (cnss_is_fw_managed_pwr(pci_priv))
		return 0;
	else
		return msm_pcie_reg_dump(pci_priv->pci_dev, buf, len);
}

struct cnss_sw_reset_reg_params reset_reg_params = {
	.pcie_txvecdb = 0x360,
	.pcie_txvecstatus = 0x368,
	.pcie_rxvecdb = 0x394,
	.pcie_rxvecstatus = 0x39c,
	.pcie_parf_ltssm = 0x1e081b0,
	.ltssm_value = 0x111,
	.pcie_int_all_clear = 0x1e08228,
	.pcie_int_clear_all = 0xffffffff,
	.wlaon_qfprom_pwr_ctrl_reg = 0x01f8031c,
	.qfprom_pwr_ctrl_vdd4blow_mask = 0x4,
	.wlaon_warm_sw_entry = 0x1f80504,
	.wlaon_soc_reset_cause_reg = 0x01f8060c,
	.pcie_q6_cookie_addr = 0x01f80500,
	.pcie_soc_global_reset = 0x3008,
	.pcie_soc_global_reset_v = 0x1,
	.mhistatus = 0x48,
	.mhictrl = 0x38,
	.mhictrl_reset_mask = 0x2,
};

void cnss_init_sw_reset_params(struct cnss_pci_data *pci_priv)
{
	if (!cnss_is_fw_managed_pwr(pci_priv))
		return;

	switch (pci_priv->pci_dev->device) {
	case QCA6390_DEVICE_ID:
	case QCA6490_DEVICE_ID:
	case KIWI_DEVICE_ID:
		pci_priv->reset_regs = &reset_reg_params;
		break;
	default:
		cnss_pr_err("Not support get device 0x%x reset reg params",
			    pci_priv->pci_dev->device);
		pci_priv->reset_regs = NULL;
		return;
	}

	cnss_pr_info("init reset regs for device 0x%x\n",
		     pci_priv->pci_dev->device);

	return;
}

static void cnss_mhi_reset_txvecdb(struct cnss_pci_data *pci_priv)
{
	int ret;
	unsigned int offset;

	offset = pci_priv->reset_regs->pcie_txvecdb;
	ret = cnss_pci_reg_write(pci_priv, offset, 0);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register 0x%x, err %d\n",
			    0, offset, ret);
		return;
	}
}

static void cnss_mhi_reset_txvecstatus(struct cnss_pci_data *pci_priv)
{
	int ret;
	unsigned int offset;

	offset = pci_priv->reset_regs->pcie_txvecstatus;
	ret = cnss_pci_reg_write(pci_priv, offset, 0);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register 0x%x, err %d\n",
			    0, offset, ret);
		return;
	}
}

static void cnss_mhi_reset_rxvecdb(struct cnss_pci_data *pci_priv)
{
	int ret;
	unsigned int offset;

	offset = pci_priv->reset_regs->pcie_rxvecdb;
	ret = cnss_pci_reg_write(pci_priv, offset, 0);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register 0x%x, err %d\n",
			    0, offset, ret);
		return;
	}
}

static void cnss_mhi_reset_rxvecstatus(struct cnss_pci_data *pci_priv)
{
	int ret;
	unsigned int offset;

	offset = pci_priv->reset_regs->pcie_rxvecstatus;
	ret = cnss_pci_reg_write(pci_priv, offset, 0);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register 0x%x, err %d\n",
			    0, offset, ret);
		return;
	}
}

static void cnss_mhi_clear_vector(struct cnss_pci_data *pci_priv)
{
	cnss_mhi_reset_txvecdb(pci_priv);
	cnss_mhi_reset_txvecstatus(pci_priv);
	cnss_mhi_reset_rxvecdb(pci_priv);
	cnss_mhi_reset_rxvecstatus(pci_priv);
}

static void cnss_pci_enable_ltssm(struct cnss_pci_data *pci_priv)
{
	unsigned int val;
	int i, ret;
	unsigned int ltssm_offset, ltssm_val;

	ltssm_offset = pci_priv->reset_regs->pcie_parf_ltssm;
	ltssm_val = pci_priv->reset_regs->ltssm_value;

	cnss_pci_reg_read(pci_priv, ltssm_offset, &val);

	/* PCIE link seems very unstable after the Hot Reset*/
	for (i = 0; val != ltssm_val && i < 5; i++) {
		if (val == 0xffffffff)
			mdelay(5);

		ret = cnss_pci_reg_write(pci_priv, ltssm_offset, ltssm_val);
		if (ret) {
			cnss_pr_err("Failed to write 0x%x to register 0x%x, err %d\n",
				    ltssm_val,
				    ltssm_offset, ret);
			return;
		}
		cnss_pci_reg_read(pci_priv, ltssm_offset, &val);
	}
	cnss_pr_dbg("ltssm val 0x%x\n", val);
}

static void cnss_pci_clear_all_intrs(struct cnss_pci_data *pci_priv)
{
	int ret;
	unsigned int offset, val;

	offset = pci_priv->reset_regs->pcie_int_all_clear;
	val = pci_priv->reset_regs->pcie_int_clear_all;

	ret = cnss_pci_reg_write(pci_priv, offset, val);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register 0x%x, err %d\n",
			    val, offset, ret);
		return;
	}
}

static void cnss_pci_reset_wlaon_pwr_ctrl(struct cnss_pci_data *pci_priv)
{
	unsigned int val;
	int ret;
	unsigned int offset, vdd4blow_mask;

	offset = pci_priv->reset_regs->wlaon_qfprom_pwr_ctrl_reg;
	vdd4blow_mask = pci_priv->reset_regs->qfprom_pwr_ctrl_vdd4blow_mask;

	cnss_pci_reg_read(pci_priv, offset, &val);
	cnss_pr_dbg("wlaon_qfprom_pwr_ctrl_reg val 0x%x\n", val);

	val &= ~vdd4blow_mask;
	ret = cnss_pci_reg_write(pci_priv, offset, val);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register 0x%x, err %d\n",
			    val, offset, ret);
		return;
	}
}

static void cnss_pci_clear_dbg_registers(struct cnss_pci_data *pci_priv)
{
	unsigned int val;
	int ret = 0;
	unsigned int warm_sw_entry, soc_reset_cause_reg, q6_cookie;

	warm_sw_entry = pci_priv->reset_regs->wlaon_warm_sw_entry;
	soc_reset_cause_reg = pci_priv->reset_regs->wlaon_soc_reset_cause_reg;
	q6_cookie = pci_priv->reset_regs->pcie_q6_cookie_addr;

	cnss_pci_reg_read(pci_priv, q6_cookie, &val);
	cnss_pr_dbg("pcie_q6_cookie val 0x%x\n", val);

	cnss_pci_reg_read(pci_priv, warm_sw_entry, &val);
	cnss_pr_dbg("wlaon_warm_sw_entry val 0x%x\n", val);

	/* write 0 to WLAON_WARM_SW_ENTRY to prevent Q6 from
	 * continuing warm path and entering dead loop.
	 */
	ret = cnss_pci_reg_write(pci_priv, warm_sw_entry, 0);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register offset 0x%x, err %d\n",
			    0, warm_sw_entry, ret);
		return;
	}
	mdelay(10);

	cnss_pci_reg_read(pci_priv, warm_sw_entry, &val);
	cnss_pr_dbg("wlaon_warm_sw_entry val 0x%x\n", val);

	/* A read clear register. clear the register to prevent
	 * Q6 from entering wrong code path.
	 */
	cnss_pci_reg_read(pci_priv, soc_reset_cause_reg, &val);
	cnss_pr_dbg("soc_reset_cause_reg val %d\n", val);
}

static void cnss_pci_soc_global_reset(struct cnss_pci_data *pci_priv)
{
	unsigned int val;
	int ret = 0;
	unsigned int soc_global_reset, soc_global_reset_v;

	soc_global_reset = pci_priv->reset_regs->pcie_soc_global_reset;
	soc_global_reset_v = pci_priv->reset_regs->pcie_soc_global_reset_v;

	cnss_pci_reg_read(pci_priv, soc_global_reset, &val);
	cnss_pr_dbg("soc_global_reset val 0x%x\n", val);
	val |= soc_global_reset_v;

	ret = cnss_pci_reg_write(pci_priv, soc_global_reset, val);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register offset 0x%x, err %d\n",
			    val, soc_global_reset, ret);
		return;
	}

	mdelay(10);

	val &= ~soc_global_reset_v;

	ret = cnss_pci_reg_write(pci_priv, soc_global_reset, val);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register offset 0x%x, err %d\n",
			    val, soc_global_reset, ret);
		return;
	}
	mdelay(10);

	cnss_pci_reg_read(pci_priv, soc_global_reset, &val);
	if (val == 0xffffffff)
		cnss_pr_err("link down error during global reset\n");

	cnss_pr_dbg("soc_global_reset final val 0x%x\n", val);
}

static void cnss_mhi_set_mhictrl_reset(struct cnss_pci_data *pci_priv)
{
	unsigned int val;
	int ret = 0;
	unsigned int mhistatus, mhictrl, reset_mask;

	mhistatus = pci_priv->reset_regs->mhistatus;
	mhictrl = pci_priv->reset_regs->mhictrl;
	reset_mask = pci_priv->reset_regs->mhictrl_reset_mask;

	cnss_pci_reg_read(pci_priv, mhistatus, &val);
	cnss_pr_dbg("mhistatus val 0x%x\n", val);

	ret = cnss_pci_reg_write(pci_priv, mhictrl, reset_mask);
	if (ret) {
		cnss_pr_err("Failed to write 0x%x to register offset 0x%x, err = %d\n",
			    reset_mask, mhictrl, ret);
		return;
	}
	mdelay(10);
}

void cnss_pci_sw_reset(struct cnss_pci_data *pci_priv, bool power_on)
{
	if (!cnss_is_fw_managed_pwr(pci_priv))
		return;

	if (pci_priv->reset_regs == NULL) {
		cnss_pr_err("Device 0x%x reset_regs NULL\n",
			    pci_priv->pci_dev->device);
		return;
	}

	if (power_on) {
		cnss_pci_enable_ltssm(pci_priv);
		cnss_pci_clear_all_intrs(pci_priv);
		cnss_pci_reset_wlaon_pwr_ctrl(pci_priv);
	}

	cnss_mhi_clear_vector(pci_priv);
	cnss_pci_clear_dbg_registers(pci_priv);
	cnss_pci_soc_global_reset(pci_priv);
	cnss_mhi_set_mhictrl_reset(pci_priv);
	return;
}
