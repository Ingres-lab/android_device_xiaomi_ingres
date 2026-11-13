#
# Copyright (C) 2025 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from xiaomi sm8450-common
$(call inherit-product, device/xiaomi/sm8450-common/common.mk)

# Inherit from the proprietary version
$(call inherit-product, vendor/xiaomi/ingres/ingres-vendor.mk)

# Xiaomi Camera (MiuiCamera, replaces Aperture)
$(call inherit-product-if-exists, device/xiaomi/miuicamera-ingres/device.mk)

# Audio configs
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/mixer_paths_waipio_mtp.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_taro/mixer_paths_waipio_mtp.xml \
    $(LOCAL_PATH)/audio/resourcemanager_waipio_mtp.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_taro/resourcemanager_waipio_mtp.xml \
    $(LOCAL_PATH)/audio/usecaseKvManager.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usecaseKvManager.xml

# USB Wi-Fi dongle firmware (linux-firmware, redistributable). rtw8822c(+wow) comes from AOSP's
# external/linux-firmware module, so it is not copied here.
PRODUCT_PACKAGES += \
    linux_firmware_rtw88-rtw8822c

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/firmware/mt7601u.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/mt7601u.bin \
    $(LOCAL_PATH)/firmware/mt7662.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/mt7662.bin \
    $(LOCAL_PATH)/firmware/mt7662_rom_patch.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/mt7662_rom_patch.bin \
    $(LOCAL_PATH)/firmware/rtw88/rtw8822b_fw.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/rtw88/rtw8822b_fw.bin \
    $(LOCAL_PATH)/firmware/rtw88/rtw8821c_fw.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/rtw88/rtw8821c_fw.bin \
    $(LOCAL_PATH)/firmware/rtw88/rtw8723d_fw.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/rtw88/rtw8723d_fw.bin \
    $(LOCAL_PATH)/firmware/LICENCE.linux-firmware.txt:$(TARGET_COPY_OUT_VENDOR)/firmware/LICENCE.linux-firmware.txt

# NeoParts
PRODUCT_PACKAGES += \
    NeoParts

# Qualcomm NNAPI HAL (HTP v69 NPU via the compute DSP); blobs live in vendor/xiaomi/ingres/nnhal
PRODUCT_PACKAGES += \
    android.hardware.neuralnetworks-shim-service-qti \
    qconfigpresets.json

# Neo: shoulder trigger mapping, rear lights
PRODUCT_PACKAGES += \
    android.hardware.lights-service.neo \
    neo_triggerd \
    neo_lights.rc

# Mesa Turnip Vulkan driver, selectable in NeoParts (applied on reboot)
PRODUCT_PACKAGES += \
    vulkan.turnip

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

