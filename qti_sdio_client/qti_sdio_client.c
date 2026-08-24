// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

/* add additional information to our printk's */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/cache.h>
#ifdef CONFIG_CNSS_OUT_OF_TREE
#include "qcn_sdio_al.h"
#include "cnss_prealloc.h"
#else
#include <linux/qcn_sdio_al.h>
#endif
#include <linux/of.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <uapi/linux/major.h>
#include <linux/ipc_logging.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#define	DATA_ALIGNMENT			4
#define	MAX_CLIENTS			5
#define	TX_BUF_SIZE			(16 * 1024)
#define	RX_BUF_SIZE			(16 * 1024)

#define	TTY_RX_BYTE_COUNT		0x21
#define	TTY_RX_BYTE_COUNT_TRANS		0x22
#define	TTY_RX_BYTE_TRANS_MODE		0x23
#define	TTY_RX_BYTE_TX_READY		0x24

#define	TTY_SAHARA_DOORBELL_EVENT	0x20
#define	TTY_TX_BUF_SZ_EVENT		0x21
#define	TTY_TX_BUF_SZ_TRANS_EVENT	0x22

#define	QMI_RX_BYTE_COUNT		0x61
#define	QMI_RX_BYTE_COUNT_TRANS		0x62
#define	QMI_RX_BYTE_TRANS_MODE		0x63
#define	QMI_RX_BYTE_TX_READY		0x64

#define	QMI_DOORBELL_EVENT		0x60
#define	QMI_TX_BUF_SZ_EVENT		0x61
#define	QMI_TX_BUF_SZ_TRANS_EVENT	0x62

#define	DIAG_RX_BYTE_COUNT		0x81
#define	DIAG_RX_BYTE_COUNT_TRANS	0x82
#define	DIAG_RX_BYTE_TRANS_MODE		0x83
#define	DIAG_RX_BYTE_TX_READY		0x84

#define	DIAG_DOORBELL_EVENT		0x80
#define	DIAG_TX_BUF_SZ_EVENT		0x81
#define	DIAG_TX_BUF_SZ_TRANS_EVENT	0x82

#define	QCN_IPC_LOG_PAGES		32

#define	IPC_BRIDGE_MAX_READ_SZ		(8 * 1024)
#define	IPC_BRIDGE_MAX_WRITE_SZ		(8 * 1024)

#define SDIO_QCN_HRQ_PUSH_BLK_MASK	(0x00800000)

#define QCN_SDIO_META_VER_0		(0)
#define QCN_SDIO_META_VER_1		(0)
#define QCN_SDIO_META_VER_2		(1)

#if (QCN_SDIO_META_VER_2)
#define SDIO_META_DATA_BMSK		(0x0003FFFF)
#else
#define SDIO_META_DATA_BMSK		(0x00003FFF)
#endif

static bool to_console;
module_param(to_console, bool, S_IRUGO | S_IWUSR | S_IWGRP);

static bool ipc_log = true;
module_param(ipc_log, bool, S_IRUGO | S_IWUSR | S_IWGRP);


static DEFINE_MUTEX(work_lock);

