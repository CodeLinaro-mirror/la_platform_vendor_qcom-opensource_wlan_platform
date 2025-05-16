// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved. */

#include "pci_platform.h"
#include "debug.h"

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

int _cnss_pci_enumerate(struct cnss_plat_data *plat_priv, u32 rc_num)
{
	return 0;
}

bool cnss_pci_is_sync_probe(void)
{
	return false;
}

int cnss_pci_assert_perst(struct cnss_pci_data *pci_priv)
{
	return -EOPNOTSUPP;
}

int cnss_pci_disable_pc(struct cnss_pci_data *pci_priv, bool vote)
{
	return 0;
}

int cnss_pci_set_link_bandwidth(struct cnss_pci_data *pci_priv,
				u16 link_speed, u16 link_width)
{
	return 0;
}

int cnss_pci_set_max_link_speed(struct cnss_pci_data *pci_priv,
				u32 rc_num, u16 link_speed)
{
	return 0;
}

int cnss_reg_pci_event(struct cnss_pci_data *pci_priv)
{
	return 0;
}

void cnss_dereg_pci_event(struct cnss_pci_data *pci_priv) {}

int cnss_wlan_adsp_pc_enable(struct cnss_pci_data *pci_priv, bool control)
{
	return 0;
}

int cnss_set_pci_link(struct cnss_pci_data *pci_priv, bool link_up)
{
	return 0;
}

int cnss_pci_prevent_l1(struct device *dev)
{
	return 0;
}
EXPORT_SYMBOL(cnss_pci_prevent_l1);

void cnss_pci_allow_l1(struct device *dev)
{
}
EXPORT_SYMBOL(cnss_pci_allow_l1);

int _cnss_pci_get_reg_dump(struct cnss_pci_data *pci_priv,
			   u8 *buf, u32 len)
{
	return 0;
}

void cnss_pci_update_drv_supported(struct cnss_pci_data *pci_priv)
{
	pci_priv->drv_supported = false;
}

int cnss_pci_get_msi_assignment(struct cnss_pci_data *pci_priv)
{
	pci_priv->msi_config = &msi_config;

	return 0;
}

#ifdef CONFIG_ONE_MSI_VECTOR
/**
 * All the user share the same vector and msi data
 * For MHI user, we need pass IRQ array information to MHI component
 * MHI_IRQ_NUMBER is defined to specify this MHI IRQ array size
 */
#define MHI_IRQ_NUMBER 3
static struct cnss_msi_config msi_config_one_msi = {
	.total_vectors = 1,
	.total_users = 4,
	.users = (struct cnss_msi_user[]) {
		{ .name = "MHI", .num_vectors = 1, .base_vector = 0 },
		{ .name = "CE", .num_vectors = 1, .base_vector = 0 },
		{ .name = "WAKE", .num_vectors = 1, .base_vector = 0 },
		{ .name = "DP", .num_vectors = 1, .base_vector = 0 },
	},
};

int cnss_pci_get_one_msi_assignment(struct cnss_pci_data *pci_priv)
{
	pci_priv->msi_config = &msi_config_one_msi;

	return 0;
}

bool cnss_pci_fallback_one_msi(struct cnss_pci_data *pci_priv,
			       int *num_vectors)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	struct cnss_msi_config *msi_config;

	cnss_pci_get_one_msi_assignment(pci_priv);
	msi_config = pci_priv->msi_config;
	if (!msi_config) {
		cnss_pr_err("one msi_config is NULL!\n");
		return false;
	}
	*num_vectors = pci_alloc_irq_vectors(pci_dev,
					     msi_config->total_vectors,
					     msi_config->total_vectors,
					     PCI_IRQ_MSI);
	if (*num_vectors < 0) {
		cnss_pr_err("Failed to get one MSI vector!\n");
		return false;
	}
	cnss_pr_dbg("request MSI one vector\n");

	return true;
}

bool cnss_pci_is_one_msi(struct cnss_pci_data *pci_priv)
{
	return pci_priv && pci_priv->msi_config &&
	       (pci_priv->msi_config->total_vectors == 1);
}

int cnss_pci_get_one_msi_mhi_irq_array_size(struct cnss_pci_data *pci_priv)
{
	return MHI_IRQ_NUMBER;
}

bool cnss_pci_is_force_one_msi(struct cnss_pci_data *pci_priv)
{
	struct cnss_plat_data *plat_priv = pci_priv->plat_priv;

	return test_bit(FORCE_ONE_MSI, &plat_priv->ctrl_params.quirks);
}
#else
int cnss_pci_get_one_msi_assignment(struct cnss_pci_data *pci_priv)
{
	return 0;
}

bool cnss_pci_fallback_one_msi(struct cnss_pci_data *pci_priv,
			       int *num_vectors)
{
	return false;
}

bool cnss_pci_is_one_msi(struct cnss_pci_data *pci_priv)
{
	return false;
}

int cnss_pci_get_one_msi_mhi_irq_array_size(struct cnss_pci_data *pci_priv)
{
	return 0;
}

bool cnss_pci_is_force_one_msi(struct cnss_pci_data *pci_priv)
{
	return false;
}
#endif

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

int cnss_pci_init_smmu(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	struct cnss_plat_data *plat_priv = pci_priv->plat_priv;
	struct device_node *of_node;
	struct resource *res;
	const char *iommu_dma_type;
	u32 addr_win[2];
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
		iommu_set_fault_handler(pci_priv->iommu_domain,
					cnss_pci_smmu_fault_handler, pci_priv);
		cnss_register_iommu_fault_handler_irq(pci_priv);
	}

	ret = of_property_read_u32_array(of_node,  "qcom,iommu-dma-addr-pool",
					 addr_win, ARRAY_SIZE(addr_win));
	if (ret) {
		cnss_pr_err("Invalid SMMU size window, err = %d\n", ret);
		of_node_put(of_node);
		return ret;
	}

	pci_priv->smmu_iova_start = addr_win[0];
	pci_priv->smmu_iova_len = addr_win[1];
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

void cnss_init_sw_reset_params(struct cnss_pci_data *pci_priv)
{
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
