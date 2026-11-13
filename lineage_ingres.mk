#
# Copyright (C) 2025 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from ingres device
$(call inherit-product, device/xiaomi/ingres/device.mk)

# Inherit from common lineage configuration
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

# Built-in Google apps (MindTheGapps), when the local vendor/gapps tree is present. Built in,
# the default runtime grants for GMS and the sync adapters are applied on first boot, which a
# GApps zip flashed after the first boot never gets (contacts/calendar sync then stays off).
$(call inherit-product-if-exists, vendor/gapps/gapps.mk)

PRODUCT_NAME := lineage_ingres
PRODUCT_DEVICE := ingres
PRODUCT_MANUFACTURER := Xiaomi
PRODUCT_BRAND := Redmi
PRODUCT_MODEL := 21121210G

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="ingres-user 14 UKQ1.240624.001 OS2.0.5.0.ULJCNXM release-keys" \
    BuildFingerprint=Redmi/ingres/ingres:14/UKQ1.240624.001/OS2.0.5.0.ULJCNXM:user/release-keys \
    DeviceProduct=ingres \
    SystemName=ingres

PRODUCT_GMS_CLIENTID_BASE := android-xiaomi

# Neo
TARGET_UNOFFICIAL_BUILD_ID := Neo
# Neo OS version: properties/system.prop (ro.build.display.id, ro.neo.os.*)
NEO_GITHUB_ORG ?= Ingres-lab
PRODUCT_PRODUCT_PROPERTIES += \
    lineage.updater.uri=https://raw.githubusercontent.com/$(NEO_GITHUB_ORG)/releases/main/{device}.json