#define	qlog(qsb, _msg, ...) do {					     \
	if (!qsb ? 1 : to_console)					     \
		pr_err("[%s] " _msg, __func__, ##__VA_ARGS__);		     \
	if (qsb && ipc_log)						     \
		ipc_log_string(qsb->ipc_log_ctxt, "[%s] " _msg, __func__,    \
							##__VA_ARGS__);	     \
} while (0)

enum qcn_sdio_cli_id {
	QCN_SDIO_CLI_ID_INVALID = 0,
	QCN_SDIO_CLI_ID_TTY,
	QCN_SDIO_CLI_ID_WLAN,
	QCN_SDIO_CLI_ID_QMI,
	QCN_SDIO_CLI_ID_DIAG,
	QCN_SDIO_CLI_ID_MAX
};

struct ipc_bridge_platform_data {
	unsigned int max_read_size;
	unsigned int max_write_size;
	int (*open)(int id, void *ops);
	int (*read)(int id, char *buf, size_t count);
	int (*write)(int id, char *buf, size_t count);
	int (*close)(int id);
};

struct diag_bridge_ops {
	void *ctxt;
	void (*read_complete_cb)(void *ctxt, char *buf,
			int buf_size, int actual);
	void (*write_complete_cb)(void *ctxt, char *buf,
			int buf_size, int actual);
	int (*suspend)(void *ctxt);
	void (*resume)(void *ctxt);
};

struct qti_sdio_bridge {
	const char *name;
	const char *ch_name;
	uint8_t id;
	uint8_t mode;
	struct sdio_al_channel_handle *channel_handle;
	struct sdio_al_client_handle *client_handle;
	wait_queue_head_t wait_q;
	u8 *tx_dma_buf;
	u8 *rx_dma_buf;
	int data_avail;
	int data_remain;
	int blk_trans_mode;
	int tx_ready;
	struct diag_bridge_ops *ops;
	void *ipc_log_ctxt;
	void *priv_dev_info;
	unsigned int mdata_count;
	unsigned int tx_ready_count;
	unsigned int data_avail_count;
	atomic_t is_client_closing;
};

struct data_avail_node {
	struct work_struct work;
	int id;
	int data_avail;
	u8 *rx_dma_buf;
};

struct tty_device {
	struct device *qsb_device;
	struct class *qsb_class;
};

static struct qti_sdio_bridge *qsbdev[MAX_CLIENTS];

static int kworker_refs_count;
struct task_struct *task;
static struct completion read_complete;
struct workqueue_struct *qsb_wq;
static atomic_t status;

static void data_avail_worker(struct work_struct *work)
{
	struct data_avail_node *data_node = container_of(work,
						struct data_avail_node, work);
	struct qti_sdio_bridge *qsb = NULL;

	mutex_lock(&work_lock);
	reinit_completion(&read_complete);

	qsb = qsbdev[data_node->id];
	qsb->rx_dma_buf = data_node->rx_dma_buf;

	/* Publish availability only after rx buffer is fully visible */
	smp_store_release(&qsb->data_avail, data_node->data_avail);

	qlog(qsb, "%s Queuing to read %d %p\n", qsb->name,
				qsb->data_avail, qsb->rx_dma_buf);

	if (qsb->data_avail)
		wake_up(&qsb->wait_q);

	wait_for_completion(&read_complete);
	wcnss_prealloc_put(data_node->rx_dma_buf);
	kfree(data_node);
	mutex_unlock(&work_lock);
}

void qti_client_queue_rx(int id, u8 *buf, unsigned int bytes)
{
	struct data_avail_node *data_node;

	if ((id < QCN_SDIO_CLI_ID_TTY) || (id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s invalid client ID %d\n", __func__, id);
		return;
	}

	data_node = kzalloc(sizeof(struct data_avail_node), GFP_ATOMIC);
	if (!data_node) {
		to_console = 1;
		qlog(qsbdev[id], "client %d dnode allocation failed\n", id);
		to_console = 0;
		return;
	}

	INIT_WORK(&data_node->work, data_avail_worker);

	qlog(qsbdev[id], "%s Queuing to work %d %p\n", qsbdev[id]->name, bytes,
									buf);
	data_node->data_avail = bytes;
	data_node->id = id;
	data_node->rx_dma_buf = buf;

	queue_work(qsb_wq, &data_node->work);
}

void qti_client_ul_xfer_cb(struct sdio_al_channel_handle *ch_handle,
				struct sdio_al_xfer_result *xfer, void *ctxt)
{
	struct qti_sdio_bridge *qsb = NULL;
	struct completion *tx_complete = (struct completion *)ctxt;
	struct sdio_al_client_data *cl_data =
					ch_handle->channel_data->client_data;

	if (!xfer || xfer->xfer_status || !cl_data ||
		(cl_data->id < QCN_SDIO_CLI_ID_TTY) ||
		(cl_data->id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s invalid client ID\n", __func__);
		return;
	}

	qsb = qsbdev[cl_data->id];
	complete(tx_complete);
}

void qti_client_dl_xfer_cb(struct sdio_al_channel_handle *ch_handle,
				struct sdio_al_xfer_result *xfer, void *ctxt)
{
	struct qti_sdio_bridge *qsb = NULL;
	struct sdio_al_client_data *cl_data =
					ch_handle->channel_data->client_data;

	if (!xfer || xfer->xfer_status || !cl_data ||
		(cl_data->id < QCN_SDIO_CLI_ID_TTY) ||
		(cl_data->id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s invalid client ID\n", __func__);
		return;
	}

	qsb = qsbdev[cl_data->id];
	qti_client_queue_rx(cl_data->id, xfer->buf_addr, (int)(uintptr_t)ctxt);
}

void qti_client_data_avail_cb(struct sdio_al_channel_handle *ch_handle,
							unsigned int bytes)
{
	int ret = 0;
	int padded_len = 0;
	u8 *rx_dma_buf = NULL;
	struct qti_sdio_bridge *qsb = NULL;
	struct sdio_al_client_data *cl_data =
					ch_handle->channel_data->client_data;

	if (!cl_data || (cl_data->id < QCN_SDIO_CLI_ID_TTY) ||
			(cl_data->id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s invalid client ID\n", __func__);
		return;
	}

	qsb = qsbdev[cl_data->id];
	qsb->data_avail_count++;
	rx_dma_buf = wcnss_prealloc_get(RX_BUF_SIZE);
	if (!rx_dma_buf) {
		to_console = 1;
		qlog(qsb, "Unable to allocate rx_dma_buf\n");
		to_console = 0;
		return;
	}

        if (bytes < 512) {
		padded_len = bytes;
	}
	else if(bytes % qsb->client_handle->block_size) {
		padded_len = (((bytes /
				qsb->client_handle->block_size) + 1) *
				(qsb->client_handle->block_size));
	} else {
		padded_len = bytes;
	}

	if (qsb->mode) {
		ret = sdio_al_queue_transfer_async(qsb->channel_handle,
						SDIO_AL_RX, rx_dma_buf,
						padded_len, 0,
						(void *)(uintptr_t)bytes);
		if (ret) {
			to_console = 1;
			qlog(qsb, "%s: data queueing failed %d\n", qsb->name,
									ret);
			to_console = 0;
			return;
		}
	} else {
		ret = sdio_al_queue_transfer(qsb->channel_handle,
				SDIO_AL_RX, rx_dma_buf, padded_len, 0);
		if (ret == 1) {
			pr_debug("operating in async mode now\n");
			goto out;
		}
		if (ret) {
			to_console = 1;
			qlog(qsb, "%s: data transfer failed %d\n", qsb->name,
									ret);
			to_console = 0;
			return;
		}
		qti_client_queue_rx(cl_data->id, rx_dma_buf, bytes);
	}
out:
	qlog(qsb, "%s: data %s success\n", qsb->name,
					qsb->mode ? "queueing" : "transfer");
}

static void sdio_dl_meta_data_cb(struct sdio_al_channel_handle *ch_handle,
		unsigned int data)
{
	u8 event = 0;
	struct qti_sdio_bridge *qsb = NULL;
	struct sdio_al_client_data *cl_data =
					ch_handle->channel_data->client_data;

	if (!cl_data || (cl_data->id < QCN_SDIO_CLI_ID_TTY) ||
			(cl_data->id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s invalid client ID\n", __func__);
		return;
	}

	qsb = qsbdev[cl_data->id];

	event = (u8)((data & 0xFF000000) >> 24);

	switch (event) {
	case TTY_RX_BYTE_COUNT:
		qlog(qsb, "client %s data_avail %d\n", qsb->name,
							data & SDIO_META_DATA_BMSK);
		qti_client_data_avail_cb(ch_handle, (data & SDIO_META_DATA_BMSK));
		break;
	case QMI_RX_BYTE_COUNT:
	case DIAG_RX_BYTE_COUNT:
		qlog(qsb, "client %s meta_data %x\n", qsb->name, data);
		break;
	case TTY_RX_BYTE_COUNT_TRANS:
	case QMI_RX_BYTE_COUNT_TRANS:
	case DIAG_RX_BYTE_COUNT_TRANS:
		break;
	case TTY_RX_BYTE_TRANS_MODE:
	case QMI_RX_BYTE_TRANS_MODE:
	case DIAG_RX_BYTE_TRANS_MODE:
		qsb->blk_trans_mode = (data & 0x00000001);
		qlog(qsb, "client %s mode = %d data %x\n", qsb->name,
						qsb->blk_trans_mode, data);
		break;
	case TTY_RX_BYTE_TX_READY:
	case QMI_RX_BYTE_TX_READY:
	case DIAG_RX_BYTE_TX_READY:
		qsb->tx_ready = 1;
		wake_up(&qsb->wait_q);
		qlog(qsb, "client %s tx_ready data = %x\n", qsb->name, data);
		qsb->tx_ready_count++;
		break;
	default:
		to_console = 1;
		qlog(qsb, "client %s INVALID_DATA\n", qsb->name);
		to_console = 0;
	}
}

int qti_client_open(int id, void *ops)
{
	struct qti_sdio_bridge *qsb = NULL;

	qsb = qsbdev[id];

	qlog(qsb, "client %s\n", qsb->name);

	qsb->ops = (struct diag_bridge_ops *)ops;

	return 0;
}
EXPORT_SYMBOL(qti_client_open);

int qti_client_close(int id)
{
	int ret = -ENODEV;
	struct qti_sdio_bridge *qsb = NULL;

	if ((id < QCN_SDIO_CLI_ID_TTY) || (id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s invalid client ID %d\n", __func__, id);
		return ret;
	}

	qsb = qsbdev[id];

	qlog(qsb, "client %s\n", qsb->name);

	qsb->ops = NULL;

	return 0;
}
EXPORT_SYMBOL(qti_client_close);

int qti_client_is_ready(void)
{
	return atomic_read(&status);
}
EXPORT_SYMBOL(qti_client_is_ready);

int qti_client_read(int id, char *buf, size_t count)
{
	int ret = 0;
	int bytes = 0;
	struct qti_sdio_bridge *qsb = NULL;

	if ((id < QCN_SDIO_CLI_ID_TTY) || (id > QCN_SDIO_CLI_ID_DIAG) ||
				atomic_read(&qsbdev[id]->is_client_closing)) {
		pr_err("%s invalid client ID %d\n", __func__, id);
		return -ENODEV;
	}

	qsb = qsbdev[id];
	qlog(qsb, "client %s count %zu\n", qsb->name, count);

	if (id == QCN_SDIO_CLI_ID_DIAG && !qsb->ops) {
		to_console = 1;
		qlog(qsb, "%s: no diag operations assigned\n", qsb->name);
		to_console = 0;
		ret = -ENODEV;
		goto out;
	}

	wait_event(qsb->wait_q, smp_load_acquire(&qsb->data_avail) ||
					atomic_read(&qsb->is_client_closing));
	if (atomic_read(&qsb->is_client_closing)) {
		ret = -ENODEV;
		goto out;
	}

	bytes = qsb->data_avail;

	if (!qsb->data_remain) {
		if (count > bytes)
			count = bytes;

		if (id == QCN_SDIO_CLI_ID_TTY) {
			ret = copy_to_user(buf, qsb->rx_dma_buf, count);
			if (ret) {
				to_console = 1;
				qlog(qsb, "%s: failed to copy to user buffer\n",
								qsb->name);
				to_console = 0;
				return -EIO;
			}
		} else {
			memcpy(buf, qsb->rx_dma_buf, count);
		}
		qsb->data_remain = bytes - count;
	} else {
		if (count > qsb->data_remain)
			count = qsb->data_remain;

		if (id == QCN_SDIO_CLI_ID_TTY) {
			ret = copy_to_user(buf, qsb->rx_dma_buf +
				(bytes - qsb->data_remain), count);
			if (ret) {
				to_console = 1;
				qlog(qsb, "%s: failed to copy to user buffer\n",
						qsb->name);
				to_console = 0;
				return -EIO;
			}
		} else {
			memcpy(buf, qsb->rx_dma_buf +
				(bytes - qsb->data_remain), count);
		}
		qsb->data_remain -= count;
	}
out:
	if (id == QCN_SDIO_CLI_ID_DIAG && qsb->ops &&
						qsb->ops->read_complete_cb) {
		qsb->ops->read_complete_cb((void *)(uintptr_t)0, buf, count,
				ret < 0 ? ret : count);
	}

	if (!qsb->data_remain) {
		qsb->data_avail = 0;
		bytes = 0;
		complete(&read_complete);
	}

	return count;
}
EXPORT_SYMBOL(qti_client_read);

int qti_client_write(int id, char *buf, size_t count)
{
	int ret = 0;
	int remaining = 0;
	int padded_len = 0;
	int temp_count = 0;
	u8 *buffer = NULL;
	unsigned int mdata = 0;
	unsigned int event = 0;
	struct qti_sdio_bridge *qsb = NULL;
	DECLARE_COMPLETION_ONSTACK(tx_complete);

	if ((id < QCN_SDIO_CLI_ID_TTY) || (id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s invalid client ID %d\n", __func__, id);
		return -ENODEV;
	}

	qsb = qsbdev[id];

	if (!qsb) {
		pr_err("%s: invalid qsb for client %d\n", __func__, id);
		return -ENODEV;
	}

	if (atomic_read(&qsbdev[id]->is_client_closing))
		return -ENODEV;

	switch (id) {
	case QCN_SDIO_CLI_ID_TTY:
		event = TTY_TX_BUF_SZ_EVENT;
		break;
	case QCN_SDIO_CLI_ID_QMI:
		event = QMI_TX_BUF_SZ_EVENT;
		break;
	case QCN_SDIO_CLI_ID_DIAG:
		event = DIAG_TX_BUF_SZ_EVENT;
		break;
	default:
		to_console = 1;
		qlog(qsb, "Invalid client\n");
		to_console = 0;
		return ret;
	}

	if (count > TX_BUF_SIZE) {
		qlog(qsb, "%s: write data too large\n", qsb->name);
		return -EINVAL;
	}

	qsb->tx_ready = 0;

	qlog(qsb, "client %s count %zu\n", qsb->name, count);

	if (id == QCN_SDIO_CLI_ID_DIAG && !qsb->ops) {
		to_console = 1;
		qlog(qsb, "%s: no diag operations assigned\n", qsb->name);
		to_console = 0;
		ret = -ENODEV;
		return ret;
	}

	if (id == QCN_SDIO_CLI_ID_TTY) {
		ret = copy_from_user(qsb->tx_dma_buf, buf, count);
		if (ret) {
			qlog(qsb, "%s: failed to copy from user buffer\n",
								qsb->name);
			return ret;
		}
	} else {
		memcpy(qsb->tx_dma_buf, buf, count);
	}

	remaining = count;
	temp_count = count;
	buffer = qsb->tx_dma_buf;

	while (remaining) {
		qsb->tx_ready = 0;
		if (id != QCN_SDIO_CLI_ID_TTY && remaining > 1024) {
			temp_count = remaining;
			remaining = remaining - 1024;
			temp_count = temp_count - remaining;
		} else if (id == QCN_SDIO_CLI_ID_TTY) {
			remaining = 0;
		} else {
			temp_count = remaining;
			remaining = 0;
		}

		if (temp_count < 512) {
			padded_len = temp_count;
		} else if(temp_count % qsb->client_handle->block_size) {
			padded_len =
				(((temp_count / qsb->client_handle->block_size)
				+ 1) * (qsb->client_handle->block_size));
		} else {
			padded_len = temp_count;
		}

		if (temp_count <= 0x3FFF) {
			mdata = ((event << 24) | (temp_count & 0x3FFF));
		} else {
			mdata = ((event << 24) | SDIO_QCN_HRQ_PUSH_BLK_MASK);
			ret = sdio_al_send_host_trans_len(temp_count);
			if (ret) {
				pr_emerg("%s: [Debug_sdio_wlan] sdio_al_fill failed\n",__func__);
			}
		}

		ret = sdio_al_meta_transfer(qsb->channel_handle, mdata, 0);
		if (ret) {
			to_console = 1;
			qlog(qsb, "%s: meta data transfer failed %d\n",
								qsb->name, ret);
			to_console = 0;
			return ret;
		}

		qlog(qsb, "MDATA: %x\n", mdata);
		qsb->mdata_count++;

		ret = wait_event_timeout(qsb->wait_q, qsb->tx_ready ||
					 atomic_read(&qsb->is_client_closing),
					 msecs_to_jiffies(2000));
		if (atomic_read(&qsb->is_client_closing)) {
			to_console = 1;
			qlog(qsb, "%s: client closed \n", qsb->name);
			to_console = 0;
			ret = -ENODEV;
			goto out;
		}

		if (!ret) {
			to_console = 1;
			qlog(qsb, "%s: MDTA: 0x%x TX ready event timed out \n",
			     qsb->name, mdata);
			to_console = 0;
			ret = -ETIMEDOUT;
			goto out;
		}

		if (qsb->mode) {
			reinit_completion(&tx_complete);
			ret = sdio_al_queue_transfer_async(qsb->channel_handle,
							SDIO_AL_TX, buffer,
							padded_len, 0,
							(void *)&tx_complete);
			if (ret) {
				to_console = 1;
				qlog(qsb, "%s: data transfer failed %d\n",
								qsb->name, ret);
				to_console = 0;
				return ret;
			}

			if (qsb->mode)
				wait_for_completion(&tx_complete);
		} else {

			ret = sdio_al_queue_transfer(qsb->channel_handle,
					SDIO_AL_TX, buffer, padded_len, 0);
			if (ret) {
				to_console = 1;
				qlog(qsb, "%s: data transfer failed %d\n",
								qsb->name, ret);
				to_console = 0;
				return ret;
			}
		}
		buffer = buffer + temp_count;
	}

out:
	if (id == QCN_SDIO_CLI_ID_DIAG && qsb->ops &&
			qsb->ops->write_complete_cb) {
		qsb->ops->write_complete_cb((void *)(uintptr_t)0, buf, count,
				ret < 0 ? ret : count);
	}

	return count;
}
EXPORT_SYMBOL(qti_client_write);

static int qsb_dev_open(struct inode *inode, struct file *file)
{
	if (atomic_read(&inode->i_count) != 1)
		return -EBUSY;

	return qti_client_open(QCN_SDIO_CLI_ID_TTY, NULL);
}

static ssize_t qsb_dev_read(struct file *file, char __user *buf, size_t count,
		loff_t *ppos)
{
	return qti_client_read(QCN_SDIO_CLI_ID_TTY, (char *)buf, count);
}

static ssize_t qsb_dev_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	return qti_client_write(QCN_SDIO_CLI_ID_TTY, (char *)buf, count);
}

static int qsb_dev_release(struct inode *inode, struct file *file)
{
	return qti_client_close(QCN_SDIO_CLI_ID_TTY);
}

static
unsigned int qsb_dev_poll(struct file *file, struct poll_table_struct *wait)
{
	int ret = 0;
	struct qti_sdio_bridge *qsb = NULL;

	qsb = qsbdev[QCN_SDIO_CLI_ID_TTY];

	if (!qsb->data_avail)
		poll_wait(file, &qsb->wait_q, wait);

	if (qsb->data_avail)
		ret = POLLIN | POLLRDNORM;

	return ret;
}

static const struct ipc_bridge_platform_data ipc_bridge_pdata = {
	.max_read_size = IPC_BRIDGE_MAX_READ_SZ,
	.max_write_size = IPC_BRIDGE_MAX_WRITE_SZ,
	.open = qti_client_open,
	.read = qti_client_read,
	.write = qti_client_write,
	.close = qti_client_close,
};

static const struct file_operations qsb_dev_ops = {
	.open = qsb_dev_open,
	.read = qsb_dev_read,
	.write = qsb_dev_write,
	.release = qsb_dev_release,
	.poll = qsb_dev_poll,
};

int qti_client_debug_init(int id)
{
	int ret = -EINVAL;
	char name[32] = {0};
	struct qti_sdio_bridge *qsb = NULL;

	if ((id < QCN_SDIO_CLI_ID_TTY) || (id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s : invalid client ID %d\n", __func__, id);
		return ret;
	}

	qsb = qsbdev[id];

	snprintf(name, sizeof(name), "%s_%s", "qcn_client",
			(char *)(qsb->name + 15));

	qsb->ipc_log_ctxt = ipc_log_context_create(QCN_IPC_LOG_PAGES, name, 0);
	if (!qsb->ipc_log_ctxt) {
		pr_err("failed to initialize ipc logging for client_%d", id);
		goto out;
	}

	return 0;
out:
	return ret;
}

void qti_client_debug_deinit(int id)
{
	struct qti_sdio_bridge *qsb = NULL;

	if ((id < QCN_SDIO_CLI_ID_TTY) || (id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s : invalid client ID %d\n", __func__, id);
		return;
	}

	qsb = qsbdev[id];

	if (qsb && qsb->ipc_log_ctxt) {
		ipc_log_context_destroy(qsb->ipc_log_ctxt);
		qsb->ipc_log_ctxt = NULL;
		qsb = NULL;
	}
}

static int qti_client_probe(struct sdio_al_client_handle *client_handle)
{
	int ret = -EINVAL;
	int diag_ch = QCN_SDIO_CLI_ID_DIAG;
	struct platform_device *ipc_pdev = NULL;
	struct platform_device *diag_pdev = NULL;
	struct qti_sdio_bridge *qsb = NULL;
	struct sdio_al_channel_handle *channel_handle = NULL;
	struct sdio_al_channel_data *channel_data = NULL;
	unsigned int event = 0;
	unsigned int mdata = 0;

	if ((client_handle->id < QCN_SDIO_CLI_ID_TTY) ||
			(client_handle->id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s : invalid client ID %d\n", __func__,
							client_handle->id);
		goto err;
	}

	qsb = qsbdev[client_handle->id];

	qlog(qsb, "probing client %s\n", qsb->name);

	channel_data = kzalloc(sizeof(struct sdio_al_channel_data), GFP_KERNEL);
	if (!channel_data) {
		to_console = 1;
		qlog(qsb, "client %s failed to allocate channel_data\n",
								qsb->name);
		to_console = 0;
		ret = -ENOMEM;
		goto err;
	}

	channel_data->name = kasprintf(GFP_KERNEL, qsb->ch_name);
	channel_data->client_data = client_handle->client_data;
	channel_data->dl_meta_data_cb = sdio_dl_meta_data_cb;

	if (client_handle->id != QCN_SDIO_CLI_ID_TTY)
		channel_data->dl_data_avail_cb = qti_client_data_avail_cb;
	else
		channel_data->dl_data_avail_cb = NULL;

	channel_data->ul_xfer_cb = qti_client_ul_xfer_cb;
	channel_data->dl_xfer_cb = qti_client_dl_xfer_cb;

	channel_handle = sdio_al_register_channel(client_handle, channel_data);
	if (IS_ERR(channel_handle)) {
		ret = PTR_ERR(channel_handle);
		to_console = 1;
		qlog(qsb,
		       "client %s failed to register channel_handle ret = %d\n",
								qsb->name, ret);
		to_console = 0;
		goto channel_data_err;
	}

	qsb->channel_handle = channel_handle;

	qsb->tx_dma_buf = wcnss_prealloc_get(TX_BUF_SIZE);
	if (!qsb->tx_dma_buf) {
		to_console = 1;
		qlog(qsb, "client %s failed to allocate tx_buf\n", qsb->name);
		to_console = 0;
		ret = -ENOMEM;
		goto channel_handle_err;
	}

	if (client_handle->id == QCN_SDIO_CLI_ID_TTY) {
		event = TTY_SAHARA_DOORBELL_EVENT;
		mdata = (event << 24);
		ret = sdio_al_meta_transfer(qsb->channel_handle, mdata, 0);
		if (ret)
			goto channel_handle_err;
		qlog(qsb, "TTY_SAHARA_DOORBELL_EVENT sent");
	}

	if (client_handle->id == QCN_SDIO_CLI_ID_QMI) {
		ipc_pdev = platform_device_alloc("ipc_bridge_sdio",
							QCN_SDIO_CLI_ID_QMI);
		if (!ipc_pdev) {
			to_console = 1;
			qlog(qsb, "unable to allocate platform device\n");
			to_console = 0;
			ret = -ENOMEM;
			goto tx_err;
		}

		ret = platform_device_add_data(ipc_pdev, &ipc_bridge_pdata,
				sizeof(struct ipc_bridge_platform_data));
		if (ret) {
			to_console = 1;
			qlog(qsb, "failed to add pdata\n");
			to_console = 0;
			goto put_pdev;
		}

		ret = platform_device_add(ipc_pdev);
		if (ret) {
			to_console = 1;
			qlog(qsb, "failed to add ipc_pdev\n");
			to_console = 0;
			goto put_pdev;
		}
		qsb->priv_dev_info = ipc_pdev;
	}

	if (client_handle->id == QCN_SDIO_CLI_ID_DIAG) {
		diag_pdev = platform_device_register_data(NULL,
				"diag_bridge_sdio", 0, &diag_ch, sizeof(int));
		if (IS_ERR(diag_pdev)) {
			to_console = 1;
			qlog(qsb, "%s: unable to allocate platform device\n",
								__func__);
			to_console = 0;
			ret = PTR_ERR(diag_pdev);
			goto put_pdev;
		}
		qsb->priv_dev_info = diag_pdev;
	}

	atomic_set(&qsb->is_client_closing, 0);
	qlog(qsb, "probed client %s\n", qsb->name);
	return 0;

put_pdev:
	if (client_handle->id == QCN_SDIO_CLI_ID_QMI)
		platform_device_put(ipc_pdev);

tx_err:
	wcnss_prealloc_put(qsb->tx_dma_buf);
channel_handle_err:
	sdio_al_deregister_channel(channel_handle);
channel_data_err:
	kfree(channel_data);
err:
	pr_err("probe failed for client %d\n", client_handle->id);
	return ret;
}

static int qti_client_remove(struct sdio_al_client_handle *client_handle)
{
	struct qti_sdio_bridge *qsb = NULL;
	int ret = -EINVAL;

	if ((client_handle->id < QCN_SDIO_CLI_ID_TTY) ||
			(client_handle->id > QCN_SDIO_CLI_ID_DIAG)) {
		pr_err("%s : invalid client ID %d\n", __func__,
							client_handle->id);
		goto err;
	}

	qsb = qsbdev[client_handle->id];

	atomic_set(&qsb->is_client_closing, 1);
	wake_up(&qsb->wait_q);

	if (client_handle->id == QCN_SDIO_CLI_ID_QMI)
		platform_device_unregister(qsb->priv_dev_info);

	if (client_handle->id == QCN_SDIO_CLI_ID_DIAG)
		platform_device_unregister(qsb->priv_dev_info);

	qlog(qsb, "removed client %s\n", qsb->name);
	wcnss_prealloc_put(qsb->tx_dma_buf);
	qti_client_debug_deinit(client_handle->id);
	return 0;

err:
	pr_err("%s : failed to removed client %d\n", __func__,
							client_handle->id);
	return ret;
}

static int qti_bridge_probe(struct platform_device *pdev)
{
	struct sdio_al_client_data *client_data = NULL;
	struct sdio_al_client_handle *client_handle = NULL;
	struct tty_device *tty_dev = NULL;
	struct qti_sdio_bridge *qsb = NULL;
	int ret = -EPROBE_DEFER;
	int major_no = 0;
	int id = 0;

	ret = sdio_al_is_ready();
	if (ret) {
		ret = -EPROBE_DEFER;
		goto out;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,client-id", &id);
	if (ret) {
		pr_err("qcom,client-id not found\n");
		goto out;
	}

	qsbdev[id] = kzalloc(sizeof(struct qti_sdio_bridge), GFP_KERNEL);
	if (!qsbdev[id]) {
		ret = -ENOMEM;
		goto out;
	}

	qsbdev[id]->id = id;

	ret = of_property_read_string(pdev->dev.of_node, "qcom,ch-name",
			&(qsbdev[id]->ch_name));
	if (ret) {
		pr_err("qcom,ch-name not found\n");
		goto out;
	}

	client_data = kzalloc(sizeof(struct sdio_al_client_data), GFP_KERNEL);
	if (!client_data) {
		ret = -ENOMEM;
		goto bridge_alloc_error;
	}

	ret = of_property_read_string(pdev->dev.of_node, "qcom,client-name",
			&client_data->name);
	if (ret) {
		pr_err("qcom,client-name not found\n");
		goto bridge_alloc_error;
	}

	qsbdev[id]->name = kasprintf(GFP_KERNEL, client_data->name);

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,client-mode",
							&client_data->mode);
	if (ret)
		pr_err("qcom,client-mode not set, mode[async] as default\n");

	client_data->probe = qti_client_probe;
	client_data->remove = qti_client_remove;
	client_data->id = id;

	qsbdev[id]->mode = client_data->mode;

	client_handle = sdio_al_register_client(client_data);
	if (IS_ERR(client_handle)) {
		ret = PTR_ERR(client_handle);
		goto client_error;
	}

	if (qsbdev[client_handle->id]->id != client_handle->id) {
		pr_err("probed client %d doesn't match registered client %d\n",
			qsbdev[client_handle->id]->id, client_handle->id);
		goto client_reg_error;
	}

	qti_client_debug_init(client_handle->id);

	qsbdev[client_handle->id]->client_handle = client_handle;

	if (!kworker_refs_count) {
		qsb_wq = alloc_ordered_workqueue("qcn_worker", 0);
		init_completion(&read_complete);
		qcn_sdio_client_probe_complete(client_handle->id);
	}
	++kworker_refs_count;

	qsb = qsbdev[client_handle->id];
	if (client_handle->id == QCN_SDIO_CLI_ID_TTY) {
		init_waitqueue_head(&qsb->wait_q);
		tty_dev = kmalloc(sizeof(struct tty_device), GFP_KERNEL);
		if (!tty_dev) {
			to_console = 1;
			qlog(qsb, "unable to allocate platform device\n");
			to_console = 0;
			ret = -ENOMEM;
			goto out;
		}

		major_no = register_chrdev(UNNAMED_MAJOR, "QCN", &qsb_dev_ops);
		if (major_no < 0) {
			to_console = 1;
			qlog(qsb, "client %s failed to allocate major_no\n",
								qsb->name);
			to_console = 0;
			ret = major_no;
			goto out;
		}

		tty_dev->qsb_class = class_create("qsahara");
		if (IS_ERR(tty_dev->qsb_class)) {
			to_console = 1;
			qlog(qsb, "client %s failed to create class\n",
								qsb->name);
			to_console = 0;
			ret = PTR_ERR(tty_dev->qsb_class);
			goto reg_err;
		}

		tty_dev->qsb_device = device_create(tty_dev->qsb_class, NULL,
					MKDEV(major_no, 0), NULL, "qcn_sdio");
		if (IS_ERR(tty_dev->qsb_device)) {
			to_console = 1;
			qlog(qsb, "client %s failed to create device node\n",
								qsb->name);
			to_console = 0;
			ret = PTR_ERR(tty_dev->qsb_device);

			goto dev_err;
		}
		qsb->priv_dev_info = tty_dev;
	}
	atomic_set(&status, 1);

	return 0;

client_reg_error:
	sdio_al_deregister_client(client_handle);
client_error:
	kfree(client_data);
bridge_alloc_error:
	kfree(qsbdev[id]);
dev_err:
	if (tty_dev)
		class_destroy(tty_dev->qsb_class);
reg_err:
	unregister_chrdev(major_no, "qsahara");
out:
	return ret;
}

static void qti_bridge_remove(struct platform_device *pdev)
{
	struct qti_sdio_bridge *qsb = NULL;
	struct tty_device *tty_dev = NULL;
	int minor_no = 0;
	int major_no = 0;
	int ret = -EBUSY;
	int id = 0;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,client-id", &id);
	if (ret) {
		pr_err("%s: qcom,client-id not found\n", __func__);
		goto out;
	}

	qsb = qsbdev[id];

	tty_dev = (struct tty_device *)qsb->priv_dev_info;
	if (qsb->client_handle->id == QCN_SDIO_CLI_ID_TTY &&
	    tty_dev->qsb_device) {
		minor_no = MINOR(tty_dev->qsb_device->devt);
		major_no = MAJOR(tty_dev->qsb_device->devt);
		device_destroy(tty_dev->qsb_class, MKDEV(major_no, minor_no));
		class_destroy(tty_dev->qsb_class);
		unregister_chrdev(major_no, "qsahara");
		tty_dev->qsb_class = NULL;
		tty_dev->qsb_device = NULL;
		major_no = 0;
	}

	--kworker_refs_count;
	if (!kworker_refs_count) {
		flush_workqueue(qsb_wq);
		destroy_workqueue(qsb_wq);
	}

	sdio_al_deregister_client(qsb->client_handle);
	atomic_set(&status, 0);
	kfree(qsb);

out:
	return;
}

static const struct of_device_id qti_sdio_bridge_of_match[] = {
	{.compatible	= "qcom,sdio-bridge"},
	{}
};
MODULE_DEVICE_TABLE(of, qti_sdio_bridge_of_match);

static struct platform_driver qti_sdio_bridge_driver = {
	.probe	= qti_bridge_probe,
	.remove	= qti_bridge_remove,
	.driver	= {
		.name	= "sdio_bridge",
		.owner	= THIS_MODULE,
		.of_match_table	= qti_sdio_bridge_of_match,
	},
};

module_platform_driver(qti_sdio_bridge_driver);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc.");
MODULE_LICENSE("GPL v2");
