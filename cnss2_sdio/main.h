/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _CNSS_MAIN_H
#define _CNSS_MAIN_H
#include <linux/etherdevice.h>
#ifdef CONFIG_CNSS_OUT_OF_TREE
#include "cnss2.h"
#else
#include <net/cnss2.h>
#endif
#include "cnss_prealloc.h"
#include "wlan_firmware_service_v01.h"
#include <linux/platform_device.h>
#include <linux/rpmsg.h>

#define CNSS_FS_NAME			"cnss_sdio"
#define CNSS_FS_NAME_SIZE		15
#define CNSS_DEVICE_NAME_SIZE		16
#define CNSS_SMC_EVENT_RETRY_COUNT	2
#define ASSERT_FILE_MAX_LEN 24

#define CNSS_EVENT_SYNC   BIT(0)
#define CNSS_EVENT_UNINTERRUPTIBLE BIT(1)
#define CNSS_EVENT_UNKILLABLE BIT(2)
#define CNSS_EVENT_SYNC_UNINTERRUPTIBLE (CNSS_EVENT_SYNC | \
				CNSS_EVENT_UNINTERRUPTIBLE)
#define CNSS_EVENT_SYNC_UNKILLABLE (CNSS_EVENT_SYNC | CNSS_EVENT_UNKILLABLE)

#define TSF_IRQ_TS		"tsf_irq_ts"
#define TSF_IRQ_TS_OP_VALID	BIT(0)
#define TSF_IRQ_TS_OP_OVERFLOW	BIT(8)
#define TSF_IRQ_TS_OP_OFFSET	0x0
#define TSF_IRQ_TS_LO_OFFSET	0x4
#define TSF_IRQ_TS_HI_OFFSET	0x8
#define TSF_IRQ_TS_HI_MASK	0xFFFFFF

extern struct completion hang_event_complete;

enum cnss_dt_type {
	CNSS_DTT_LEGACY = 0,
	CNSS_DTT_CONVERGED = 1,
	CNSS_DTT_MULTIEXCHG = 2
};

enum cnss_ssr_state {
	SSR_STATE_IDLE = 0,
	SSR_DUMP_IN_PROGRESS,
	SSR_DUMP_COMPLETE,
	SSR_STATE_FAILED
};

enum cnss_dev_bus_type {
	CNSS_BUS_NONE = -1,
	CNSS_BUS_PCI,
	CNSS_BUS_SDIO,
	CNSS_BUS_MAX
};

#if IS_ENABLED(CONFIG_MSM_SUBSYSTEM_RESTART)
struct cnss_subsys_info {
	struct subsys_device *subsys_device;
	struct subsys_desc subsys_desc;
	void *subsys_handle;
};
#endif

enum cnss_driver_event_type {
	CNSS_DRIVER_EVENT_REGISTER_DRIVER,
	CNSS_DRIVER_EVENT_UNREGISTER_DRIVER,
	CNSS_DRIVER_EVENT_DEVICE_UP,
	CNSS_DRIVER_EVENT_IDLE_RESTART,
	CNSS_DRIVER_EVENT_IDLE_SHUTDOWN,
	CNSS_DRIVER_EVENT_REINIT,
	CNSS_DRIVER_EVENT_SHUTDOWN,
	CNSS_DRIVER_EVENT_RECOVERY,
	CNSS_DRIVER_EVENT_FW_DOWN,
	CNSS_DRIVER_EVENT_MAX
};

enum cnss_driver_state {
	CNSS_DRIVER_LOADING,
	CNSS_DRIVER_UNLOADING,
	CNSS_DRIVER_PROBING,
	CNSS_DRIVER_PROBED,
	CNSS_DEV_REMOVED,
	CNSS_DRIVER_IDLE_RESTART,
	CNSS_DRIVER_IDLE_SHUTDOWN,
	CNSS_DRIVER_SHUTDOWN,
	CNSS_DRIVER_REINIT,
	CNSS_IN_REBOOT,
	CNSS_DRIVER_RECOVERING,
	CNSS_DEVICE_RESET,
	CNSS_SSR_DUMP_IN_PROGRESS,
	CNSS_POWER_OFF,
	CNSS_DRIVER_FW_SSR_IN_PROGRESS,
	CNSS_DRIVER_SMC_SSR_IN_PROGRESS,
};

struct cnss_recovery_data {
	enum cnss_recovery_reason reason;
};

