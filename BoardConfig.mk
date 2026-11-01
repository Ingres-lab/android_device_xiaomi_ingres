#
# Copyright (C) 2025 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Security patch level
VENDOR_SECURITY_PATCH := 2025-04-01

# Inherit from xiaomi sm8450-common
include device/xiaomi/sm8450-common/BoardConfigCommon.mk

# Inherit from the proprietary version
include vendor/xiaomi/ingres/BoardConfigVendor.mk

DEVICE_PATH := device/xiaomi/ingres

# Audio
AUDIO_FEATURE_ENABLED_CIRRUS_CALIBRATION_RESISTANCE := true
AUDIO_FEATURE_ENABLED_ULTRASOUND_PROXIMITY := true

# Properties
TARGET_SYSTEM_PROP += $(DEVICE_PATH)/properties/system.prop
TARGET_VENDOR_PROP += $(DEVICE_PATH)/properties/vendor.prop

# Screen density
TARGET_SCREEN_DENSITY := 395

# --- 6.6 GKI kernel ---
#
# 6.6 GKI kernel override for the LineageOS 24.0 ingres tree.
#
# The 5.10 device/common trees provide the HALs and product packages; only the
# kernel source, config and devicetree change.
#

TARGET_KERNEL_CONFIG := \
    gki_defconfig \
    vendor/waipio_perf.config \
    vendor/ingres_GKI.config

TARGET_KERNEL_SOURCE := kernel/xiaomi/sm8450
TARGET_KERNEL_EXT_MODULE_ROOT := kernel/xiaomi/sm8450-modules
TARGET_KERNEL_NO_GCC := true

# 6.6 module set (differs from the 5.10 common list):
# - cvp-kernel is not present in the Sony 6.6 modules tree
# - kgsl, touch, mm/sync/synx/securemsm are external modules in 6.6
TARGET_KERNEL_EXT_MODULES := \
    qcom/opensource/mmrm-driver \
    qcom/opensource/mm-drivers/sync_fence \
    qcom/opensource/mm-drivers/msm_ext_display \
    qcom/opensource/synx-kernel \
    qcom/opensource/securemsm-kernel \
    qcom/opensource/datarmnet-ext/mem \
    qcom/opensource/dataipa/drivers/platform/msm \
    qcom/opensource/datarmnet/core \
    qcom/opensource/datarmnet-ext/aps \
    qcom/opensource/datarmnet-ext/offload \
    qcom/opensource/datarmnet-ext/shs \
    qcom/opensource/datarmnet-ext/perf \
    qcom/opensource/datarmnet-ext/perf_tether \
    qcom/opensource/datarmnet-ext/sch \
    qcom/opensource/datarmnet-ext/wlan \
    qcom/opensource/camera-kernel \
    qcom/opensource/dsp-kernel \
    qcom/opensource/eva-kernel \
    qcom/opensource/video-driver \
    qcom/opensource/display-drivers/msm \
    qcom/opensource/graphics-kernel \
    qcom/opensource/touch-drivers \
    qcom/opensource/wlan/platform \
    qcom/opensource/wlan/qcacld-3.0/.qca6490 \
    qcom/opensource/wlan/qcacld-3.0/.qca6750

BOARD_BOOT_HEADER_VERSION := 4
TARGET_NEEDS_DTBOIMAGE := true
BOARD_USES_QCOM_MERGE_DTBS_SCRIPT := true

# The release userspace blobs (adpl) link the legacy rmnet control API. With
# TARGET_KERNEL_VERSION=6.6, qcom_defs.mk moves taro/sm8450 into UM_6_6_FAMILY,
# which removes the opt-in that enabled USE_OLD_RMNET_DATA on 5.10. Select the
# legacy API explicitly so librmnetctl exports the symbols adpl needs.
$(call soong_config_set,rmnetctl,old_rmnet_data,true)
