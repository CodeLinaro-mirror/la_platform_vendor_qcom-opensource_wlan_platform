// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2016-2021, The Linux Foundation. All rights reserved. */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/ipc_logging.h>
#include <linux/debugfs.h>
#include "main.h"
#include "debug.h"

#define DEFAULT_KERNEL_LOG_LEVEL		INFO_LOG
#define DEFAULT_IPC_LOG_LEVEL			DEBUG_LOG

enum log_level cnss_kernel_log_level = DEFAULT_KERNEL_LOG_LEVEL;

#if IS_ENABLED(CONFIG_IPC_LOGGING)
void *cnss_ipc_log_context;
void *cnss_ipc_log_long_context;
enum log_level cnss_ipc_log_level = DEFAULT_IPC_LOG_LEVEL;
#endif

static u64 cnss_get_serial_id(struct cnss_plat_data *plat_priv)
{
	u32 msb = plat_priv->serial_id.serial_id_msb;
	u32 lsb = plat_priv->serial_id.serial_id_lsb;

	msb &= 0xFFFF;
	return (((u64)msb << 32) | lsb);
}

static int cnss_stats_show_state(struct seq_file *s,
				 struct cnss_plat_data *plat_priv)
{
	enum cnss_driver_state i;
	int skip = 0;
	unsigned long state;

	seq_printf(s, "\nSerial Number: 0x%llx",
		   cnss_get_serial_id(plat_priv));
	seq_printf(s, "\nState: 0x%lx(", plat_priv->driver_state);
	for (i = 0, state = plat_priv->driver_state; state != 0;
	     state >>= 1, i++) {
		if (!(state & 0x1))
			continue;

		if (skip++)
			seq_puts(s, " | ");

		switch (i) {
		case CNSS_DRIVER_LOADING:
			seq_puts(s, "DRIVER_LOADING");
			continue;
		case CNSS_DRIVER_UNLOADING:
			seq_puts(s, "DRIVER_UNLOADING");
			continue;
		case CNSS_DRIVER_PROBING:
			seq_puts(s, "DRIVER PROBING");
			continue;
		case CNSS_DRIVER_PROBED:
			seq_puts(s, "DRIVER_PROBED");
			continue;
		case CNSS_DEV_REMOVED:
			seq_puts(s, "DEV REMOVED");
			continue;
		case CNSS_DRIVER_IDLE_RESTART:
			seq_puts(s, "IDLE RESTART");
			continue;
		case CNSS_DRIVER_IDLE_SHUTDOWN:
			seq_puts(s, "IDLE SHUTDOWN");
			continue;
		case CNSS_DRIVER_SHUTDOWN:
			seq_puts(s, "DRIVER SHUTDOWN");
			continue;
		case CNSS_DEVICE_RESET:
			seq_puts(s, "DEVICE RESET");
			continue;
		case CNSS_DRIVER_REINIT:
			seq_puts(s, "DRIVER REINIT");
			continue;
		case CNSS_IN_REBOOT:
			seq_puts(s, "SYSTEM REBOOT");
			continue;
		case CNSS_DRIVER_RECOVERING:
			seq_puts(s, "SYSTEM RECOVERING");
			continue;
		case CNSS_SSR_DUMP_IN_PROGRESS:
			seq_puts(s, "SSR DUMP IN PROGRESS");
			continue;
		case CNSS_POWER_OFF:
			seq_puts(s, "POWER OFF");
			continue;
		case CNSS_DRIVER_FW_SSR_IN_PROGRESS:
			seq_puts(s, "FW SSR IN PROGRESS");
			continue;
		case CNSS_DRIVER_SMC_SSR_IN_PROGRESS:
			seq_puts(s, "SMC SSR IN PROGRESS");
			continue;
		}

		seq_printf(s, "UNKNOWN-%d", i);
	}
	seq_puts(s, ")\n");

	return 0;
}

static int cnss_stats_show(struct seq_file *s, void *data)
{
	struct cnss_plat_data *plat_priv = s->private;

	cnss_stats_show_state(s, plat_priv);

	return 0;
}

static int cnss_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, cnss_stats_show, inode->i_private);
}

static const struct file_operations cnss_stats_fops = {
	.read		= seq_read,
	.release	= single_release,
	.open		= cnss_stats_open,
	.owner		= THIS_MODULE,
	.llseek		= seq_lseek,
};

#ifdef CONFIG_DEBUG_FS
int cnss_debugfs_create(struct cnss_plat_data *plat_priv)
{
	int ret = 0;
	struct dentry *root_dentry;
	char name[CNSS_FS_NAME_SIZE];

	if (cnss_is_dual_wlan_enabled())
		snprintf(name, CNSS_FS_NAME_SIZE, CNSS_FS_NAME "_%d",
			 plat_priv->plat_idx);
	else
		snprintf(name, CNSS_FS_NAME_SIZE, CNSS_FS_NAME);

	root_dentry = debugfs_create_dir(name, 0);
	if (IS_ERR(root_dentry)) {
		ret = PTR_ERR(root_dentry);
		cnss_pr_err("Unable to create debugfs %d\n", ret);
		goto out;
	}

	plat_priv->root_dentry = root_dentry;

	debugfs_create_file("stats", 0644, root_dentry, plat_priv,
			    &cnss_stats_fops);

out:
	return ret;
}

