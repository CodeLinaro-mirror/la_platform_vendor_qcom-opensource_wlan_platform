// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <net/genetlink.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>


#include "main.h"
#include "genl.h"
#include "debug.h"
#include "sdio.h"
#include "bus.h"

#define CNSS_GENL_FAMILY_NAME "cnss-genl"
#define CNSS_GENL_MCAST_GROUP_NAME "cnss-genl-grp"
#define CNSS_GENL_VERSION 1
#define CNSS_GENL_DATA_LEN_MAX (15 * 1024)
#define CNSS_GENL_STR_LEN_MAX 16
#define CNSS_GENL_SEND_RETRY_COUNT 10
#define CNSS_GENL_SEND_RETRY_DELAY 200

enum {
	CNSS_GENL_ATTR_MSG_UNSPEC,
	CNSS_GENL_ATTR_MSG_TYPE,
	CNSS_GENL_ATTR_MSG_FILE_NAME,
	CNSS_GENL_ATTR_MSG_TOTAL_SIZE,
	CNSS_GENL_ATTR_MSG_SEG_ID,
	CNSS_GENL_ATTR_MSG_END,
	CNSS_GENL_ATTR_MSG_DATA_LEN,
	CNSS_GENL_ATTR_MSG_DATA,
	CNSS_GENL_ATTR_MSG_INSTANCE_ID,
	CNSS_GENL_ATTR_MSG_VALUE,
	CNSS_GENL_ATTR_MSG_BIN_ID,
	__CNSS_GENL_ATTR_MAX,
};

#define CNSS_GENL_ATTR_MAX (__CNSS_GENL_ATTR_MAX - 1)

enum {
	CNSS_GENL_CMD_UNSPEC,
	CNSS_GENL_CMD_MSG,
	__CNSS_GENL_CMD_MAX,
};

#define CNSS_GENL_CMD_MAX (__CNSS_GENL_CMD_MAX - 1)

static struct nla_policy cnss_genl_msg_policy[CNSS_GENL_ATTR_MAX + 1] = {
	[CNSS_GENL_ATTR_MSG_TYPE] = { .type = NLA_U8 },
	[CNSS_GENL_ATTR_MSG_FILE_NAME] = { .type = NLA_NUL_STRING,
					   .len = CNSS_GENL_STR_LEN_MAX },
	[CNSS_GENL_ATTR_MSG_TOTAL_SIZE] = { .type = NLA_U32 },
	[CNSS_GENL_ATTR_MSG_SEG_ID] = { .type = NLA_U32 },
	[CNSS_GENL_ATTR_MSG_END] = { .type = NLA_U8 },
	[CNSS_GENL_ATTR_MSG_VALUE] = { .type = NLA_U32 },
	[CNSS_GENL_ATTR_MSG_DATA_LEN] = { .type = NLA_U32 },
	[CNSS_GENL_ATTR_MSG_DATA] = { .type = NLA_BINARY,
				      .len = CNSS_GENL_DATA_LEN_MAX },
	[CNSS_GENL_ATTR_MSG_BIN_ID] =  { .type = NLA_U32 },
};

static struct genl_ops cnss_genl_ops[] = {
	{
		.cmd = CNSS_GENL_CMD_MSG,
		.doit = cnss_genl_process_msg,
		.policy = cnss_genl_msg_policy,
		.maxattr =  CNSS_GENL_ATTR_MAX,
		.flags = 0,
	},
};

static struct genl_multicast_group cnss_genl_mcast_grp[] = {
	{
		.name = CNSS_GENL_MCAST_GROUP_NAME,
	},
};

static struct genl_family cnss_genl_family = {
	.id = 0,
	.hdrsize = 0,
	.name = CNSS_GENL_FAMILY_NAME,
	.version = CNSS_GENL_VERSION,
	.maxattr = CNSS_GENL_ATTR_MAX,
	.policy = cnss_genl_msg_policy,
	.module = THIS_MODULE,
	.ops = cnss_genl_ops,
	.n_ops = ARRAY_SIZE(cnss_genl_ops),
	.mcgrps = cnss_genl_mcast_grp,
	.n_mcgrps = ARRAY_SIZE(cnss_genl_mcast_grp),
};

static char *err_type_to_str(err_type type)
{
	switch (type) {
	case ERR_NONE_S:
		return "ERR_NONE_S";
	case ERR_EXCEPT:
		return "ERR_EXCEPT";
	case ERR_ASSERT:
		return "ERR_ASSERT";
	case ERR_NMI:
		return "ERR_NMI";
	case ERR_NMI_EXT:
		return "ERR_NMI_EXT";
	case ERR_B_TRAN_E:
		return "ERR_B_TRAN_E";
	case ERR_B_TIMEOUT:
		return "ERR_B_TIMEOUT";
	case ERR_M_CORRUPT:
		return "ERR_M_CORRUPT";
	default:
		return "ERR_UNRESOLVE";
	}
};

