FUSION_INC_DIR := drivers/message/fusion
EXTRA_CFLAGS += -I$(FUSION_INC_DIR) -Iinclude/scsi

EXTRA_CFLAGS += -DCONFIG_SCST_EXTRACHECKS 
#EXTRA_CFLAGS += -DCONFIG_SCST_TRACING
EXTRA_CFLAGS += -DCONFIG_SCST_DEBUG
#EXTRA_CFLAGS += -DDEBUG_WORK_IN_THREAD

obj-$(CONFIG_FUSION_SCST) += mpt_scst.o

