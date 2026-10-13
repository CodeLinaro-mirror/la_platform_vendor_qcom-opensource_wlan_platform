// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sd.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/of_gpio.h>
#include "qcn_sdio.h"
#include <soc/qcom/sdhci-msm.h>
#include <linux/ipc_logging.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#define SDIO_CCCR_INTERRUPT_EXTENSION  0x16
#define SDIO_SUPPORT_ASYNC_INTR        (1<<0)
#define SDIO_ENABLE_ASYNC_INTR         (1<<1)
#define	QCN_IPC_LOG_PAGES		32

static bool tx_dump;
module_param(tx_dump, bool, S_IRUGO | S_IWUSR | S_IWGRP);

static bool rx_dump;
module_param(rx_dump, bool, S_IRUGO | S_IWUSR | S_IWGRP);

static int dump_len = 32;
module_param(dump_len, int, S_IRUGO | S_IWUSR | S_IWGRP);

#define qcn_pr_info(_msg, ...) \
	do { \
		if (ipc_log_ctxt) \
			ipc_log_string(ipc_log_ctxt, "[%s] " _msg, __func__, ##__VA_ARGS__); \
		else \
			pr_info("[%s] " _msg, __func__, ##__VA_ARGS__); \
	} while (0)

#define qcn_pr_err(_msg, ...) pr_err("[%s] " _msg, __func__, ##__VA_ARGS__);

/* driver_state :
 *	QCN_SDIO_SW_RESET = 0,
 *	QCN_SDIO_SW_PBL,
 *	QCN_SDIO_SW_SBL,
 *	QCN_SDIO_SW_RDDM,
 *	QCN_SDIO_SW_MROM,
*/
static int driver_state;
module_param(driver_state, int, S_IRUGO | S_IRUSR | S_IRGRP);

static struct mmc_host *current_host;

#define HEX_DUMP(mode, buf, len)				\
	print_hex_dump(KERN_ERR, mode, 2, 32, 4, buf,		\
			dump_len > len ? len : dump_len, 0)

struct qcn_sdio_gpio_info {
	int wake_gpio;
	int wake_irq;
};

static struct qcn_sdio_gpio_info *sdio_gpio_info;

struct qcn_sdio {
	enum qcn_sdio_sw_mode curr_sw_mode;
	struct sdio_func *func;
	const struct sdio_device_id *id;
	struct qcn_sdio_ch_info *ch[QCN_SDIO_CH_MAX];
	atomic_t ch_status[QCN_SDIO_CH_MAX];
	spinlock_t lock_free_q;
	spinlock_t lock_wait_q;
	spinlock_t lock_comp_q;
	u32 rx_addr_base;
	u32 tx_addr_base;
	u8 rx_cnum_base;
	u8 tx_cnum_base;
	struct qcn_sdio_rw_info rw_req_info[QCN_SDIO_RW_REQ_MAX];
	struct list_head rw_free_q;
	struct list_head rw_wait_q;
	struct list_head rw_comp_q;
	atomic_t free_list_count;
	atomic_t wait_list_count;
	struct workqueue_struct *qcn_sdio_gpio_irq_wq;
	struct work_struct qcn_sdio_gpio_irq_w;
	bool pending_crq_ch_1;
	struct task_struct *rw_thread;
	struct semaphore sem_rw_thread;
	int rw_shutdown;
	struct task_struct *rw_comp_thread;
	struct semaphore sem_rw_comp_thread;
	int rw_comp_shutdown;
	atomic_t oob_wake_irq;
	struct dentry *debugfs_dir;
	struct qcn_sdio_rw_history_entry rw_history[QCN_SDIO_RW_HISTORY_MAX];
	u32 rw_history_idx;
	spinlock_t lock_history;
};

static struct qcn_sdio *sdio_ctxt;
struct completion client_probe_complete;
struct completion meson_suspend_allowed;
struct completion meson_resume_complete;
static struct mutex lock;
static struct list_head cinfo_head;
static atomic_t status;
static atomic_t xport_status;
static atomic_t meson_card_state;
static atomic_t meson_suspended;
static spinlock_t async_lock;
static void *ipc_log_ctxt;
static void qcn_sdio_fw_assert(struct work_struct *work);
static DECLARE_WORK(qcn_sdio_fw_assert_work, qcn_sdio_fw_assert);
#ifndef CONFIG_SDIO_QCN
static struct task_struct *reset_task;
#endif
static bool htc_ready;

static int qcn_create_sysfs(struct device *dev);
static void qcn_sdio_debugfs_create(void);
static void qcn_sdio_debugfs_destroy(void);

#if (QCN_SDIO_META_VER_0)
#define	META_INFO(event, data)						  \
	((u32)((u32)data << QCN_SDIO_HMETA_DATA_SHFT) |			  \
	(u32)(((u32)event << QCN_SDIO_HMETA_EVENT_SHFT) &		  \
	QCN_SDIO_HMETA_EVENT_BMSK) | (u32)(((u32)(sdio_ctxt->curr_sw_mode)\
	<< QCN_SDIO_HMETA_SW_SHFT) & QCN_SDIO_HMETA_SW_BMSK) |		  \
	(u32)(QCN_SDIO_HMETA_FMT_VER & QCN_SDIO_HMETA_VER_BMSK))
#elif (QCN_SDIO_META_VER_1)
#define	META_INFO(even, data)						  \
	((u32)(((u32)event << QCN_SDIO_HMETA_EVENT_SHFT) &		  \
	QCN_SDIO_HMETA_EVENT_BMSK) | (u32)(((u32)data <<		  \
	QCN_SDIO_HMETA_DATA_SHFT) & QCN_SDIO_HMETA_DATA_BMSK))
#elif (QCN_SDIO_META_VER_2)
#define	META_INFO(even, data)						  \
	((u32)(((u32)event << QCN_SDIO_HMETA_EVENT_SHFT) &		  \
	QCN_SDIO_HMETA_EVENT_BMSK) | (u32)(((u32)data <<		  \
	QCN_SDIO_HMETA_DATA_SHFT) & (QCN_SDIO_HMETA_DATA_BMSK |		  \
	SDIO_QCN_HRQ_PUSH_BLK_MASK)))
#endif

#define	SDIO_RW_OFFSET		31
#define	SDIO_RW_MASK		1
#define	SDIO_FUNCTION_OFFSET	28
#define	SDIO_FUNCTION_MASK	7
#define	SDIO_MODE_OFFSET	27
#define	SDIO_MODE_MASK		1
#define	SDIO_OPCODE_OFFSET	26
#define	SDIO_OPCODE_MASK	1
#define	SDIO_ADDRESS_OFFSET	9
#define	SDIO_ADDRESS_MASK	0x1FFFF
#define	SDIO_RAW_OFFSET		27
#define	SDIO_RAW_MASK		1
#define	SDIO_STUFF_OFFSET1	26
#define	SDIO_STUFF_OFFSET2	8
#define	SDIO_STUFF_MASK		1
#define	SDIO_BLOCKSZ_MASK	0x1FF
#define	SDIO_DATA_MASK		0xFF
#define QCN_SDIO_LP_LHI_LEVENT_TIMEOUT 100

static inline
void qcn_sdio_set_cmd53_arg(u32 *arg, u8 rw, u8 func, u8 mode, u8 opcode,
							u32 addr, u16 blksz)
{
	*arg = (((rw & SDIO_RW_MASK) << SDIO_RW_OFFSET) |
		((func & SDIO_FUNCTION_MASK) << SDIO_FUNCTION_OFFSET) |
		((mode & SDIO_MODE_MASK) << SDIO_MODE_OFFSET) |
		((opcode & SDIO_OPCODE_MASK) << SDIO_OPCODE_OFFSET) |
		((addr & SDIO_ADDRESS_MASK) << SDIO_ADDRESS_OFFSET) |
		(blksz & SDIO_BLOCKSZ_MASK));
}

static inline
void qcn_sdio_set_cmd52_arg(u32 *arg, u8 rw, u8 func, u8 raw, u32 addr, u8 val)
{
	*arg = ((rw & SDIO_RW_MASK) << SDIO_RW_OFFSET) |
		((func & SDIO_FUNCTION_MASK) << SDIO_FUNCTION_OFFSET) |
		((raw & SDIO_RAW_MASK) << SDIO_RAW_OFFSET) |
		(SDIO_STUFF_MASK << SDIO_STUFF_OFFSET1) |
		((addr & SDIO_ADDRESS_MASK) << SDIO_ADDRESS_OFFSET) |
		(SDIO_STUFF_MASK << SDIO_STUFF_OFFSET2) |
		(val & SDIO_DATA_MASK);
}

static void qcn_sdio_free_rw_req(struct qcn_sdio_rw_info *rw_req)
{
	spin_lock_bh(&sdio_ctxt->lock_free_q);
	list_add_tail(&rw_req->list, &sdio_ctxt->rw_free_q);
	atomic_inc(&sdio_ctxt->free_list_count);
	spin_unlock_bh(&sdio_ctxt->lock_free_q);
}

static void qcn_sdio_add_rw_comp_req(struct qcn_sdio_rw_comp_info *rw_comp)
{
	spin_lock_bh(&sdio_ctxt->lock_comp_q);
	list_add_tail(&rw_comp->list, &sdio_ctxt->rw_comp_q);
	spin_unlock_bh(&sdio_ctxt->lock_comp_q);
}

void qcn_sdio_purge_rw_buff(void)
{
	struct qcn_sdio_rw_info *rw_req = NULL;

	if (sdio_ctxt == NULL)
		return;

	spin_lock_bh(&sdio_ctxt->lock_wait_q);
	while (!list_empty(&sdio_ctxt->rw_wait_q)) {
		rw_req = list_first_entry(&sdio_ctxt->rw_wait_q,
						struct qcn_sdio_rw_info, list);
		list_del(&rw_req->list);
		atomic_dec(&sdio_ctxt->wait_list_count);
		spin_unlock_bh(&sdio_ctxt->lock_wait_q);
		qcn_sdio_free_rw_req(rw_req);
		spin_lock_bh(&sdio_ctxt->lock_wait_q);
	}
	spin_unlock_bh(&sdio_ctxt->lock_wait_q);
}

EXPORT_SYMBOL(qcn_sdio_purge_rw_buff);

static void qcn_sdio_purge_rw_comp_buff(void)
{
	struct qcn_sdio_rw_comp_info *rw_comp = NULL;

	if (sdio_ctxt == NULL)
		return;

	spin_lock_bh(&sdio_ctxt->lock_comp_q);
	while (!list_empty(&sdio_ctxt->rw_comp_q)) {
		rw_comp = list_first_entry(&sdio_ctxt->rw_comp_q,
					struct qcn_sdio_rw_comp_info, list);
		list_del(&rw_comp->list);
		spin_unlock_bh(&sdio_ctxt->lock_comp_q);
		kfree(rw_comp);
		spin_lock_bh(&sdio_ctxt->lock_comp_q);
	}
	spin_unlock_bh(&sdio_ctxt->lock_comp_q);
}

void qcn_sdio_irq_handler(struct sdio_func *func);

void qcn_sdio_client_probe_complete(int id)
{
	complete(&client_probe_complete);
}
EXPORT_SYMBOL(qcn_sdio_client_probe_complete);

static struct qcn_sdio_rw_info *qcn_sdio_alloc_rw_req(void)
{
	struct qcn_sdio_rw_info *rw_req = NULL;

	spin_lock_bh(&sdio_ctxt->lock_free_q);
	if (list_empty(&sdio_ctxt->rw_free_q)) {
		spin_unlock_bh(&sdio_ctxt->lock_free_q);
		return rw_req;
	}

	rw_req = list_first_entry(&sdio_ctxt->rw_free_q,
						struct qcn_sdio_rw_info, list);
	list_del(&rw_req->list);
	atomic_dec(&sdio_ctxt->free_list_count);
	spin_unlock_bh(&sdio_ctxt->lock_free_q);

	rw_req->ts_irq = 0;
	rw_req->ts_queued = 0;
	rw_req->ts_dequeued = 0;
	rw_req->ts_xfer_done = 0;
	rw_req->ts_comp_queued = 0;
	rw_req->ts_comp_done = 0;

	return rw_req;
}

static void qcn_sdio_add_rw_req(struct qcn_sdio_rw_info *rw_req)
{
	spin_lock_bh(&sdio_ctxt->lock_wait_q);
	list_add_tail(&rw_req->list, &sdio_ctxt->rw_wait_q);
	atomic_inc(&sdio_ctxt->wait_list_count);
	spin_unlock_bh(&sdio_ctxt->lock_wait_q);
}

static int qcn_enable_async_irq(bool enable)
{
	unsigned int num = 0;
	int ret = 0;
	u32 data = 0;

	num = sdio_ctxt->func->num;
	sdio_claim_host(sdio_ctxt->func);
	sdio_ctxt->func->num = 0;
	data = sdio_readb(sdio_ctxt->func, SDIO_CCCR_INTERRUPT_EXTENSION, NULL);
	if (enable)
		data |= SDIO_ENABLE_ASYNC_INTR;
	else
		data &= ~SDIO_ENABLE_ASYNC_INTR;
	sdio_writeb(sdio_ctxt->func, data, SDIO_CCCR_INTERRUPT_EXTENSION, &ret);
	sdio_ctxt->func->num = num;
	sdio_release_host(sdio_ctxt->func);

	return ret;
}

static int qcn_send_io_abort(void)
{
	unsigned int num = 0;
	int ret = 0;

	num = sdio_ctxt->func->num;
	sdio_claim_host(sdio_ctxt->func);
	sdio_ctxt->func->num = 0;
	sdio_writeb(sdio_ctxt->func, 0x1, SDIO_CCCR_ABORT, &ret);
	sdio_ctxt->func->num = num;
	sdio_release_host(sdio_ctxt->func);

	return ret;
}

int sdio_al_send_host_trans_len(unsigned int data_len)
{
	int ret = 0;
	sdio_claim_host(sdio_ctxt->func);
	sdio_writel(sdio_ctxt->func, data_len, SDIO_QCN_HOST_TRANS_REG0, &ret);
	sdio_release_host(sdio_ctxt->func);
	return ret;
}
EXPORT_SYMBOL(sdio_al_send_host_trans_len);

static int change_oob_mask(void)
{
	int ret = 0;
	u8 data = 0;

	sdio_claim_host(sdio_ctxt->func);

	data = sdio_readb(sdio_ctxt->func, SDIO_QCN_CONFIG, &ret);
	if (ret) {
		qcn_pr_err("IRQ status read error ret = %d\n", ret);
		goto out;
	}

	data = data ^ (u8)SDIO_QCN_CONFIG_OOB_MASK;

	sdio_writeb(sdio_ctxt->func, data, SDIO_QCN_CONFIG, &ret);
	if (ret) {
		qcn_pr_err("IRQ status write error ret = %d\n", ret);
		goto out;
	}

out:
	sdio_release_host(sdio_ctxt->func);
	return ret;
}

static int enable_oob_interupts(void)
{
	int ret = 0;

	ret = change_oob_mask();
	if (ret)
		return ret;

	sdio_claim_host(sdio_ctxt->func);
	sdio_release_irq(sdio_ctxt->func);
	sdio_release_host(sdio_ctxt->func);

	return 0;
}

static int disable_oob_interupts(void)
{
	int ret = 0;

	sdio_claim_host(sdio_ctxt->func);

	ret = sdio_claim_irq(sdio_ctxt->func, qcn_sdio_irq_handler);
	if (ret)
		qcn_pr_err("Error:%d SDIO claim irq\n", ret);

	sdio_release_host(sdio_ctxt->func);

	ret = change_oob_mask();
	if (ret)
		return ret;

	return ret;
}

static int qcn_send_lpm_indication(void)
{
	int ret = 0;

	sdio_claim_host(sdio_ctxt->func);
	sdio_writeb(sdio_ctxt->func, 0x1, SDIO_QCN_LOW_PWR, &ret);
	sdio_release_host(sdio_ctxt->func);

	if (ret)
		qcn_pr_err("error: %d, while sending cmd 52\n", ret);

	return ret;
}

static void qcn_sdio_fw_assert(struct work_struct *work)
{
	struct qcn_sdio_client_info *cinfo = NULL;
	struct device *dev = NULL;

	if (!sdio_ctxt || !sdio_ctxt->func) {
		qcn_pr_err("Invalid sdio_ctxt\n");
		return;
	}

	dev = &sdio_ctxt->func->dev;

	mutex_lock(&lock);
	list_for_each_entry(cinfo, &cinfo_head, cli_list) {
	mutex_unlock(&lock);
		if (cinfo->cli_data.fw_assert_cb) {
			cinfo->cli_data.fw_assert_cb(dev);
		}
		mutex_lock(&lock);
	}
	mutex_unlock(&lock);
}

static int qcn_sdio_pm(int lpm_event)
{
	struct qcn_sdio_client_info *cinfo = NULL;
	int ret = 0;

	mutex_lock(&lock);
	list_for_each_entry(cinfo, &cinfo_head, cli_list) {
	mutex_unlock(&lock);
		if (cinfo->cli_data.lpm_notify_cb) {
			ret = cinfo->cli_data.lpm_notify_cb(&cinfo->cli_handle, lpm_event);
			if (ret)
				goto exit;
		}
		mutex_lock(&lock);
	}
	mutex_unlock(&lock);

exit:
	return ret;
}

static int qcn_sdio_suspend(struct device *dev)
{
	int ret = 0;

	if (!atomic_read(&meson_card_state)) {
		ret = sdio_set_host_pm_flags(sdio_ctxt->func,
					     MMC_PM_KEEP_POWER);
		if (ret) {
			qcn_pr_err("Failed to set PM flags: %d\n", ret);
			return ret;
		}
		return 0;
	}

	reinit_completion(&meson_suspend_allowed);

	ret = qcn_sdio_pm(LPM_ENTER);
	if (ret) {
		qcn_pr_err("Failed to suspend\n");
		return ret;
	}

	sdio_set_host_pm_flags(sdio_ctxt->func, MMC_PM_KEEP_POWER);

	ret = enable_oob_interupts();
	if (ret)
		goto resume;

	ret = qcn_send_lpm_indication();
	if (ret)
		goto disable_oob;

	ret = wait_for_completion_timeout(&meson_suspend_allowed,
			msecs_to_jiffies(QCN_SDIO_LP_LHI_LEVENT_TIMEOUT));
	if (!ret) {
		qcn_pr_err("Didn't get allow response from Meson\n");
		goto disable_oob;
	}
	atomic_set(&meson_suspended, 1);
	reinit_completion(&meson_resume_complete);

	return 0;

disable_oob:
	disable_oob_interupts();
resume:
	qcn_sdio_pm(LPM_EXIT);
	return -EBUSY;
}

static int qcn_sdio_resume(struct device *dev)
{
	int ret, try_cnt = 0, MAX_RETRY_CNT = 3;

	if (!atomic_read(&meson_card_state)) {
		return 0;
	}

	atomic_set(&meson_suspended, 0);
	qcn_pr_info("pid %d", !in_interrupt() ? current->pid : 0);

	/* If Resume is FW triggered, we would have got OOB INTR by now.
	 * Disable SDIO LPM mode and then wait for LP LHI event over
	 * normal SDIO INTR path.
	 */
	if (atomic_read(&sdio_ctxt->oob_wake_irq)) {
		qcn_pr_info("No need to toggle DAT1, OOB intr was received\n");
		disable_oob_interupts();
		atomic_set(&sdio_ctxt->oob_wake_irq, 0);
		enable_irq(sdio_gpio_info->wake_irq);
		ret = wait_for_completion_timeout(
			&meson_resume_complete,
			msecs_to_jiffies(QCN_SDIO_LP_LHI_LEVENT_TIMEOUT));
		if (!ret) {
			qcn_pr_err("Failed to get LHI resumed event\n");
			return 0;
		} else {
			goto resume;
		}
	}

	/* Resume Triggered by APPS:
	 * 1. Wakeup FW by toggling the DAT1 line, FW will send the OOB INTR
	 * 2. Read SDIO IRQ register in OOB INTR handler to get LP LHI event
	 * 3. Disable the SDIO LPM mode once LP LHI Resumed event is received.
	 */
	while (try_cnt++ < MAX_RETRY_CNT) {
		sdio_claim_host(sdio_ctxt->func);
		qcn_pr_info("toggling DAT1 GPIO pin\n");
		sdhci_msm_toggle_dat1_gpio(sdio_ctxt->func);
		sdio_release_host(sdio_ctxt->func);

		ret = wait_for_completion_timeout(
			&meson_resume_complete,
			msecs_to_jiffies(QCN_SDIO_LP_LHI_LEVENT_TIMEOUT));
		if (!ret) {
			if (try_cnt < MAX_RETRY_CNT) {
					qcn_pr_err("Failed to resume Meson, Retrying\n");
					continue;
			}
			qcn_pr_err("Failed to resume Meson after %d attempts\n", MAX_RETRY_CNT);
			schedule_work(&qcn_sdio_fw_assert_work);
			return -ETIMEDOUT;
		}
		qcn_pr_info("Meson resumed after %d try\n", try_cnt);
		disable_oob_interupts();
		break;
	}

resume:
	return qcn_sdio_pm(LPM_EXIT);
}

static int qcn_sdio_runtime_suspend(struct device *dev)
{
	return qcn_sdio_pm(LPM_RUNTIME_SUSPEND_ENTER);
}

static int qcn_sdio_runtime_resume(struct device *dev)
{
	return qcn_sdio_pm(LPM_RUNTIME_SUSPEND_EXIT);
}

static int qcn_sdio_runtime_idle(struct device *dev)
{
	return 0;
}

static int qcn_send_meta_info(u8 event, u32 data)
{
	int ret = 0;
	u32 value = 0;

	value =	META_INFO(event, data);

	sdio_claim_host(sdio_ctxt->func);
		sdio_writel(sdio_ctxt->func, value, SDIO_QCN_HRQ_PUSH, &ret);

	sdio_release_host(sdio_ctxt->func);

	return ret;
}

static int qcn_read_crq_info(void)
{
	int ret = 0;
	u32 temp = 0;
	u32 data = 0;
	u32 len = 0;
	u8 cid = 0;

	struct sdio_al_channel_handle *ch_handle = NULL;
	struct qcn_sdio_rw_info *rw_req = NULL;

	sdio_claim_host(sdio_ctxt->func);
		data = sdio_readl(sdio_ctxt->func, SDIO_QCN_CRQ_PULL, &ret);

	sdio_release_host(sdio_ctxt->func);
	if (ret)
		return ret;

	if (data & SDIO_QCN_CRQ_PULL_TRANS_MASK) {
		cid = (u8)(data & SDIO_QCN_CRQ_PULL_CH_NUM_MASK);
		cid -= sdio_ctxt->rx_cnum_base;
		len = (data & SDIO_QCN_CRQ_PULL_BLK_CNT_MASK) >>
			SDIO_QCN_CRQ_PULL_BLK_CNT_SHIFT;

		if (data & SDIO_QCN_CRQ_PULL_BLK_MASK)
			len *= sdio_ctxt->func->cur_blksize;
		temp = (data & SDIO_QCN_CRQ_PULL_UD_MASK) >>
						SDIO_QCN_CRQ_PULL_UD_SHIFT;

		if (cid == 0)
			qcn_pr_info("CRQ received for cid %d len %d\n", cid, len);

		if (cid == 1)
			htc_ready = true;

		if (!sdio_ctxt->ch[cid]) {
			qcn_pr_err("Client Id not initialized %d\n", cid);
			if (cid != QCN_SDIO_CH_1)
				return -EINVAL;

			qcn_pr_err("Add CRQ request len %d type 0x%x", len, temp);
			rw_req = qcn_sdio_alloc_rw_req();
			if (!rw_req)
				return -ENOMEM;

			rw_req->cid = cid;
			rw_req->dir = SDIO_AL_RX_AVBL;
			rw_req->buf = NULL;
			rw_req->len = len;
			rw_req->ctxt = NULL;

			qcn_sdio_add_rw_req(rw_req);
			sdio_ctxt->pending_crq_ch_1 = true;

			return -EINVAL;
		}

		switch (temp) {
		case QCN_SDIO_CRQ_START:
			sdio_ctxt->ch[cid]->ts_irq_rx = qcn_sdio_get_timestamp();
			sdio_ctxt->ch[cid]->crq_len = len;
			return ret;
		case QCN_SDIO_CRQ_END:
			sdio_ctxt->ch[cid]->crq_len += len;
			break;
		default:
			sdio_ctxt->ch[cid]->crq_len = len;
		}

		ch_handle = &(sdio_ctxt->ch[cid]->ch_handle);
		sdio_ctxt->ch[cid]->ts_irq_rx = qcn_sdio_get_timestamp();
		if (sdio_ctxt->ch[cid]->ch_data.dl_data_avail_cb)
			sdio_ctxt->ch[cid]->ch_data.dl_data_avail_cb(ch_handle,
					sdio_ctxt->ch[cid]->crq_len);
	}

	return ret;
}

static int qcn_sdio_config(struct qcn_sdio_client_info *cinfo)
{
	int ret = 0;
	u32 data = 0;

	sdio_claim_host(sdio_ctxt->func);
	ret = sdio_set_block_size(sdio_ctxt->func,
				  cinfo->cli_handle.block_size);

	if (ret) {
		sdio_release_host(sdio_ctxt->func);
		goto err;
	}

	data = SDIO_QCN_CONFIG_QE_MASK;

	sdio_writeb(sdio_ctxt->func, (u8)data, SDIO_QCN_CONFIG, &ret);
	if (ret) {
		sdio_release_host(sdio_ctxt->func);
		goto err;
	}

	data = (SDIO_QCN_IRQ_EN_LOCAL_MASK |
			SDIO_QCN_IRQ_EN_SYS_ERR_MASK |
			SDIO_QCN_IRQ_UNDERFLOW_MASK |
			SDIO_QCN_IRQ_OVERFLOW_MASK |
			SDIO_QCN_IRQ_CH_MISMATCH_MASK |
			SDIO_QCN_IRQ_CRQ_READY_MASK);

	sdio_writeb(sdio_ctxt->func, (u8)data, SDIO_QCN_IRQ_EN, &ret);
	sdio_release_host(sdio_ctxt->func);
	if (ret) {
		qcn_pr_err("failed write config\n");
		goto err;
	}

	sdio_ctxt->rx_addr_base = SDIO_QCN_MC_DMA0_RX_CH0;
	sdio_ctxt->rx_cnum_base	= QCN_SDIO_DMA0_RX_CNUM;
	sdio_ctxt->tx_addr_base = SDIO_QCN_MC_DMA1_TX_CH0;
	sdio_ctxt->tx_cnum_base = QCN_SDIO_DMA1_TX_CNUM;

#if (QCN_SDIO_META_VER_0)
	data = ((cinfo->cli_handle.block_size / 8) - 1);
#elif (QCN_SDIO_META_VER_1)
	data = cinfo->cli_handle.block_size;
#elif (QCN_SDIO_META_VER_2)
	data = cinfo->cli_handle.block_size;
#endif
	ret = qcn_send_meta_info(QCN_SDIO_BLK_SZ_HEVENT, data);
err:
	return ret;
}

void qcn_sw_mode_reset(void)
{
	sdio_ctxt->curr_sw_mode = QCN_SDIO_SW_RESET;
}
EXPORT_SYMBOL(qcn_sw_mode_reset);


int qcn_sw_mode_change(enum qcn_sdio_sw_mode mode)
{
	struct qcn_sdio_client_info *cinfo = NULL;

	if (!(mode) && !(mode < QCN_SDIO_SW_MAX))
		return -EINVAL;

	qcn_pr_info("qcn_sdio: current sw mode 0x%x new mode 0x%x\n",
		sdio_ctxt->curr_sw_mode, mode);
	if (sdio_ctxt->curr_sw_mode == mode)
		return 0;

	if ((sdio_ctxt->curr_sw_mode == QCN_SDIO_SW_PBL) &&
						(mode == QCN_SDIO_SW_SBL)) {
		sdio_ctxt->curr_sw_mode = QCN_SDIO_SW_SBL;
		qcn_send_meta_info(QCN_SDIO_BLK_SZ_HEVENT,
						sdio_ctxt->func->cur_blksize);
		qcn_send_meta_info(QCN_SDIO_DOORBELL_HEVENT, (u32)0);
		return 0;
	}

	switch (sdio_ctxt->curr_sw_mode) {
	case QCN_SDIO_SW_PBL:
	case QCN_SDIO_SW_SBL:
	case QCN_SDIO_SW_RDDM:
		mutex_lock(&lock);
		list_for_each_entry(cinfo, &cinfo_head, cli_list) {
			if (((cinfo->cli_handle.id == QCN_SDIO_CLI_ID_WLAN) ||
			     (cinfo->cli_handle.id == QCN_SDIO_CLI_ID_QMI) ||
			     (cinfo->cli_handle.id == QCN_SDIO_CLI_ID_DIAG)) &&
			     (mode == QCN_SDIO_SW_MROM || mode == QCN_SDIO_SW_RDDM)) {
				qcn_send_meta_info((u8)QCN_SDIO_SW_MODE_HEVENT,
						(u32)(mode | QCN_SDIO_MAJOR_VER
						| QCN_SDIO_MINOR_VER));
				if (mode == QCN_SDIO_SW_RDDM) {
					cinfo->cli_handle.block_size =
							QCN_SDIO_TTY_BLK_SZ;
				} else {
				cinfo->cli_handle.block_size =
							QCN_SDIO_MROM_BLK_SZ;
				}
				qcn_sdio_config(cinfo);
				qcn_send_meta_info(QCN_SDIO_DOORBELL_HEVENT,
									(u32)0);
				qcn_pr_info("qcn_sdio: QCN_SDIO_DOORBELL_HEVENT sent,  blk_size 0x%x\n",
					cinfo->cli_handle.block_size);
			}
		}
		mutex_unlock(&lock);
		break;
	case QCN_SDIO_SW_RESET:
	case QCN_SDIO_SW_MROM:
		mutex_lock(&lock);
		list_for_each_entry(cinfo, &cinfo_head, cli_list) {
			if ((cinfo->cli_handle.id == QCN_SDIO_CLI_ID_TTY) &&
						   (mode <= QCN_SDIO_SW_MROM)) {
				qcn_send_meta_info((u8)QCN_SDIO_SW_MODE_HEVENT,
						(u32)(mode | QCN_SDIO_MAJOR_VER
						| QCN_SDIO_MINOR_VER));
				cinfo->cli_handle.block_size =
							QCN_SDIO_TTY_BLK_SZ;
				qcn_sdio_config(cinfo);
				qcn_send_meta_info(QCN_SDIO_DOORBELL_HEVENT,
									(u32)0);
				qcn_pr_info("qcn_sdio: QCN_SDIO_DOORBELL_HEVENT sent,  blk_size 0x%x\n",
					QCN_SDIO_TTY_BLK_SZ);
			}
		}
		mutex_unlock(&lock);
		break;
	default:
		qcn_pr_err("Invalid mode\n");
	}

	driver_state = mode;
	sdio_ctxt->curr_sw_mode = mode;
	return 0;
}

static int qcn_read_meta_info(void)
{
	int ret = 0;
	u32 data = 0;
	u32 temp = 0;

	sdio_claim_host(sdio_ctxt->func);
	data = sdio_readl(sdio_ctxt->func, SDIO_QCN_LOCAL_INFO,	&ret);

	if (ret) {
		qcn_pr_err("Error while reading register = %d\n", ret);
		sdio_release_host(sdio_ctxt->func);
		return ret;
	}

	sdio_writeb(sdio_ctxt->func, (u8)SDIO_QCN_IRQ_CLR_LOCAL_MASK,
		    SDIO_QCN_IRQ_CLR, &ret);

	sdio_release_host(sdio_ctxt->func);

	if (ret) {
		qcn_pr_err("Failed to clear irq = %d\n", ret);
		return ret;
	}

	temp = (data & QCN_SDIO_LMETA_EVENT_BMSK) >> QCN_SDIO_LMETA_EVENT_SHFT;
	if (!htc_ready)
		qcn_pr_info("LOCAL EVENT data 0x%x\n", temp);
	switch (temp) {
	case QCN_SDIO_SW_MODE_LEVENT:
		temp = (data & QCN_SDIO_LMETA_SW_BMSK) >>
						QCN_SDIO_LMETA_SW_SHFT;
		qcn_sw_mode_change((enum qcn_sdio_sw_mode)temp);
		break;
	case QCN_SDIO_LP_LHI_LEVENT:
		temp = (data & QCN_SDIO_LMETA_DATA_BMSK) >> QCN_SDIO_LMETA_DATA_SHFT;
		qcn_pr_info("QCN_SDIO_LP_LHI_LEVENT received: %d pid %d\n",
			temp, !in_interrupt() ? current->pid : 0);
		switch (temp) {
		case LHI_LP_EVENT_REQ_SUSPEND:
			qcn_sdio_suspend(&sdio_ctxt->func->dev);
			break;
		case LHI_LP_EVENT_RSP_ALLOW:
			complete(&meson_suspend_allowed);
			break;
		case LHI_LP_EVENT_RSP_REJECT:
			break;
		case LHI_LP_EVENT_RSP_RESUMED:
			complete(&meson_resume_complete);
			break;
		default:
			qcn_pr_err("PM: Invalid LHI event\n");
		}
		break;
	default:
		if ((temp >= QCN_SDIO_META_START_CH0) &&
				(temp < QCN_SDIO_META_START_CH1)) {
			if (sdio_ctxt->curr_sw_mode == QCN_SDIO_SW_MROM) {
				qcn_pr_err("TTY event in MROM mode data 0x%x",
					   data);
				ret = -EINVAL;
			} else if (sdio_ctxt->ch[0] &&
				sdio_ctxt->ch[0]->ch_data.dl_meta_data_cb)
				sdio_ctxt->ch[0]->ch_data.dl_meta_data_cb(
					&(sdio_ctxt->ch[0]->ch_handle), data);
		} else if ((temp >= QCN_SDIO_META_START_CH1) &&
			(temp < QCN_SDIO_META_START_CH2)) {
			if (sdio_ctxt->ch[1] &&
				sdio_ctxt->ch[1]->ch_data.dl_meta_data_cb)
				sdio_ctxt->ch[1]->ch_data.dl_meta_data_cb(
					&(sdio_ctxt->ch[1]->ch_handle), data);
		} else if ((temp >= QCN_SDIO_META_START_CH2) &&
				(temp < QCN_SDIO_META_START_CH3)) {
			if (sdio_ctxt->ch[2] &&
				sdio_ctxt->ch[2]->ch_data.dl_meta_data_cb)
				sdio_ctxt->ch[2]->ch_data.dl_meta_data_cb(
					&(sdio_ctxt->ch[2]->ch_handle), data);
		} else if ((temp >= QCN_SDIO_META_START_CH3) &&
					(temp < QCN_SDIO_META_END)) {
			if (sdio_ctxt->ch[3] &&
				sdio_ctxt->ch[3]->ch_data.dl_meta_data_cb)
				sdio_ctxt->ch[3]->ch_data.dl_meta_data_cb(
					&(sdio_ctxt->ch[3]->ch_handle), data);
		} else {
			ret = -EINVAL;
		}
	}

	return ret;
}

#ifndef CONFIG_SDIO_QCN
static int reset_thread(void *data)
{
	qcn_sdio_purge_rw_buff();
	qcn_sdio_card_state(false);
	qcn_sdio_card_state(true);
	kthread_stop(reset_task);
	reset_task = NULL;

	return 0;
}
#endif

void qcn_sdio_irq_handler(struct sdio_func *func)
{
	u8 data = 0;
	int ret = 0;

	sdio_claim_host(sdio_ctxt->func);
	data = sdio_readb(sdio_ctxt->func, SDIO_QCN_IRQ_STATUS, &ret);
	if (ret == -ETIMEDOUT) {
		sdio_release_host(sdio_ctxt->func);

		qcn_pr_err("IRQ status read error ret = %d\n", ret);

#ifndef CONFIG_SDIO_QCN
		reset_task = kthread_run(reset_thread, NULL, "qcn_reset");
		if (IS_ERR(reset_task))
			qcn_pr_err("Failed to run qcn_reset thread\n");
#endif
		return;
	}
	sdio_release_host(sdio_ctxt->func);

	if (!htc_ready)
		qcn_pr_info("SDIO IRQ status 0x%x\n", data);
	if (data & SDIO_QCN_IRQ_CRQ_READY_MASK) {
		qcn_read_crq_info();
	} else if (data & SDIO_QCN_IRQ_LOCAL_MASK) {
		qcn_read_meta_info();
	} else if (data & SDIO_QCN_IRQ_EN_SYS_ERR_MASK) {
		sdio_claim_host(sdio_ctxt->func);
		sdio_writeb(sdio_ctxt->func, (u8)SDIO_QCN_IRQ_CLR_SYS_ERR_MASK,
				SDIO_QCN_IRQ_CLR, NULL);
		sdio_release_host(sdio_ctxt->func);
		qcn_pr_err("sys_err interrupt triggered\n");
	} else if (data & SDIO_QCN_IRQ_EN_UNDERFLOW_MASK) {
		sdio_claim_host(sdio_ctxt->func);
		sdio_writeb(sdio_ctxt->func,
					(u8)SDIO_QCN_IRQ_CLR_UNDERFLOW_MASK,
					SDIO_QCN_IRQ_CLR, NULL);
		sdio_release_host(sdio_ctxt->func);
		qcn_pr_err("underflow interrupt triggered\n");
	} else if (data & SDIO_QCN_IRQ_EN_OVERFLOW_MASK) {
		sdio_claim_host(sdio_ctxt->func);
		sdio_writeb(sdio_ctxt->func, (u8)SDIO_QCN_IRQ_CLR_OVERFLOW_MASK,
				SDIO_QCN_IRQ_CLR, NULL);
		sdio_release_host(sdio_ctxt->func);
		qcn_pr_err("overflow interrupt triggered\n");
	} else if (data & SDIO_QCN_IRQ_EN_CH_MISMATCH_MASK) {
		sdio_claim_host(sdio_ctxt->func);
		sdio_writeb(sdio_ctxt->func,
					(u8)SDIO_QCN_IRQ_CLR_CH_MISMATCH_MASK,
					SDIO_QCN_IRQ_CLR, NULL);
		sdio_release_host(sdio_ctxt->func);
		qcn_pr_err("channel mismatch interrupt triggered\n");
	} else {
		sdio_claim_host(sdio_ctxt->func);
		sdio_writeb(sdio_ctxt->func, (u8)data, SDIO_QCN_IRQ_CLR, NULL);
		sdio_release_host(sdio_ctxt->func);
	}
}

static void qcn_sdio_gpio_irq_work_handler(struct work_struct *work)
{
	static int cnt;

	qcn_pr_info("OOB intr work called %d times pid %d\n", cnt++,
		!in_interrupt() ? current->pid : 0);
	qcn_sdio_irq_handler(sdio_ctxt->func);
	qcn_pr_info("OOB intr done, re-enabling OOB intr pid %d\n",
		!in_interrupt() ? current->pid : 0);
	enable_irq(sdio_gpio_info->wake_irq);
}

static irqreturn_t _qcn_sdio_irq_handler(int irq, void *dev_id)
{
	static int count;

	qcn_pr_info("Received OOB intr %d times, SDIO LPM state %d\n", count++,
						atomic_read(&meson_suspended));
	disable_irq_nosync(irq);

	/* FW triggered resume, wakeup the SDIO dev */
	if (atomic_read(&meson_suspended)) {
		atomic_set(&sdio_ctxt->oob_wake_irq, 1);
		pm_wakeup_event(&sdio_ctxt->func->dev, 0);
		atomic_set(&meson_suspended, 0);
		return IRQ_HANDLED;
	}

	queue_work(sdio_ctxt->qcn_sdio_gpio_irq_wq, &sdio_ctxt->qcn_sdio_gpio_irq_w);
	return IRQ_HANDLED;

}

static int qcn_sdio_send_buff(u32 cid, void *buff, size_t len)
{
	int ret = 0;

	sdio_claim_host(sdio_ctxt->func);
	ret = sdio_writesb(sdio_ctxt->func,
			(sdio_ctxt->tx_addr_base + (cid * (u32)4)), buff, len);

	if (ret)
		qcn_send_io_abort();

	sdio_release_host(sdio_ctxt->func);

	return ret;
}

static int qcn_sdio_recv_buff(u32 cid, void *buff, size_t len)
{
	int ret = 0;

	sdio_claim_host(sdio_ctxt->func);
	ret = sdio_readsb(sdio_ctxt->func, buff,
			(sdio_ctxt->rx_addr_base + (cid * (u32)4)), len);

	if (ret)
		qcn_send_io_abort();

	sdio_release_host(sdio_ctxt->func);

	return ret;
}

static void qcn_sdio_record_rw_history(struct qcn_sdio_rw_info *rw_req,
					struct sdio_al_xfer_result *result)
{
	struct qcn_sdio_rw_history_entry *entry;

	spin_lock_bh(&sdio_ctxt->lock_history);
	entry = &sdio_ctxt->rw_history[sdio_ctxt->rw_history_idx];
	entry->valid = true;
	entry->cid = rw_req->cid;
	entry->dir = rw_req->dir;
	entry->len = rw_req->len;
	entry->xfer_status = result ? result->xfer_status : 0;
	entry->ts_irq = rw_req->ts_irq;
	entry->ts_queued = rw_req->ts_queued;
	entry->ts_dequeued = rw_req->ts_dequeued;
	entry->ts_xfer_done = rw_req->ts_xfer_done;
	entry->ts_comp_queued = rw_req->ts_comp_queued;
	entry->ts_comp_done = rw_req->ts_comp_done;
	sdio_ctxt->rw_history_idx =
		(sdio_ctxt->rw_history_idx + 1) % QCN_SDIO_RW_HISTORY_MAX;
	spin_unlock_bh(&sdio_ctxt->lock_history);
}

static void qcn_sdio_process_rw_req(struct qcn_sdio_rw_info *rw_req,
				    struct sdio_al_xfer_result *result)
{
	struct sdio_al_channel_handle *ch_handle = NULL;

	if (!sdio_ctxt->ch[rw_req->cid]) {
		qcn_pr_err("req dir %d cid %d channel is not registered",
				rw_req->dir, rw_req->cid);
		goto free_rw_req;
	}
	ch_handle = &sdio_ctxt->ch[rw_req->cid]->ch_handle;
	if (rw_req->dir == SDIO_AL_RX &&
			sdio_ctxt->ch[rw_req->cid]->ch_data.dl_xfer_cb) {
		sdio_ctxt->ch[rw_req->cid]->ch_data.dl_xfer_cb(
				ch_handle, result, rw_req->ctxt);
	} else if (rw_req->dir == SDIO_AL_TX &&
			sdio_ctxt->ch[rw_req->cid]->ch_data.ul_xfer_cb) {
		sdio_ctxt->ch[rw_req->cid]->ch_data.ul_xfer_cb(
				ch_handle, result, rw_req->ctxt);
	} else if (rw_req->dir == SDIO_AL_RX_AVBL &&
			sdio_ctxt->ch[rw_req->cid]->ch_data.dl_data_avail_cb) {
		sdio_ctxt->ch[rw_req->cid]->ch_data.dl_data_avail_cb(
				ch_handle, rw_req->len);
	}
free_rw_req:
	if (sdio_ctxt->ch[rw_req->cid])
		atomic_set(&sdio_ctxt->ch_status[rw_req->cid], 0);
	qcn_sdio_record_rw_history(rw_req, result);
	qcn_sdio_free_rw_req(rw_req);
	atomic_dec(&sdio_ctxt->wait_list_count);
}

static void qcn_sdio_drain_rw_req(void)
{
	int ret = 0, budget = 32;
	struct qcn_sdio_rw_comp_info *rw_comp = NULL;
	struct qcn_sdio_rw_info *rw_req = NULL;
	struct sdio_al_xfer_result result = {0};

	while (budget--) {
		spin_lock_bh(&sdio_ctxt->lock_wait_q);
		if (list_empty(&sdio_ctxt->rw_wait_q)) {
			spin_unlock_bh(&sdio_ctxt->lock_wait_q);
			break;
		}
		rw_req = list_first_entry(&sdio_ctxt->rw_wait_q,
						struct qcn_sdio_rw_info, list);
		list_del(&rw_req->list);
		spin_unlock_bh(&sdio_ctxt->lock_wait_q);
		rw_req->ts_dequeued = qcn_sdio_get_timestamp();

		if (rw_req->dir == SDIO_AL_RX) {
			ret = qcn_sdio_recv_buff(rw_req->cid, rw_req->buf,
								rw_req->len);
			rw_req->ts_xfer_done = qcn_sdio_get_timestamp();
			if (rx_dump)
				HEX_DUMP("ASYNC_RECV: ", rw_req->buf,
								rw_req->len);
		} else if (rw_req->dir == SDIO_AL_TX) {
			ret = qcn_sdio_send_buff(rw_req->cid, rw_req->buf,
								rw_req->len);
			rw_req->ts_xfer_done = qcn_sdio_get_timestamp();
			if (tx_dump)
				HEX_DUMP("ASYNC_SEND: ", rw_req->buf,
								rw_req->len);
		}

		result.xfer_status = ret;
		result.buf_addr = rw_req->buf;
		result.xfer_len = rw_req->len;
		rw_comp = kmalloc(sizeof(struct qcn_sdio_rw_comp_info), GFP_KERNEL);
		if (!rw_comp) {
			qcn_pr_err("failed to alloc rw_comp\n");
			qcn_sdio_process_rw_req(rw_req, &result);
			continue;
		}
		rw_comp->result = result;
		rw_comp->rw_req = rw_req;
		rw_req->ts_comp_queued = qcn_sdio_get_timestamp();

		qcn_sdio_add_rw_comp_req(rw_comp);
		up(&sdio_ctxt->sem_rw_comp_thread);
		}
	}

static int qcn_sdio_rw_thread(void *arg)
{
	while (!kthread_should_stop() && !sdio_ctxt->rw_shutdown) {
		if (down_interruptible(&sdio_ctxt->sem_rw_thread))
			break;
		qcn_sdio_drain_rw_req();
	}

	return 0;
}

static void qcn_sdio_process_rw_comp(void)
{
	struct qcn_sdio_rw_comp_info *rw_comp = NULL;

	while (1) {
		spin_lock_bh(&sdio_ctxt->lock_comp_q);
		if (list_empty(&sdio_ctxt->rw_comp_q)) {
			spin_unlock_bh(&sdio_ctxt->lock_comp_q);
			break;
		}
		rw_comp = list_first_entry(&sdio_ctxt->rw_comp_q,
					   struct qcn_sdio_rw_comp_info, list);
		list_del(&rw_comp->list);
		spin_unlock_bh(&sdio_ctxt->lock_comp_q);
		rw_comp->rw_req->ts_comp_done = qcn_sdio_get_timestamp();

		qcn_sdio_process_rw_req(rw_comp->rw_req, &rw_comp->result);
		kfree(rw_comp);
	}
}

static int qcn_sdio_rw_comp_thread(void *arg)
{
	while (!kthread_should_stop() && !sdio_ctxt->rw_comp_shutdown) {
		if (down_interruptible(&sdio_ctxt->sem_rw_comp_thread))
			break;
		qcn_sdio_process_rw_comp();
	}

	return 0;
}

static int qcn_sdio_wake_gpio_init(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;

	sdio_gpio_info = kzalloc(sizeof(struct qcn_sdio_gpio_info), GFP_KERNEL);
	if (!sdio_gpio_info)
		return -ENOMEM;

	sdio_gpio_info->wake_gpio = of_get_named_gpio(dev->of_node, "wlan-sdio-wake-gpio", 0);
	if (sdio_gpio_info->wake_gpio < 0) {
		qcn_pr_err("PM: error reading\n");
		goto out;
	}

	ret = gpio_request(sdio_gpio_info->wake_gpio, "wlan_sdio_wake_gpio");
	if (ret) {
		qcn_pr_err("Failed to request gpio wake GPIO, err = %d\n", ret);
		goto out;
	}

	gpio_direction_input(sdio_gpio_info->wake_gpio);
	sdio_gpio_info->wake_irq = gpio_to_irq(sdio_gpio_info->wake_gpio);

	ret = request_irq(sdio_gpio_info->wake_irq, _qcn_sdio_irq_handler, IRQF_TRIGGER_HIGH, "wlan_sdio_wake_irq", sdio_gpio_info);
	if (ret) {
		qcn_pr_err("Failed to request gpio wake IRQ, err = %d\n", ret);
		goto free_gpio;
	}

	ret = enable_irq_wake(sdio_gpio_info->wake_irq);
	if (ret) {
		qcn_pr_err("Failed to enable gpio wake IRQ, err = %d\n", ret);
		goto free_irq;
	}

	return 0;

free_irq:
	free_irq(sdio_gpio_info->wake_irq, sdio_gpio_info);
free_gpio:
	gpio_free(sdio_gpio_info->wake_gpio);
out:
	kfree(sdio_gpio_info);
	sdio_gpio_info = NULL;
	return ret;
}

static void qcn_sdio_wake_gpio_deinit(void)
{
	if (!sdio_gpio_info)
		return;

	disable_irq_wake(sdio_gpio_info->wake_irq);
	free_irq(sdio_gpio_info->wake_irq, sdio_gpio_info);
	gpio_free(sdio_gpio_info->wake_gpio);
	kfree(sdio_gpio_info);
	sdio_gpio_info = NULL;
}

static
int qcn_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct qcn_sdio_client_info *cinfo = NULL;
	int ret = 0;

	sdio_ctxt = kzalloc(sizeof(struct qcn_sdio), GFP_KERNEL);
	if (!sdio_ctxt)
		return -ENOMEM;

	sdio_ctxt->func = func;
	sdio_ctxt->id = id;
	sdio_set_drvdata(func, sdio_ctxt);

	sdio_ctxt->qcn_sdio_gpio_irq_wq = create_singlethread_workqueue("qcn_sdio_gpio_irq_wq");
	if (!sdio_ctxt->qcn_sdio_gpio_irq_wq) {
		qcn_pr_err("Error: SDIO create wq\n");
		goto err;
	}

	for (ret = 0; ret < QCN_SDIO_CH_MAX; ret++) {
		sdio_ctxt->ch[ret] = NULL;
		atomic_set(&sdio_ctxt->ch_status[ret], -1);
	}

	spin_lock_init(&sdio_ctxt->lock_free_q);
	spin_lock_init(&sdio_ctxt->lock_wait_q);
	spin_lock_init(&sdio_ctxt->lock_comp_q);
	spin_lock_init(&sdio_ctxt->lock_history);
	spin_lock_init(&async_lock);
	INIT_WORK(&sdio_ctxt->qcn_sdio_gpio_irq_w, qcn_sdio_gpio_irq_work_handler);
	sema_init(&sdio_ctxt->sem_rw_thread, 0);
	sdio_ctxt->rw_shutdown = 0;
	sdio_ctxt->rw_thread = kthread_create(qcn_sdio_rw_thread,
					      sdio_ctxt,
					      "qcn_sdio_rw");
	if (IS_ERR(sdio_ctxt->rw_thread)) {
		qcn_pr_err("Could not create rw thread");
		goto err;
	}
	set_user_nice(sdio_ctxt->rw_thread, -20);
	wake_up_process(sdio_ctxt->rw_thread);

	sema_init(&sdio_ctxt->sem_rw_comp_thread, 0);
	sdio_ctxt->rw_comp_shutdown = 0;
	sdio_ctxt->rw_comp_thread = kthread_create(qcn_sdio_rw_comp_thread,
						   sdio_ctxt,
						   "qcn_sdio_rw_comp");
	if (IS_ERR(sdio_ctxt->rw_comp_thread)) {
		qcn_pr_err("Could not create rw comp thread");
		goto err;
	}
	set_user_nice(sdio_ctxt->rw_comp_thread, -19);
	wake_up_process(sdio_ctxt->rw_comp_thread);
	INIT_LIST_HEAD(&sdio_ctxt->rw_free_q);
	INIT_LIST_HEAD(&sdio_ctxt->rw_wait_q);
	INIT_LIST_HEAD(&sdio_ctxt->rw_comp_q);

	for (ret = 0; ret < QCN_SDIO_RW_REQ_MAX; ret++)
		qcn_sdio_free_rw_req(&sdio_ctxt->rw_req_info[ret]);

	sdio_claim_host(sdio_ctxt->func);
	ret = sdio_enable_func(sdio_ctxt->func);
	if (ret) {
		qcn_pr_err("Error:%d SDIO enable func\n", ret);
		sdio_release_host(sdio_ctxt->func);
		goto err;
	}
	ret = sdio_claim_irq(sdio_ctxt->func, qcn_sdio_irq_handler);
	if (ret) {
		qcn_pr_err("Error:%d SDIO claim irq\n", ret);
		sdio_release_host(sdio_ctxt->func);
		goto err;
	}

	qcn_enable_async_irq(true);
	sdio_release_host(sdio_ctxt->func);

	atomic_set(&xport_status, 1);
	atomic_set(&meson_card_state, 1);

	if (qcn_read_meta_info()) {
		qcn_pr_err("Error: SDIO Config\n");
		qcn_send_meta_info((u8)QCN_SDIO_SW_MODE_HEVENT, (u32)0);
	}

	current_host = func->card->host;
	init_completion(&meson_suspend_allowed);
	init_completion(&meson_resume_complete);

	mutex_lock(&lock);
	list_for_each_entry(cinfo, &cinfo_head, cli_list) {
		mutex_unlock(&lock);
		if (!cinfo->is_probed) {
			qcn_pr_info("Probe SDIO AL client id %d",
						cinfo->cli_handle.id);
			cinfo->cli_handle.func = sdio_ctxt->func;
			cinfo->is_probed = !cinfo->cli_data.probe(
							&cinfo->cli_handle);
		}
		mutex_lock(&lock);
	}
	mutex_unlock(&lock);

	qcn_sdio_debugfs_create();

	return 0;
err:
	kfree(sdio_ctxt);
	sdio_ctxt = NULL;
	current_host = NULL;
	return ret;
}

static void qcn_sdio_stop_rw_thread(void)
{
	if (sdio_ctxt->rw_thread) {
		sdio_ctxt->rw_shutdown = 1;
		up(&sdio_ctxt->sem_rw_thread);
		kthread_stop(sdio_ctxt->rw_thread);
		sdio_ctxt->rw_thread = NULL;
	}
}

static void qcn_sdio_stop_rw_comp_thread(void)
{
	if (sdio_ctxt->rw_comp_thread) {
		sdio_ctxt->rw_comp_shutdown = 1;
		up(&sdio_ctxt->sem_rw_comp_thread);
		kthread_stop(sdio_ctxt->rw_comp_thread);
		sdio_ctxt->rw_comp_thread = NULL;
	}
}

static void qcn_sdio_remove(struct sdio_func *func)
{
	struct qcn_sdio_client_info *cinfo = NULL;
	struct qcn_sdio_ch_info *ch_info = NULL;

	atomic_set(&xport_status, 0);
	atomic_set(&meson_card_state, 0);
	sdio_claim_host(sdio_ctxt->func);
	qcn_enable_async_irq(false);
	sdio_release_irq(sdio_ctxt->func);
	sdio_release_host(sdio_ctxt->func);

	qcn_sdio_stop_rw_thread();
	qcn_sdio_stop_rw_comp_thread();
	qcn_sdio_purge_rw_buff();
	qcn_sdio_purge_rw_comp_buff();

	destroy_workqueue(sdio_ctxt->qcn_sdio_gpio_irq_wq);
	mutex_lock(&lock);
	list_for_each_entry(cinfo, &cinfo_head, cli_list) {
		while (!list_empty(&cinfo->ch_head)) {
			ch_info = list_first_entry(&cinfo->ch_head,
					struct qcn_sdio_ch_info, ch_list);
			sdio_al_deregister_channel(&ch_info->ch_handle);
		}
		mutex_unlock(&lock);
		if (cinfo->is_probed) {
			cinfo->cli_data.remove(&cinfo->cli_handle);
			cinfo->is_probed = 0;
		}
		mutex_lock(&lock);
	}
	mutex_unlock(&lock);

	qcn_sdio_debugfs_destroy();
	kfree(sdio_ctxt);
	sdio_ctxt = NULL;
//	mmc_retune_enable(current_host);
	current_host = NULL;
}

static const struct sdio_device_id qcn_sdio_devices[] = {
	{SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_QCN_BASE | 0x0))},
	{SDIO_DEVICE(MANUFACTURER_CODE, (MANUFACTURER_ID_THEMISTO_QCN_BASE | 0x0))},
	{},
};

