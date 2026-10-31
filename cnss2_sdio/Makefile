ifeq ($(CONFIG_CNSS_OUT_OF_TREE),y)
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/cnss_utils
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/inc
else
ccflags-y += -I$(srctree)/drivers/net/wireless/cnss_utils
endif

ifeq ($(CONFIG_CNSS_HW_SECURE_DISABLE), y)
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/../../securemsm-kernel/smcinvoke/
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/../../securemsm-kernel/linux/
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/../../securemsm-kernel/include/linux/
ccflags-y += -I$(WLAN_PLATFORM_ROOT)/../../securemsm-kernel/include/uapi/linux/
endif

obj-$(CONFIG_CNSS2_SDIO) += cnss2_sdio.o

cnss2_sdio-y := main.o
cnss2_sdio-y += bus.o
cnss2_sdio-y += debug.o
cnss2_sdio-y += sdio.o
cnss2_sdio-y += genl.o