/**
 * enum cnss_time_sync_period_vote - to get per vote time sync period
 * @TIME_SYNC_VOTE_WLAN: WLAN Driver vote
 * @TIME_SYNC_VOTE_CNSS: sys config vote
 * @TIME_SYNC_VOTE_MAX
 */
enum cnss_time_sync_period_vote {
	TIME_SYNC_VOTE_WLAN,
	TIME_SYNC_VOTE_CNSS,
	TIME_SYNC_VOTE_MAX,
};

struct cnss_control_params {
	unsigned int time_sync_period;
	unsigned int time_sync_period_vote[TIME_SYNC_VOTE_MAX];
};

enum meson_dump_bins {
	MESON_CMEM_BIN = 0,
	MESON_CORE_BIN,
	MESON_DRAM_BIN,
	MESON_IRAM_BIN,
	MESON_MWSS_BIN,
	MAX_MESON_BINS
};

struct meson_dump_data {
	u8 *meson_dump_bin;
	size_t meson_dump_size;
};

struct cnss_thermal_cdev {
	struct list_head tcdev_list;
	int tcdev_id;
	unsigned long curr_thermal_state;
	unsigned long max_thermal_state;
	struct device_node *dev_node;
	struct thermal_cooling_device *tcdev;
};

/* Forward declaration for WLAN GPIO data */
struct cnss_wlan_gpio_data;

/**
 * enum wlan_gpio_cmd_type - Command types sent from HOST to ADSP
 */
enum wlan_gpio_cmd_type {
	WLAN_GPIO_CMD_REGISTER = 0x01,
	WLAN_GPIO_CMD_DEREGISTER = 0x02,
};

/**
 * enum wlan_gpio_rsp_type - Response types sent from ADSP to HOST
 */
enum wlan_gpio_rsp_type {
	WLAN_GPIO_RSP_REGISTER = 0x01,
	WLAN_GPIO_RSP_DEREGISTER = 0x02,
};

/**
 * enum wlan_gpio_status - Status codes for WLAN GPIO operations
 */
enum wlan_gpio_status {
	WLAN_GPIO_STATUS_SUCCESS = 0x00,
	WLAN_GPIO_STATUS_INVALID_PARAMS = 0x01,
	WLAN_GPIO_STATUS_ALREADY_REGISTERED = 0x02,
	WLAN_GPIO_STATUS_NOT_REGISTERED = 0x03,
};

/**
 * enum cnss_wlan_gpio_state - WLAN GPIO channel state
 */
enum cnss_wlan_gpio_state {
	CNSS_WLAN_GPIO_STATE_DOWN = 0,
	CNSS_WLAN_GPIO_STATE_UP = 1,
};

/**
 * struct wlan_gpio_register_cmd - GPIO registration command
 * @cmd_type: WLAN_GPIO_CMD_REGISTER
 *
 * All configuration (direction, initial_value, flags) is handled by ADSP.
 */
struct wlan_gpio_register_cmd {
	u8 cmd_type;
} __packed;

/**
 * struct wlan_gpio_deregister_cmd - GPIO deregistration command
 * @cmd_type: WLAN_GPIO_CMD_DEREGISTER
 */
struct wlan_gpio_deregister_cmd {
	u8 cmd_type;
} __packed;

/**
 * struct wlan_gpio_response - Response/acknowledgment message
 * @rsp_type: Response type from ADSP
 * @status: Status code (success/error)
 */
struct wlan_gpio_response {
	u8 rsp_type;
	u8 status;
} __packed;

/**
 * struct cnss_wlan_gpio_data - WLAN GPIO channel management data
 * @rpdev: RPMsg device pointer (protected by rpdev_sem)
 * @rpdev_sem: Read-write semaphore for channel synchronization
 * @channel_up: Boolean indicating channel state (protected by rpdev_sem)
 * @rx_lock: Spinlock for RX list protection
 * @rx_list: List of received messages
 * @rx_work: Work structure for RX processing
 * @rx_wq: Workqueue for RX processing
 * @gpio_registered: Flag indicating if GPIO is registered
 * @reg_lock: Mutex protecting registration state
 * @rpmsg_match: RPMsg device ID table
 * @channel_name: RPMsg channel name
 * @gpio_reg_complete: completion structure pointer
 * @gpio_dereg_complete: completion structure pointer
 */
struct cnss_wlan_gpio_data {
	struct rpmsg_device *rpdev;
	struct rw_semaphore rpdev_sem;
	bool channel_up;
	spinlock_t rx_lock;
	struct list_head rx_list;
	struct work_struct rx_work;
	struct workqueue_struct *rx_wq;
	bool gpio_registered;
	struct mutex reg_lock;
	struct rpmsg_device_id rpmsg_match[2];
	char channel_name[RPMSG_NAME_SIZE];
	struct completion gpio_reg_complete;
	struct completion gpio_dereg_complete;
};