MODULE_DEVICE_TABLE(sdio, qcn_sdio_devices);



static const struct dev_pm_ops qcn_sdio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(qcn_sdio_suspend, qcn_sdio_resume)
	SET_RUNTIME_PM_OPS(qcn_sdio_runtime_suspend, qcn_sdio_runtime_resume, qcn_sdio_runtime_idle)
};

static struct sdio_driver qcn_sdio_driver = {
	.name = "qcn_sdio",
	.id_table = qcn_sdio_devices,
	.probe = qcn_sdio_probe,
	.remove = qcn_sdio_remove,
	.drv = {
		.pm = &qcn_sdio_pm_ops,
	}
};

static int qcn_sdio_plat_probe(struct platform_device *pdev)
{
	int ret = 0;

	mutex_init(&lock);
	INIT_LIST_HEAD(&cinfo_head);
	atomic_set(&status, 1);

	init_completion(&client_probe_complete);

	ipc_log_ctxt = ipc_log_context_create(QCN_IPC_LOG_PAGES, "qcn_sdio", 0);
	if (!ipc_log_ctxt) {
		qcn_pr_err("failed to initialize ipc logging for qcn_sdio");
	}

	ret = sdio_register_driver(&qcn_sdio_driver);
	if (ret) {
		qcn_pr_err("SDIO driver registration failed: %d\n", ret);
		mutex_destroy(&lock);
		atomic_set(&status, 0);
	}

	qcn_sdio_wake_gpio_init(pdev);

	ret = device_init_wakeup(&pdev->dev, true);
	if (ret)
		qcn_pr_err("Failed to init platform device wakeup source, err = %d\n", ret);

	qcn_create_sysfs(&pdev->dev);

	return ret;
}

