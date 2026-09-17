/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2019-2020, The Linux Foundation. All rights reserved. */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#ifndef __CNSS_GENL_H__
#define __CNSS_GENL_H__
#include <net/genetlink.h>
#include <net/netlink.h>

// This enum should be in sync with enum defined in cnss_genl_msg.h
enum cnss_genl_msg_type {
	CNSS_GENL_MSG_TYPE_HANG_EVENT = 9,
};


int cnss_genl_init(void);
void cnss_genl_exit(void);
int cnss_genl_send_msg(void *buff, u8 type,
		       char *file_name, u32 total_size);
int cnss_genl_process_msg(struct sk_buff *skb, struct genl_info *info);


#endif