/**
 * struct cnss_wlan_gpio_buf - RX buffer structure
 * @node: List node
 * @len: Buffer length
 * @buf: Buffer data (flexible array)
 */
struct cnss_wlan_gpio_buf {
	struct list_head node;
	size_t len;
	u8 buf[];
};

typedef uint8_t bus_hsize;
typedef uint8_t bus_hport;

typedef struct {
	uint32_t ra;
	uint32_t sp;
	uint32_t gp;
	uint32_t tp;
	uint32_t t0;
	uint32_t t1;
	uint32_t t2;
	uint32_t s0;
	uint32_t s1;
	uint32_t a0;
	uint32_t a1;
	uint32_t a2;
	uint32_t a3;
	uint32_t a4;
	uint32_t a5;
	uint32_t a6;
	uint32_t a7;
	uint32_t s2;
	uint32_t s3;
	uint32_t s4;
	uint32_t s5;
	uint32_t s6;
	uint32_t s7;
	uint32_t s8;
	uint32_t s9;
	uint32_t s10;
	uint32_t s11;
	uint32_t t3;
	uint32_t t4;
	uint32_t t5;
	uint32_t t6;
} genr;

typedef struct {
	uint32_t hw_version;
	uint32_t inst_id;
	uint32_t num_slaves;
	uint32_t timer_loadval;
	uint32_t timer_mode;
	uint32_t intr_status;
	uint32_t intr_clear;
	uint32_t intr_enable;
	uint32_t synd_valid;
	uint32_t synd_clear;
	uint32_t synd_id;
	uint32_t synd_addr0;
	uint32_t synd_addr1;
	uint32_t synd_hready;
} timeoutr;

typedef struct {
	uint32_t mepc;
	uint32_t mcause;
	uint32_t mtval;
	uint32_t mdcause;
} mthr;

typedef struct {
	uint32_t mstatus;
	uint32_t mtvec;
	uint32_t mtvt;
} mtsr;

typedef struct {
	genr gen;
	mthr mth;
	mtsr mts;
} archr;

typedef struct {
	uint32_t code;
	char file[ASSERT_FILE_MAX_LEN];  /* filename (basename), stored as string directly */
	uint32_t line;
} asstinf;

typedef struct {
	uint32_t int_handler_time;   /* err interrupt handler called time */
	uint32_t excep_handler_time; /* exception handler called time */
	uint32_t nmi_handler_time;   /* nmi handler called time */
	uint32_t saved_times;        /* dump info saved times, to debug multi-save case */
} err_timeinf;

/* Master ID of the transaction that caused bus hang. This Slave logs only CPU Master access */
typedef enum {
	BUS_HMID_SNOC = 9,
	BUS_HMID_DXE0 = 10,
	BUS_HMID_DXE1 = 11,
	BUS_HMID_RRI  = 12,
	BUS_HMID_MCSS = 13,
	BUS_HMID_UART = 14,
	BUS_HMID_SDIO = 15,
	BUS_HMID_MWSS = 1,
} bus_hmid;

/* Htrans of the beat of the Master transaction that caused bus hang. */
typedef enum {
	BUS_HTRANS_IDLE    = 0,
	BUS_HTRANS_NON_SEQ = 1,
	BUS_HTRANS_SEQ     = 2,
	BUS_HTRANS_BUSY    = 3,
} bus_htrans;

/* Hburst of the beat of the Master transaction that caused bus hang. */
typedef enum {
	BUS_BURST_SINGLE = 0,
	BUS_BURST_INCR   = 1,
	BUS_BURST_WRAP4  = 2,
	BUS_BURST_INCR4  = 3,
	BUS_BURST_WRAP8  = 4,
	BUS_BURST_INCR8  = 5,
	BUS_BURST_WRAP16 = 6,
	BUS_BURST_INCR16 = 7,
} bus_hburst;

/* Hwrite of the Master transaction that caused bus hang. */
typedef enum {
	BUS_HWRITE_READ  = 0,
	BUS_HWRITE_WRITE = 1,
} bus_hwrite;