static void qcn_sdio_plat_remove(struct platform_device *pdev)
{
	struct qcn_sdio_client_info *cinfo = NULL;

	qcn_sdio_wake_gpio_deinit();

	mutex_lock(&lock);
	while (!list_empty(&cinfo_head)) {
		cinfo = list_first_entry(&cinfo_head, struct
						qcn_sdio_client_info, cli_list);
		mutex_unlock(&lock);
		sdio_al_deregister_client(&cinfo->cli_handle);
		mutex_lock(&lock);
		list_del(&cinfo->cli_list);
	}
	mutex_unlock(&lock);
	mutex_destroy(&lock);
	sdio_unregister_driver(&qcn_sdio_driver);
	ipc_log_context_destroy(ipc_log_ctxt);
	ipc_log_ctxt = NULL;
	atomic_set(&status, 0);

	return;
}

static const struct of_device_id qcn_sdio_dt_match[] = {
	{.compatible = "qcom,qcn-sdio"},
	{}
};
MODULE_DEVICE_TABLE(of, qcn_sdio_dt_match);

static struct platform_driver qcn_sdio_plat_driver = {
	.probe  = qcn_sdio_plat_probe,
	.remove = qcn_sdio_plat_remove,
	.driver = {
		.name = "qcn-sdio",
		.owner = THIS_MODULE,
		.of_match_table = qcn_sdio_dt_match,
	},
};