void cnss_debugfs_destroy(struct cnss_plat_data *plat_priv)
{
	debugfs_remove_recursive(plat_priv->root_dentry);
}
#else
int cnss_debugfs_create(struct cnss_plat_data *plat_priv)
{
	plat_priv->root_dentry = NULL;
	return 0;
}

void cnss_debugfs_destroy(struct cnss_plat_data *plat_priv)
{
}
#endif

#if IS_ENABLED(CONFIG_IPC_LOGGING)
void cnss_debug_ipc_log_print(void *log_ctx, char *process, const char *fn,
			      enum log_level kern_log_level,
			      enum log_level ipc_log_level, char *fmt, ...)
{
	struct va_format vaf;
	va_list va_args;

	va_start(va_args, fmt);
	vaf.fmt = fmt;
	vaf.va = &va_args;

	if (kern_log_level <= cnss_kernel_log_level) {
		switch (kern_log_level) {
		case EMERG_LOG:
			pr_emerg("cnss: %pV", &vaf);
			break;
		case ALERT_LOG:
			pr_alert("cnss: %pV", &vaf);
			break;
		case CRIT_LOG:
			pr_crit("cnss: %pV", &vaf);
			break;
		case ERR_LOG:
			pr_err("cnss: %pV", &vaf);
			break;
		case WARNING_LOG:
			pr_warn("cnss: %pV", &vaf);
			break;
		case NOTICE_LOG:
			pr_notice("cnss: %pV", &vaf);
			break;
		case INFO_LOG:
			pr_info("cnss: %pV", &vaf);
			break;
		case DEBUG_LOG:
		case DEBUG_HI_LOG:
			pr_debug("cnss: %pV", &vaf);
			break;
		default:
			break;
		}
	}

	if (ipc_log_level <= cnss_ipc_log_level)
		ipc_log_string(log_ctx, "[%s] %s: %pV", process, fn, &vaf);

	va_end(va_args);
}

static int cnss_ipc_logging_init(void)
{
	cnss_ipc_log_context = ipc_log_context_create(CNSS_IPC_LOG_PAGES,
						      "cnss", 0);
	if (!cnss_ipc_log_context) {
		cnss_pr_err("Unable to create IPC log context\n");
		return -EINVAL;
	}

	cnss_ipc_log_long_context = ipc_log_context_create(CNSS_IPC_LOG_PAGES,
							   "cnss-long", 0);
	if (!cnss_ipc_log_long_context) {
		cnss_pr_err("Unable to create IPC long log context\n");
		ipc_log_context_destroy(cnss_ipc_log_context);
		return -EINVAL;
	}

	return 0;
}

static void cnss_ipc_logging_deinit(void)
{
	if (cnss_ipc_log_long_context) {
		ipc_log_context_destroy(cnss_ipc_log_long_context);
		cnss_ipc_log_long_context = NULL;
	}

	if (cnss_ipc_log_context) {
		ipc_log_context_destroy(cnss_ipc_log_context);
		cnss_ipc_log_context = NULL;
	}
}
#else
static int cnss_ipc_logging_init(void) { return 0; }
static void cnss_ipc_logging_deinit(void) {}
void cnss_debug_ipc_log_print(void *log_ctx, char *process, const char *fn,
			      enum log_level kern_log_level,
			      enum log_level ipc_log_level, char *fmt, ...)
{
	struct va_format vaf;
	va_list va_args;

	va_start(va_args, fmt);
	vaf.fmt = fmt;
	vaf.va = &va_args;

	if (kern_log_level <= cnss_kernel_log_level) {
		switch (kern_log_level) {
		case EMERG_LOG:
			pr_emerg("cnss: %pV", &vaf);
			break;
		case ALERT_LOG:
			pr_alert("cnss: %pV", &vaf);
			break;
		case CRIT_LOG:
			pr_crit("cnss: %pV", &vaf);
			break;
		case ERR_LOG:
			pr_err("cnss: %pV", &vaf);
			break;
		case WARNING_LOG:
			pr_warn("cnss: %pV", &vaf);
			break;
		case NOTICE_LOG:
			pr_notice("cnss: %pV", &vaf);
			break;
		case INFO_LOG:
			pr_info("cnss: %pV", &vaf);
			break;
		case DEBUG_LOG:
		case DEBUG_HI_LOG:
			pr_debug("cnss: %pV", &vaf);
			break;
		default:
			break;
		}
	}

	va_end(va_args);
}

#endif

int cnss_debug_init(void)
{
	return cnss_ipc_logging_init();
}

void cnss_debug_deinit(void)
{
	cnss_ipc_logging_deinit();
}