typedef union {
	uint32_t reg;
	struct {
		bus_hport hw_protection_type : 4;   // [3:0]   HPROT
		bus_hsize hw_transfer_size   : 3;   // [6:4]   HSIZE
		bus_hwrite hw_write          : 1;   // [7]     HWRITE
		bus_hburst hw_burst_type     : 3;   // [10:8]  HBURST
		uint32_t reserved_1          : 1;   // [11]    Reserved
		bus_htrans hw_transfer_type  : 2;   // [13:12] HTRANS
		uint32_t reserved_2          : 2;   // [15:14] Reserved
		bus_hmid hw_master_id        : 8;   // [23:16] HMID
		uint32_t reserved_3          : 8;   // [31:24] Reserved
	} field;
} bus_synd_id;

typedef enum {
	BUS_MASTER_BIT_SNOC = 0,
	BUS_MASTER_BIT_DXE0 = 1,
	BUS_MASTER_BIT_DXE1 = 2,
	BUS_MASTER_BIT_RRI  = 3,
	BUS_MASTER_BIT_UART = 4,
	BUS_MASTER_BIT_SDIO = 5,
	BUS_MASTER_BIT_MWSS = 6,
	BUS_MASTER_BIT_MAX  = 7,
	BUS_MASTER_BIT_MCSS = 8,    /* Doesn't have this bit actually, just for a symbol */
} bus_master_bit;

typedef enum {
	ERR_NONE_S = 0,
	ERR_EXCEPT = 1,		/* Precise exception */
	ERR_ASSERT = 2,     /* Software assert */
	ERR_NMI = 3,        /* None-precise exception, local NMI */
	ERR_NMI_EXT = 4,    /* None-precise exception, external NMI */
	ERR_B_TRAN_E = 5,   /* None-precise exception, cpu bus write/read err */
	ERR_B_TIMEOUT = 6,  /* None-precise exception, system bus timeout */
	ERR_M_CORRUPT = 7,	/* None-precise exception, memory corrupt */
	ERR_UNRESOLVE,
} err_type;

typedef struct {
	uint8_t intr_status;
	bus_master_bit master_bit;
	bus_synd_id synd_id;
	uint32_t synd_addr;
	uint32_t synd_slave_ready_bit;
} err_bus_timeout_info;

typedef struct {
	err_type type;
	union {
		asstinf assert;       /* valid when type == ERR_ASSERT    */
		err_bus_timeout_info bus_timeout; /* valid when type == ERR_B_TIMEOUT */
	};
	archr regs;
} coredump;

struct cnss_plat_data {
	struct platform_device *plat_dev;
	void *bus_priv;
	enum cnss_dev_bus_type bus_type;
#if IS_ENABLED(CONFIG_MSM_SUBSYSTEM_RESTART)
	struct cnss_subsys_info subsys_info;
#endif
	struct notifier_block smc_nb;
	struct smc_notif_info *smc_notif;
	unsigned long device_id;
	unsigned long driver_state;
	struct list_head event_list;
	struct list_head cnss_tcdev_list;
	struct mutex tcdev_lock; /* mutex for cooling devices list access */
	spinlock_t event_lock; /* spinlock for driver work event handling */
	struct work_struct event_work;
	struct workqueue_struct *event_wq;
	struct dentry *root_dentry;
	atomic_t pm_count;
	atomic_t power_up_retry_cnt;
	atomic_t power_down_retry_cnt;
	struct cnss_wlan_driver *driver_ops;
	u32 dt_type;
	enum cnss_ssr_state ssr_state;
	bool smc_crash_state;
	struct device_node *dev_node;
	char device_name[CNSS_DEVICE_NAME_SIZE];
	u32 plat_idx;
	struct wlchip_serial_id_v01 serial_id;
	struct completion power_up_complete;
	struct completion power_down_complete;
	struct completion fw_assert_complete;
	struct notifier_block smc_tsf_nb;
	struct smc_notif_info *smc_tsf_notif;
	struct notifier_block reboot_nb;
	struct completion time_sync_notif;
	struct cnss_control_params ctrl_params;
	struct mutex ts_lock; /* mutex to update time sync period */
	u8 recovery_enabled;
	struct meson_dump_data dump_data[MAX_MESON_BINS];
	void __iomem *ts_reg_addr;
	/* WLAN GPIO channel data */
	struct cnss_wlan_gpio_data *wlan_gpio_data;
};

int cnss2_sdio_schedule_recovery(void);
struct cnss_plat_data *cnss_get_plat_priv(struct platform_device *plat_dev);
bool cnss_is_dual_wlan_enabled(void);
int cnss_driver_event_post(struct cnss_plat_data *plat_priv,
			   enum cnss_driver_event_type type,
			   u32 flags, void *data);

#endif /* _CNSS_MAIN_H */