int sdio_al_is_ready(void)
{
	if (atomic_read(&status))
		return 0;
	else
		return -EBUSY;
}
EXPORT_SYMBOL(sdio_al_is_ready);

struct sdio_al_client_handle *sdio_al_register_client(
					struct sdio_al_client_data *client_data)
{
	struct qcn_sdio_client_info *client_info = NULL;

	if (!((client_data) && (client_data->name) &&
			(client_data->probe) && (client_data->remove))) {
		qcn_pr_err("SDIO: Invalid param\n");
		return ERR_PTR(-EINVAL);
	}

	client_info = (struct qcn_sdio_client_info *)
		kzalloc(sizeof(struct qcn_sdio_client_info), GFP_KERNEL);
	if (!client_info)
		return ERR_PTR(-ENOMEM);

	memcpy(&client_info->cli_data, client_data,
					sizeof(struct sdio_al_client_data));

	if (!strcmp(client_data->name, "SDIO_AL_CLIENT_TTY")) {
		client_info->cli_handle.id = QCN_SDIO_CLI_ID_TTY;
		client_info->cli_handle.block_size = QCN_SDIO_TTY_BLK_SZ;
	} else if (!strcmp(client_data->name, "SDIO_AL_CLIENT_WLAN")) {
		client_info->cli_handle.id = QCN_SDIO_CLI_ID_WLAN;
		client_info->cli_handle.block_size = QCN_SDIO_MROM_BLK_SZ;
	} else if (!strcmp(client_data->name, "SDIO_AL_CLIENT_QMI")) {
		client_info->cli_handle.id = QCN_SDIO_CLI_ID_QMI;
		client_info->cli_handle.block_size = QCN_SDIO_MROM_BLK_SZ;
	} else if (!strcmp(client_data->name, "SDIO_AL_CLIENT_DIAG")) {
		client_info->cli_handle.id = QCN_SDIO_CLI_ID_DIAG;
		client_info->cli_handle.block_size = QCN_SDIO_MROM_BLK_SZ;
	} else {
		qcn_pr_err("SDIO: Invalid name\n");
		kfree(client_info);
		return ERR_PTR(-EINVAL);
	}
	client_info->cli_handle.client_data = &client_info->cli_data;

