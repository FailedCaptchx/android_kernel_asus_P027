# ---------------------------------------------------
# Compile Options
# ---------------------------------------------------
WLAN_CHIP_LIST:=-UMT6620 -UMT6628 -UMT5931 -UMT6630
ccflags-y += $(WLAN_CHIP_LIST)

WLAN_CHIP_ID=MT6630

ccflags-y += -DCFG_SUPPORT_DEBUG_FS=0
ccflags-y += -DWLAN_INCLUDE_PROC
ccflags-y += -DCFG_SUPPORT_AGPS_ASSIST=1
ccflags-y += -DCFG_SUPPORT_TSF_USING_BOOTTIME=1

ccflags-y:=$(filter-out -U$(WLAN_CHIP_ID),$(ccflags-y))
ccflags-y += -DLINUX -D$(WLAN_CHIP_ID)

ifeq ($(CONFIG_MTK_WIFI_MCC_SUPPORT), y)
    ccflags-y += -DCFG_SUPPORT_CHNL_CONFLICT_REVISE=0
else
    ccflags-y += -DCFG_SUPPORT_CHNL_CONFLICT_REVISE=1
endif

ifeq ($(CONFIG_MTK_AEE_FEATURE), y)
    ccflags-y += -DCFG_SUPPORT_AEE=1
else
    ccflags-y += -DCFG_SUPPORT_AEE=0
endif

# Disable ASSERT() for user load, enable for others
ifneq ($(TARGET_BUILD_VARIANT),user)
    ccflags-y += -DBUILD_QA_DBG=1
else
    ccflags-y += -DBUILD_QA_DBG=0
endif

#ifeq ($(CONFIG_MTK_COMBO_WIFI_HIF_SDIO1), y)
#    ccflags-y += -D_HIF_SDIO=1
#endif

ifeq ($(CONFIG_MTK_PASSPOINT_R1_SUPPORT), y)
    ccflags-y += -DCFG_SUPPORT_PASSPOINT=1
    ccflags-y += -DCFG_HS20_DEBUG=1
    ccflags-y += -DCFG_ENABLE_GTK_FRAME_FILTER=1
else ifeq ($(CONFIG_MTK_PASSPOINT_R2_SUPPORT), y)
    ccflags-y += -DCFG_SUPPORT_PASSPOINT=1
    ccflags-y += -DCFG_HS20_DEBUG=1
    ccflags-y += -DCFG_ENABLE_GTK_FRAME_FILTER=1
else
    ccflags-y += -DCFG_SUPPORT_PASSPOINT=0
    ccflags-y += -DCFG_HS20_DEBUG=0
    ccflags-y += -DCFG_ENABLE_GTK_FRAME_FILTER=0
endif

MTK_MET_PROFILING_SUPPORT = yes
ifeq ($(MTK_MET_PROFILING_SUPPORT), yes)
    ccflags-y += -DCFG_MET_PACKET_TRACE_SUPPORT=1
else
    ccflags-y += -DCFG_MET_PACKET_TRACE_SUPPORT=0
endif

ccflags-y += -DCFG_FACTORY_MAC=1

ccflags-y += -D_HIF_SDIO=1

ccflags-y += -DDBG=0
ccflags-y += -I$(src)/os -I$(src)/os/linux/include -I$(src)/os/linux/hif/sdio/include
ccflags-y += -I$(src)/include -I$(src)/include/nic -I$(src)/include/mgmt

MODULE_NAME := wlan_gen3
obj-$(CONFIG_MTK_COMBO_WIFI) += $(MODULE_NAME).o
#obj-y += $(MODULE_NAME).o

# ---------------------------------------------------
# Directory List
# ---------------------------------------------------
COMMON_DIR  := common/
OS_DIR      := os/linux/
HIF_DIR	    := os/linux/hif/sdio/
NIC_DIR     := nic/
MGMT_DIR    := mgmt/


# ---------------------------------------------------
# Objects List
# ---------------------------------------------------

COMMON_OBJS :=	$(COMMON_DIR)dump.o \
	       		$(COMMON_DIR)wlan_lib.o \
	       		$(COMMON_DIR)wlan_oid.o \
	       		$(COMMON_DIR)wlan_bow.o

