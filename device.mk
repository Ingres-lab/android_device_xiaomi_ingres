#
# Copyright (C) 2025 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from xiaomi sm8450-common
$(call inherit-product, device/xiaomi/sm8450-common/common.mk)

# Inherit from the proprietary version
$(call inherit-product, vendor/xiaomi/ingres/ingres-vendor.mk)

# Audio configs
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/mixer_paths_waipio_mtp.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_taro/mixer_paths_waipio_mtp.xml \
    $(LOCAL_PATH)/audio/resourcemanager_waipio_mtp.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_taro/resourcemanager_waipio_mtp.xml \
    $(LOCAL_PATH)/audio/usecaseKvManager.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usecaseKvManager.xml

# USB Wi-Fi dongle firmware (linux-firmware, redistributable)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/mt7601u.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/mt7601u.bin

# NeoParts
PRODUCT_PACKAGES += \
    NeoParts

# Overlay
PRODUCT_PACKAGES += \
    SettingsProviderResIngres \
    WifiResIngres \
    FrameworksResIngres \
    SystemUIResIngres \
    ApertureResIngres \
    NfcResIngres

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    hardware/qcom-caf/sm8450/audio \
    hardware/qcom-caf/sm8450/display \
    $(LOCAL_PATH)

# Lineage Health: bypass charging. night_charging makes the firmware stop
# charging and run the phone from the adapter once the battery is at or
# above 80%; input_suspend (the default) cuts the input instead, so the
# battery drained while plugged in at the limit.
$(call soong_config_set,lineage_health,charging_control_charging_path,/sys/class/qcom-battery/night_charging)
$(call soong_config_set,lineage_health,charging_control_charging_enabled,0)
$(call soong_config_set,lineage_health,charging_control_charging_disabled,1)
$(call soong_config_set_bool,lineage_health,charging_control_supports_bypass,true)