	INIT_LIST_HEAD(&client_info->ch_head);
	mutex_lock(&lock);
	list_add_tail(&client_info->cli_list, &cinfo_head);
	mutex_unlock(&lock);

	client_info->is_probed = 0;

	return &client_info->cli_handle;
}
EXPORT_SYMBOL(sdio_al_register_client);

void sdio_al_deregister_client(struct sdio_al_client_handle *handle)
{
	struct qcn_sdio_ch_info	*ch_info = NULL;
	struct qcn_sdio_client_info *client_info = NULL;

	if (!handle) {
		qcn_pr_err("SDIO: Invalid param\n");
		return;
	}

	client_info = container_of(handle, struct qcn_sdio_client_info,
								cli_handle);

	while (!list_empty(&client_info->ch_head)) {
		ch_info = list_first_entry(&client_info->ch_head,
					struct qcn_sdio_ch_info, ch_list);
		sdio_al_deregister_channel(&ch_info->ch_handle);
	}
	mutex_lock(&lock);
	list_del(&client_info->cli_list);
	kfree(client_info);
	mutex_unlock(&lock);
}
EXPORT_SYMBOL(sdio_al_deregister_client);

struct sdio_al_channel_handle *sdio_al_register_channel(
		struct sdio_al_client_handle *client_handle,
		struct sdio_al_channel_data *channel_data)
{
	struct qcn_sdio_ch_info	*ch_info = NULL;
	struct qcn_sdio_client_info *client_info = NULL;