NIC_OBJS := $(NIC_DIR)nic.o \
			$(NIC_DIR)nic_tx.o \
			$(NIC_DIR)nic_rx.o \
			$(NIC_DIR)nic_pwr_mgt.o \
            $(NIC_DIR)nic_rate.o \
			$(NIC_DIR)cmd_buf.o \
			$(NIC_DIR)que_mgt.o \
			$(NIC_DIR)nic_cmd_event.o

OS_OBJS :=	$(OS_DIR)gl_init.o \
			$(OS_DIR)gl_kal.o  \
			$(OS_DIR)gl_bow.o \
			$(OS_DIR)gl_wext.o \
			$(OS_DIR)gl_wext_priv.o \
			$(OS_DIR)gl_rst.o \
			$(OS_DIR)gl_cfg80211.o \
			$(OS_DIR)gl_vendor.o \
			$(OS_DIR)platform.o \
			$(OS_DIR)platform_wifi.o

MGMT_OBJS := $(MGMT_DIR)ais_fsm.o \
			 $(MGMT_DIR)aaa_fsm.o \
			 $(MGMT_DIR)assoc.o \
			 $(MGMT_DIR)auth.o \
			 $(MGMT_DIR)bss.o \
			 $(MGMT_DIR)cnm.o \
			 $(MGMT_DIR)cnm_timer.o \
			 $(MGMT_DIR)cnm_mem.o \
			 $(MGMT_DIR)hem_mbox.o \
			 $(MGMT_DIR)mib.o \
			 $(MGMT_DIR)privacy.o  \
			 $(MGMT_DIR)rate.o \
			 $(MGMT_DIR)rlm.o \
			 $(MGMT_DIR)rlm_domain.o \
			 $(MGMT_DIR)rlm_obss.o \
			 $(MGMT_DIR)rlm_protection.o \
			 $(MGMT_DIR)rsn.o \
			 $(MGMT_DIR)saa_fsm.o \
			 $(MGMT_DIR)scan.o \
			 $(MGMT_DIR)scan_fsm.o \
             $(MGMT_DIR)swcr.o \
             $(MGMT_DIR)roaming_fsm.o \
             $(MGMT_DIR)tkip_mic.o \
             $(MGMT_DIR)hs20.o \
             $(MGMT_DIR)tdls.o
# ---------------------------------------------------
# STATS Objects List
# ---------------------------------------------------
MGMT_OBJS += $(MGMT_DIR)stats.o

# ---------------------------------------------------
# P2P Objects List
# ---------------------------------------------------

COMMON_OBJS += $(COMMON_DIR)wlan_p2p.o

NIC_OBJS += $(NIC_DIR)p2p_nic.o

OS_OBJS += $(OS_DIR)gl_p2p.o \
           $(OS_DIR)gl_p2p_cfg80211.o \
           $(OS_DIR)gl_p2p_init.o \
           $(OS_DIR)gl_p2p_kal.o

MGMT_OBJS += $(MGMT_DIR)p2p_dev_fsm.o\
            $(MGMT_DIR)p2p_dev_state.o\
            $(MGMT_DIR)p2p_role_fsm.o\
            $(MGMT_DIR)p2p_role_state.o\
            $(MGMT_DIR)p2p_func.o\
            $(MGMT_DIR)p2p_scan.o\
            $(MGMT_DIR)p2p_ie.o\
            $(MGMT_DIR)p2p_rlm.o\
            $(MGMT_DIR)p2p_assoc.o\
            $(MGMT_DIR)p2p_bss.o\
            $(MGMT_DIR)p2p_rlm_obss.o

MGMT_OBJS += $(MGMT_DIR)wapi.o

OS_OBJS += $(OS_DIR)gl_proc.o

HIF_OBJS :=  $(HIF_DIR)arm.o \
             $(HIF_DIR)sdio.o

$(MODULE_NAME)-objs  += $(COMMON_OBJS)
$(MODULE_NAME)-objs  += $(NIC_OBJS)
$(MODULE_NAME)-objs  += $(OS_OBJS)
$(MODULE_NAME)-objs  += $(HIF_OBJS)
$(MODULE_NAME)-objs  += $(MGMT_OBJS)