void cnss_parse_fw_bin(struct meson_dump_data *meson_core)
{
	coredump *core_data;
	mthr mth_data;

	if (!meson_core || !meson_core->meson_dump_bin) {
		cnss_pr_err("Invalid dump data\n");
		return;
	}

	if (meson_core->meson_dump_size < sizeof(coredump)) {
		cnss_pr_err("Insufficient dump data size\n");
		return;
	}

	core_data = (coredump *)meson_core->meson_dump_bin;
	mth_data = core_data->regs.mth;

	if (core_data->type == ERR_ASSERT) {
		cnss_pr_err("[CNSS_FW_ASSERT] %s: in function %s line#%u\n",
				err_type_to_str(core_data->type),
				core_data->assert.file, core_data->assert.line);
	} else {
		cnss_pr_err("[CNSS_FW_ASSERT] %s: Exception inst=0x%08x "
			"cause=0x%08x lr=0x%08x tval=0x%08x dcause=0x%08x",
			err_type_to_str(core_data->type), mth_data.mepc,
			mth_data.mcause, core_data->regs.gen.ra, mth_data.mtval,
			mth_data.mdcause);
	}
}

static void cnss_log_dump_region_pages(const char *file_name,
					void *va, size_t total_size)
{
	unsigned long nr_pages = (total_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned long pg;
	char base_name[CNSS_GENL_STR_LEN_MAX + 1];
	phys_addr_t *pa_table;
	phys_addr_t tbl_pa;

	strscpy(base_name, file_name, sizeof(base_name));

	pa_table = kmalloc_array(nr_pages, sizeof(phys_addr_t), GFP_KERNEL);
	if (!pa_table) {
			cnss_pr_err("failed to alloc pa_table for %s\n", base_name);
			return;
	}

	for (pg = 0; pg < nr_pages; pg++)
			pa_table[pg] =
					(phys_addr_t)vmalloc_to_pfn(va + (pg << PAGE_SHIFT)) << PAGE_SHIFT;

	tbl_pa = virt_to_phys(pa_table);

	cnss_pr_info("Dump region: %s, size: 0x%zx, pa_table: %pa, tbl_size: 0x%zx\n",
							base_name, total_size,
							&tbl_pa, nr_pages * sizeof(phys_addr_t));
	/* pa_table intentionally not freed: must stay in DDR until crash dump is captured */
}

int cnss_genl_process_msg(struct sk_buff *skb, struct genl_info *info)
{
	struct nlmsghdr *nl_header = nlmsg_hdr(skb);
	struct genlmsghdr *genl_header = nlmsg_data(nl_header);
	struct nlattr *attrs[CNSS_GENL_ATTR_MAX + 1];
	struct cnss_hang_event hang_event;
	struct cnss_plat_data *plat_data;
	struct cnss_sdio_data *sdio_priv;
	int ret = 0;
	u8 type = 0, end;
	u32 value = 0, total_size = 0, seg_id, data_len, bin_id;
	void *msg_buf = NULL;
	char *file_name = "NULL";
	plat_data = cnss_bus_dev_to_plat_priv(NULL);

	if (!plat_data)
		return -EINVAL;
	sdio_priv = plat_data->bus_priv;

	if (!info->genlhdr) {
		cnss_pr_err("%s: Invalid genl header\n", __func__);
		return -EINVAL;
	}

	if (genl_header->cmd != CNSS_GENL_CMD_MSG) {
		cnss_pr_err("%s: Invalid cmd %d on NL", __func__, genl_header->cmd);
		return -EINVAL;
	}

	ret = genlmsg_parse(nl_header, &cnss_genl_family, attrs,
						CNSS_GENL_ATTR_MAX, NULL, NULL);
	if (ret < 0) {
		cnss_pr_err("%s: RX NLMSG: Parse fail %d", __func__, ret);
		return -EINVAL;
	}

	if (!info->attrs[CNSS_GENL_ATTR_MSG_TYPE]		||
		!info->attrs[CNSS_GENL_ATTR_MSG_FILE_NAME]	||
		!info->attrs[CNSS_GENL_ATTR_MSG_TOTAL_SIZE]	||
		!info->attrs[CNSS_GENL_ATTR_MSG_SEG_ID]     ||
		!info->attrs[CNSS_GENL_ATTR_MSG_END]        ||
		!info->attrs[CNSS_GENL_ATTR_MSG_VALUE]      ||
		!info->attrs[CNSS_GENL_ATTR_MSG_DATA]		||
		!info->attrs[CNSS_GENL_ATTR_MSG_BIN_ID]		||
		!info->attrs[CNSS_GENL_ATTR_MSG_DATA_LEN]) {
		cnss_pr_err("missing required attributes\n");
		return -EINVAL;
	}

	type 		= nla_get_u8(info->attrs[CNSS_GENL_ATTR_MSG_TYPE]);
	file_name  	= nla_data(info->attrs[CNSS_GENL_ATTR_MSG_FILE_NAME]);
	total_size	= nla_get_u32(info->attrs[CNSS_GENL_ATTR_MSG_TOTAL_SIZE]);
	seg_id		= nla_get_u32(info->attrs[CNSS_GENL_ATTR_MSG_SEG_ID]);
	end 		= nla_get_u8(info->attrs[CNSS_GENL_ATTR_MSG_END]);
	data_len	= nla_get_u32(info->attrs[CNSS_GENL_ATTR_MSG_DATA_LEN]);
	value 		= nla_get_u32(info->attrs[CNSS_GENL_ATTR_MSG_VALUE]);
	msg_buf		= nla_data(info->attrs[CNSS_GENL_ATTR_MSG_DATA]);
	bin_id		= nla_get_u32(info->attrs[CNSS_GENL_ATTR_MSG_BIN_ID]);

	if (!msg_buf) {
		cnss_pr_err("No file data received\n");
		return -EINVAL;
	}

	if (nla_len(info->attrs[CNSS_GENL_ATTR_MSG_DATA]) != data_len) {
		cnss_pr_err("payload length mismatch\n");
		return -EINVAL;
	}

	if (bin_id >= MAX_MESON_BINS) {
		cnss_pr_err("Invalid bin_id %u, max is %d\n", bin_id, MAX_MESON_BINS - 1);
		return -EINVAL;
	}

	if (seg_id == 0) {
		/* Free any previously allocated buffer for this bin_id */
		if (plat_data->dump_data[bin_id].meson_dump_bin)
			vfree(plat_data->dump_data[bin_id].meson_dump_bin);

		plat_data->dump_data[bin_id].meson_dump_bin = vzalloc(total_size);
		plat_data->dump_data[bin_id].meson_dump_size = 0;

		if (!plat_data->dump_data[bin_id].meson_dump_bin) {
			cnss_pr_err("failed to allocate bufer\n");
			return -ENOMEM;
		}
	}

	/* Copy segment into buffer at computed offset */
	loff_t offset = (loff_t)seg_id * (loff_t)CNSS_GENL_DATA_LEN_MAX;

	/* Prevent overflow beyond expected total_size */
	if (offset + data_len > total_size) {
		cnss_pr_err("offset beyond total_size\n");
		vfree(plat_data->dump_data[bin_id].meson_dump_bin);
		plat_data->dump_data[bin_id].meson_dump_bin = NULL;
		return -EINVAL;
	}

	memcpy(plat_data->dump_data[bin_id].meson_dump_bin + offset, msg_buf, data_len);
	plat_data->dump_data[bin_id].meson_dump_size += data_len;

	if (end) {
		if (plat_data->dump_data[bin_id].meson_dump_size == total_size) {
			cnss_pr_err("file '%s' received completely (%u bytes)\n",
						file_name, total_size);
			if (!plat_data->recovery_enabled)
				cnss_log_dump_region_pages(file_name,
					plat_data->dump_data[bin_id].meson_dump_bin, total_size);
		} else {
			cnss_pr_err("received incomplete file '%s': received %u bytes\n", file_name, plat_data->dump_data[bin_id].meson_dump_size);
			vfree(plat_data->dump_data[bin_id].meson_dump_bin);
			plat_data->dump_data[bin_id].meson_dump_bin = NULL;
			return -EINVAL;
		}
	} else {
		return 0;
	}

	if (bin_id == MESON_CORE_BIN) {
		cnss_pr_err("trigger Hang event\n");
		// Trigger SDIO uevent with received data
		memset(&hang_event, 0, sizeof(hang_event));
		hang_event.hang_event_data = msg_buf;
		hang_event.hang_event_data_len = data_len;

		cnss_parse_fw_bin(&plat_data->dump_data[bin_id]);

		cnss_pr_dbg("Calling SDIO uevent with CNSS_HANG_EVENT (with file data).\n");
		cnss_sdio_call_driver_uevent(sdio_priv, CNSS_HANG_EVENT, &hang_event);
	} else if (bin_id == MESON_MWSS_BIN) {
		complete(&hang_event_complete);
	}

    return 0;
}
static int cnss_genl_send_data(u8 type, char *file_name, u32 total_size,
			       u32 seg_id, u8 end, u32 data_len, u8 *msg_buff)
{
	struct sk_buff *skb = NULL;
	void *msg_header = NULL;
	int ret = 0;
	char filename[CNSS_GENL_STR_LEN_MAX + 1];

	cnss_pr_dbg("type: %u, file_name %s, total_size: %x, seg_id %u, end %u, data_len %u\n",
			type, file_name, total_size, seg_id, end, data_len);

	if (!file_name)
		strscpy(filename, "default", sizeof(filename));
	else
		strscpy(filename, file_name, sizeof(filename));

	skb = genlmsg_new(NLMSG_HDRLEN +
			  nla_total_size(sizeof(type)) +
			  nla_total_size(strlen(filename) + 1) +
			  nla_total_size(sizeof(total_size)) +
			  nla_total_size(sizeof(seg_id)) +
			  nla_total_size(sizeof(end)) +
			  nla_total_size(sizeof(data_len)) +
			  nla_total_size(data_len), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	msg_header = genlmsg_put(skb, 0, 0,
				 &cnss_genl_family, 0,
				 CNSS_GENL_CMD_MSG);
	if (!msg_header) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = nla_put_u8(skb, CNSS_GENL_ATTR_MSG_TYPE, type);
	if (ret < 0)
		goto fail;
	ret = nla_put_string(skb, CNSS_GENL_ATTR_MSG_FILE_NAME, filename);
	if (ret < 0)
		goto fail;
	ret = nla_put_u32(skb, CNSS_GENL_ATTR_MSG_TOTAL_SIZE, total_size);
	if (ret < 0)
		goto fail;
	ret = nla_put_u32(skb, CNSS_GENL_ATTR_MSG_SEG_ID, seg_id);
	if (ret < 0)
		goto fail;
	ret = nla_put_u8(skb, CNSS_GENL_ATTR_MSG_END, end);
	if (ret < 0)
		goto fail;
	ret = nla_put_u32(skb, CNSS_GENL_ATTR_MSG_DATA_LEN, data_len);
	if (ret < 0)
		goto fail;
	ret = nla_put(skb, CNSS_GENL_ATTR_MSG_DATA, data_len, msg_buff);
	if (ret < 0)
		goto fail;

	genlmsg_end(skb, msg_header);
	ret = genlmsg_multicast(&cnss_genl_family, skb, 0, 0, GFP_KERNEL);

	return ret;
fail:
	cnss_pr_err("Fail to generate genl msg: %d\n", ret);
	if (skb)
		nlmsg_free(skb);
	return ret;
}

int cnss_genl_send_msg(void *buff, u8 type, char *file_name, u32 total_size)
{
	int ret = 0;
	u8 *msg_buff = buff;
	u32 remaining = total_size;
	u32 seg_id = 0;
	u32 data_len = 0;
	u8 end = 0;
	u8 retry;

	cnss_pr_dbg_buf("type: %u, total_size: %x\n", type, total_size);

    // Handle zero-length data explicitly
	if (total_size == 0) {
		for (retry = 0; retry < CNSS_GENL_SEND_RETRY_COUNT; retry++) {
			ret = cnss_genl_send_data(type, file_name, total_size,
					seg_id, 1, 0, NULL);
			if (ret >= 0)
				break;

			cnss_pr_err("Fail to send zero-length genl msg, try %d: %d\n",
					retry + 1, ret);
			msleep(CNSS_GENL_SEND_RETRY_DELAY);
		}
		return ret;
	}

	while (remaining) {
		if (remaining > CNSS_GENL_DATA_LEN_MAX) {
			data_len = CNSS_GENL_DATA_LEN_MAX;
		} else {
			data_len = remaining;
			end = 1;
		}

		for (retry = 0; retry < CNSS_GENL_SEND_RETRY_COUNT; retry++) {
			ret = cnss_genl_send_data(type, file_name, total_size,
						  seg_id, end, data_len,
						  msg_buff);
			if (ret >= 0)
				break;

			cnss_pr_err("Fail to send genl seg_id %d: %d, try %d\n",
				    seg_id, ret, retry+1);

			msleep(CNSS_GENL_SEND_RETRY_DELAY);
		}

		if (ret < 0) {
			cnss_pr_err("fail to send genl msg, ret %d\n", ret);
			return ret;
		}

		remaining -= data_len;
		msg_buff += data_len;
		seg_id++;
	}

	return ret;
}
EXPORT_SYMBOL(cnss_genl_send_msg);

int cnss_genl_init(void)
{
	int ret = 0;

	ret = genl_register_family(&cnss_genl_family);
	if (ret != 0)
		cnss_pr_err("genl_register_family fail: %d\n", ret);

	return ret;
}

void cnss_genl_exit(void)
{
	genl_unregister_family(&cnss_genl_family);
}