	if (!((channel_data) && (channel_data->name) && (client_handle) &&
				(channel_data->client_data))) {
		qcn_pr_err("SDIO: Invalid param\n");
		return ERR_PTR(-EINVAL);
	}

	ch_info = kzalloc(sizeof(struct qcn_sdio_ch_info), GFP_KERNEL);
	if (!ch_info)
		return ERR_PTR(-ENOMEM);

	memcpy(&ch_info->ch_data, channel_data,
					sizeof(struct sdio_al_channel_data));

	if (!strcmp(channel_data->name, "SDIO_AL_TTY_CH0")) {
		if (atomic_read(&sdio_ctxt->ch_status[QCN_SDIO_CH_0]) < 0)
			ch_info->ch_handle.channel_id = QCN_SDIO_CH_0;
	} else if (!strcmp(channel_data->name, "SDIO_AL_WLAN_CH0")) {
		if (atomic_read(&sdio_ctxt->ch_status[QCN_SDIO_CH_1]) < 0)
			ch_info->ch_handle.channel_id = QCN_SDIO_CH_1;
	} else if (!strcmp(channel_data->name, "SDIO_AL_WLAN_CH1")) {
		if (atomic_read(&sdio_ctxt->ch_status[QCN_SDIO_CH_2]) < 0)
			ch_info->ch_handle.channel_id = QCN_SDIO_CH_2;
	} else if (!strcmp(channel_data->name, "SDIO_AL_QMI_CH0")) {
		if (atomic_read(&sdio_ctxt->ch_status[QCN_SDIO_CH_2]) < 0)
			ch_info->ch_handle.channel_id = QCN_SDIO_CH_2;
	} else if (!strcmp(channel_data->name, "SDIO_AL_DIAG_CH0")) {
		if (atomic_read(&sdio_ctxt->ch_status[QCN_SDIO_CH_3]) < 0)
			ch_info->ch_handle.channel_id = QCN_SDIO_CH_3;
	} else {
		qcn_pr_err("SDIO: Invalid CH name: %s\n", channel_data->name);
		kfree(ch_info);
		return ERR_PTR(-EINVAL);
	}

