LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_CFLAGS := \
        -fPIC -D_POSIX_SOURCE

LOCAL_C_INCLUDES:= \
    $(LOCAL_PATH)/include \
	$(LOCAL_PATH)/../amffmpeg

LOCAL_SRC_FILES := \
        adecproc.c adec.c log.c feeder.c audiodsp_ctl.c audio_out/android_out.cpp \
		codec_mgt.c wmapro/wmaprodec.c aac_main/aacdec.c

LOCAL_MODULE := libamadec

LOCAL_ARM_MODE := arm

$(shell cd $(LOCAL_PATH)/firmware && { \
for f in *.bin; do \
  md5sum "$$f" > "$$f".checksum; \
done;})

copy_from := $(wildcard $(LOCAL_PATH)/firmware/*.bin)

copy_from += $(wildcard $(LOCAL_PATH)/firmware/*.checksum)

install_pairs := $(foreach f,$(copy_from),$(f):system/etc/firmware/$(notdir $(f)))

PRODUCT_COPY_FILES += $(install_pairs)

include $(BUILD_STATIC_LIBRARY)