	client_info = container_of(client_handle, struct qcn_sdio_client_info,
								cli_handle);
	ch_info->ch_handle.channel_data = &ch_info->ch_data;
	ch_info->chandle = &client_info->cli_handle;
	list_add_tail(&ch_info->ch_list, &client_info->ch_head);
	sdio_ctxt->ch[ch_info->ch_handle.channel_id] = ch_info;
	atomic_set(&sdio_ctxt->ch_status[ch_info->ch_handle.channel_id], 0);

	return &ch_info->ch_handle;
}
EXPORT_SYMBOL(sdio_al_register_channel);

void sdio_al_deregister_channel(struct sdio_al_channel_handle *ch_handle)
{
	int ret = 0;
	struct qcn_sdio_ch_info *ch_info = NULL;

	if (!ch_handle) {
		qcn_pr_err("Error: Invalid Param\n");
		return;
	}

	do {
		ret = atomic_cmpxchg(
			&sdio_ctxt->ch_status[ch_handle->channel_id], 0, 1);
		if (ret) {
			if (ret == -1)
				return;

			usleep_range(1000, 1500);
		}
	} while (ret);

	ch_info = sdio_ctxt->ch[ch_handle->channel_id];
	if (ch_info) {
		list_del(&ch_info->ch_list);
		sdio_ctxt->ch[ch_handle->channel_id] = NULL;
		atomic_set(&sdio_ctxt->ch_status[ch_handle->channel_id], -1);
		kfree(ch_info);
	}
}
EXPORT_SYMBOL(sdio_al_deregister_channel);

void sdio_al_process_pending_irq(void)
{
	if (sdio_ctxt->pending_crq_ch_1) {
		qcn_pr_info("Queuing pending CH1 CRQ work\n");
		up(&sdio_ctxt->sem_rw_thread);
		sdio_ctxt->pending_crq_ch_1 = false;
	}
}
EXPORT_SYMBOL(sdio_al_process_pending_irq);

int sdio_al_queue_transfer_async(struct sdio_al_channel_handle *handle,
		enum sdio_al_dma_direction dir,
		void *buf, size_t len, int priority, void *ctxt)
{
	struct qcn_sdio_rw_info *rw_req = NULL;
	u32 cid = QCN_SDIO_CH_MAX;

	if (!atomic_read(&xport_status))
		return -ENODEV;

	if (!handle) {
		qcn_pr_err("Error: Invalid Param\n");
		return -EINVAL;
	}

	cid = handle->channel_id;

	if (!(cid < QCN_SDIO_CH_MAX) &&
				(atomic_read(&sdio_ctxt->ch_status[cid]) < 0))
		return -EINVAL;

	if (dir == SDIO_AL_TX && atomic_read(&sdio_ctxt->free_list_count) <= 8)
		return -ENOMEM;

	rw_req = qcn_sdio_alloc_rw_req();
	if (!rw_req)
		return -ENOMEM;

	rw_req->cid = cid;
	rw_req->dir = dir;
	rw_req->buf = buf;
	rw_req->len = len;
	rw_req->ctxt = ctxt;
	if (dir == SDIO_AL_TX && sdio_ctxt->ch[cid])
		sdio_ctxt->ch[cid]->ts_irq_tx = qcn_sdio_get_timestamp();
	rw_req->ts_irq = sdio_ctxt->ch[cid] ?
		(dir == SDIO_AL_TX ? sdio_ctxt->ch[cid]->ts_irq_tx :
				      sdio_ctxt->ch[cid]->ts_irq_rx) :
		qcn_sdio_get_timestamp();
	rw_req->ts_queued = qcn_sdio_get_timestamp();

	if (dir == SDIO_AL_RX)
		spin_lock(&async_lock);

	qcn_sdio_add_rw_req(rw_req);
	up(&sdio_ctxt->sem_rw_thread);

	if (dir == SDIO_AL_RX)
		spin_unlock(&async_lock);

	return 0;
}
EXPORT_SYMBOL(sdio_al_queue_transfer_async);

int sdio_al_queue_transfer(struct sdio_al_channel_handle *ch_handle,
		enum sdio_al_dma_direction dir,
		void *buf, size_t len, int priority)
{
	int ret = 0;
	u32 cid = QCN_SDIO_CH_MAX;

	if (!atomic_read(&xport_status))
		return -ENODEV;

	if (!ch_handle) {
		qcn_pr_err("SDIO: Invalid Param\n");
		return -EINVAL;
	}

	if (dir == SDIO_AL_RX && !list_empty(&sdio_ctxt->rw_wait_q) &&
				!atomic_read(&sdio_ctxt->wait_list_count)) {
		sdio_al_queue_transfer_async(ch_handle, dir, buf, len, true,
							(void *)(uintptr_t)len);
		qcn_pr_info("switching to async\n");
		ret = 1;
	} else {
		cid = ch_handle->channel_id;

		if (!(cid < QCN_SDIO_CH_MAX))
			return -EINVAL;

		if (dir == SDIO_AL_RX) {
			if (!atomic_read(&sdio_ctxt->wait_list_count))
				ret = qcn_sdio_recv_buff(cid, buf, len);
			else {
				sdio_al_queue_transfer_async(ch_handle, dir,
					buf, len, true, (void *)(uintptr_t)len);
				qcn_pr_info("switching to async\n");
				ret = 1;
			}

			if (rx_dump)
				HEX_DUMP("SYNC_RECV: ", buf, len);
		} else if (dir == SDIO_AL_TX) {
			ret = qcn_sdio_send_buff(cid, buf, len);
			if (tx_dump)
				HEX_DUMP("SYNC_SEND: ", buf, len);
		} else
			ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(sdio_al_queue_transfer);

int sdio_al_meta_transfer(struct sdio_al_channel_handle *handle,
					unsigned int data, unsigned int trans)
{
	u32 cid = QCN_SDIO_CH_MAX;
	u8 event = 0;

	if (!atomic_read(&xport_status))
		return -ENODEV;

	if (!handle)
		return -EINVAL;

	cid = handle->channel_id;

	if (!(cid < QCN_SDIO_CH_MAX))
		return -EINVAL;

	event = (u8)((data & QCN_SDIO_HMETA_EVENT_BMSK) >>
						QCN_SDIO_HMETA_EVENT_SHFT);

	if (cid == QCN_SDIO_CH_0) {
		if ((event < QCN_SDIO_META_START_CH0) &&
					(event >= QCN_SDIO_META_START_CH1)) {
			return -EINVAL;
		}
		if (sdio_ctxt->curr_sw_mode == QCN_SDIO_SW_MROM) {
			qcn_pr_err("Received TTY event in MROM mode mdata 0x%x",
				    data);
			return -EINVAL;
		}
	} else if (cid == QCN_SDIO_CH_1) {
		if ((event < QCN_SDIO_META_START_CH1) &&
					(event >= QCN_SDIO_META_START_CH2)) {
			return -EINVAL;
		}
	} else if (cid == QCN_SDIO_CH_2) {
		if ((event < QCN_SDIO_META_START_CH2) &&
					(event >= QCN_SDIO_META_START_CH3)) {
			return -EINVAL;
		}
	} else if (cid == QCN_SDIO_CH_3) {
		if ((event < QCN_SDIO_META_START_CH3) &&
					(event >= QCN_SDIO_META_END)) {
			return -EINVAL;
		}
	}

	return qcn_send_meta_info(event, data);
}
EXPORT_SYMBOL(sdio_al_meta_transfer);

void qcn_sdio_card_release(void)
{
	if (!sdio_ctxt) {
		qcn_pr_err("SDIO ctxt is NULL\n");
		return;
	}

	sdio_claim_host(sdio_ctxt->func);
	qcn_enable_async_irq(false);
	sdio_release_irq(sdio_ctxt->func);
	sdio_release_host(sdio_ctxt->func);
	qcn_sdio_purge_rw_buff();
	atomic_set(&meson_card_state, 0);
}
EXPORT_SYMBOL(qcn_sdio_card_release);

int qcn_sdio_card_claim(void)
{
	int ret = 0;
	unsigned int event = 0;
	unsigned int mdata = 0;
	struct sdio_al_channel_handle *ch_handle = NULL;

	if (!sdio_ctxt) {
		qcn_pr_err("SDIO ctxt is NULL\n");
		return -ENODEV;
	}

	htc_ready = 0;
	sdio_claim_host(sdio_ctxt->func);
	ret = mmc_hw_reset(sdio_ctxt->func->card);
	if (ret) {
		qcn_pr_err("Error:%d SDIO reset card\n", ret);
		sdio_release_host(sdio_ctxt->func);
		return ret;
	}

	ret = sdio_enable_func(sdio_ctxt->func);
	if (ret) {
		qcn_pr_err("Error:%d SDIO enable func\n", ret);
		sdio_release_host(sdio_ctxt->func);
		return ret;
	}

	ret = sdio_claim_irq(sdio_ctxt->func, qcn_sdio_irq_handler);
	if (ret) {
		qcn_pr_err("Error:%d SDIO claim irq\n", ret);
		sdio_release_host(sdio_ctxt->func);
		return ret;
	}

	qcn_enable_async_irq(true);
	sdio_release_host(sdio_ctxt->func);

	ret = qcn_read_meta_info();
	if (ret) {
		qcn_pr_err("Error: SDIO Config ret %d\n", ret);
		qcn_send_meta_info((u8)QCN_SDIO_SW_MODE_HEVENT, (u32)0);
	}

	/* Send Sahara door bell event here as TTY client is already probed */
	ch_handle = &sdio_ctxt->ch[QCN_SDIO_CH_0]->ch_handle;
	if (ch_handle) {
		event = 0x20; //TTY_SAHARA_DOORBELL_EVENT;
		mdata = (event << 24);
		ret = sdio_al_meta_transfer(ch_handle, mdata, 0);
		if (ret)
			qcn_pr_err("Sahara door bell event failed\n");
		qcn_pr_info("qcn_sdio: TTY_SAHARA_DOORBELL_EVENT sent\n");
	}

	if (!ret) {
		atomic_set(&meson_card_state, 1);
	}

	return ret;
}
EXPORT_SYMBOL(qcn_sdio_card_claim);

int qcn_sdio_card_state(bool enable)
{
	int ret = 0;

	if (!current_host)
		return -ENODEV;

	sdio_claim_host(sdio_ctxt->func);
	if (enable) {
		if (!atomic_read(&xport_status)) {
			ret = mmc_add_host(current_host);
			if (ret)
				qcn_pr_err("ret = %d\n", ret);
		}
	} else {
		if (atomic_read(&xport_status))
			mmc_remove_host(current_host);
	}
	sdio_release_host(sdio_ctxt->func);

	return ret;
}
EXPORT_SYMBOL(qcn_sdio_card_state);

static ssize_t qcn_card_state(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	int state = 0;

	if (sscanf(buf, "%du", &state) != 1)
		return -EINVAL;

	qcn_sdio_card_state(state);

	return count;
}
static DEVICE_ATTR(card_state, 0220, NULL, qcn_card_state);

static void qcn_sdio_seq_print_ts(struct seq_file *s, const char *name,
				   u64 ts)
{
	if (ts == 0)
		seq_printf(s, "%s=- ", name);
	else
		seq_printf(s, "%s=%llu ", name, ts);
}

static int qcn_sdio_debugfs_show(struct seq_file *s, void *data)
{
	struct qcn_sdio_rw_info *rw_req = NULL;
	struct qcn_sdio_rw_comp_info *rw_comp = NULL;
	int cid;
	u32 i;

	seq_puts(s, "=== ch_info: last IRQ timestamp per channel/direction ===\n");
	mutex_lock(&lock);
	for (cid = 0; cid < QCN_SDIO_CH_MAX; cid++) {
		if (!sdio_ctxt->ch[cid])
			continue;
		seq_printf(s, "cid=%d crq_len=%u ", cid,
			   sdio_ctxt->ch[cid]->crq_len);
		qcn_sdio_seq_print_ts(s, "ts_irq_rx", sdio_ctxt->ch[cid]->ts_irq_rx);
		qcn_sdio_seq_print_ts(s, "ts_irq_tx", sdio_ctxt->ch[cid]->ts_irq_tx);
		seq_puts(s, "\n");
	}
	mutex_unlock(&lock);

	seq_puts(s, "\n=== rw_wait_q: requests awaiting rw thread ===\n");
	spin_lock_bh(&sdio_ctxt->lock_wait_q);
	list_for_each_entry(rw_req, &sdio_ctxt->rw_wait_q, list) {
		seq_printf(s, "cid=%u dir=%d len=%zu ", rw_req->cid,
			   rw_req->dir, rw_req->len);
		qcn_sdio_seq_print_ts(s, "ts_irq", rw_req->ts_irq);
		qcn_sdio_seq_print_ts(s, "ts_queued", rw_req->ts_queued);
		qcn_sdio_seq_print_ts(s, "ts_dequeued", rw_req->ts_dequeued);
		qcn_sdio_seq_print_ts(s, "ts_xfer_done", rw_req->ts_xfer_done);
		qcn_sdio_seq_print_ts(s, "ts_comp_queued", rw_req->ts_comp_queued);
		qcn_sdio_seq_print_ts(s, "ts_comp_done", rw_req->ts_comp_done);
		seq_puts(s, "\n");
	}
	spin_unlock_bh(&sdio_ctxt->lock_wait_q);

	seq_puts(s, "\n=== rw_comp_q: requests awaiting comp thread dispatch ===\n");
	spin_lock_bh(&sdio_ctxt->lock_comp_q);
	list_for_each_entry(rw_comp, &sdio_ctxt->rw_comp_q, list) {
		rw_req = rw_comp->rw_req;
		seq_printf(s, "cid=%u dir=%d len=%zu xfer_status=%d ", rw_req->cid,
			   rw_req->dir, rw_req->len, rw_comp->result.xfer_status);
		qcn_sdio_seq_print_ts(s, "ts_irq", rw_req->ts_irq);
		qcn_sdio_seq_print_ts(s, "ts_queued", rw_req->ts_queued);
		qcn_sdio_seq_print_ts(s, "ts_dequeued", rw_req->ts_dequeued);
		qcn_sdio_seq_print_ts(s, "ts_xfer_done", rw_req->ts_xfer_done);
		qcn_sdio_seq_print_ts(s, "ts_comp_queued", rw_req->ts_comp_queued);
		qcn_sdio_seq_print_ts(s, "ts_comp_done", rw_req->ts_comp_done);
		seq_puts(s, "\n");
	}
	spin_unlock_bh(&sdio_ctxt->lock_comp_q);

	seq_puts(s, "\n=== rw_history: last completed requests (newest first) ===\n");
	spin_lock_bh(&sdio_ctxt->lock_history);
	for (i = 0; i < QCN_SDIO_RW_HISTORY_MAX; i++) {
		u32 idx = (sdio_ctxt->rw_history_idx + QCN_SDIO_RW_HISTORY_MAX
			   - 1 - i) % QCN_SDIO_RW_HISTORY_MAX;
		struct qcn_sdio_rw_history_entry *entry =
			&sdio_ctxt->rw_history[idx];

		if (!entry->valid)
			continue;

		seq_printf(s, "cid=%u dir=%d len=%zu xfer_status=%d ",
			   entry->cid, entry->dir, entry->len,
			   entry->xfer_status);
		qcn_sdio_seq_print_ts(s, "ts_irq", entry->ts_irq);
		qcn_sdio_seq_print_ts(s, "ts_queued", entry->ts_queued);
		qcn_sdio_seq_print_ts(s, "ts_dequeued", entry->ts_dequeued);
		qcn_sdio_seq_print_ts(s, "ts_xfer_done", entry->ts_xfer_done);
		qcn_sdio_seq_print_ts(s, "ts_comp_queued", entry->ts_comp_queued);
		qcn_sdio_seq_print_ts(s, "ts_comp_done", entry->ts_comp_done);
		seq_puts(s, "\n");
	}
	spin_unlock_bh(&sdio_ctxt->lock_history);

	return 0;
}

static int qcn_sdio_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, qcn_sdio_debugfs_show, inode->i_private);
}

static const struct file_operations qcn_sdio_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = qcn_sdio_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void qcn_sdio_debugfs_create(void)
{
	sdio_ctxt->debugfs_dir = debugfs_create_dir("qcn_sdio", NULL);
	if (IS_ERR_OR_NULL(sdio_ctxt->debugfs_dir)) {
		qcn_pr_err("Failed to create qcn_sdio debugfs dir\n");
		sdio_ctxt->debugfs_dir = NULL;
		return;
	}

	debugfs_create_file("rw_timestamps", 0444, sdio_ctxt->debugfs_dir,
			     NULL, &qcn_sdio_debugfs_fops);
}

static void qcn_sdio_debugfs_destroy(void)
{
	debugfs_remove_recursive(sdio_ctxt->debugfs_dir);
	sdio_ctxt->debugfs_dir = NULL;
}

static int qcn_create_sysfs(struct device *dev)
{
	int ret = 0;

	ret = device_create_file(dev, &dev_attr_card_state);
	if (ret) {
		qcn_pr_err("Failed to create device file, err = %d\n", ret);
		goto out;
	}

	return 0;
out:
	return ret;
}

module_platform_driver(qcn_sdio_plat_driver);
MODULE_LICENSE("GPL v2");
