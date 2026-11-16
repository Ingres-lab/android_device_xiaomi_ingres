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

# Neo OS boot animation (neo/bootanimation/gen_bootanimation.py). Lineage's
# genrule copies it with a path relative to the source root; if out/ is a
# symlink outside the tree, the out dir's parent needs a device/ link to it.
TARGET_SCREEN_HEIGHT := 2400
TARGET_SCREEN_WIDTH := 1080
TARGET_BOOTANIMATION := device/xiaomi/ingres/neo/bootanimation/bootanimation.zip

# Neo (triggers, rear lights)
BOARD_VENDOR_SEPOLICY_DIRS += device/xiaomi/ingres/sepolicy/vendor

# Xiaomi Camera (MiuiCamera)
-include device/xiaomi/miuicamera-ingres/BoardConfig.mk

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

# Kernel build profile: perf (default, ThinLTO, minimal debug/logging) or
# debug (tracers, lockdep, KFENCE, verbose logs). Select with
# NEO_KERNEL_PROFILE=debug in the environment.
NEO_KERNEL_PROFILE ?= perf
ifeq ($(filter perf debug,$(NEO_KERNEL_PROFILE)),)
$(error NEO_KERNEL_PROFILE must be perf or debug, got '$(NEO_KERNEL_PROFILE)')
endif

TARGET_KERNEL_CONFIG := \
    gki_defconfig \
    vendor/waipio_perf.config \
    vendor/ingres_GKI.config \
    vendor/neo_$(NEO_KERNEL_PROFILE).config

ifeq ($(NEO_KERNEL_PROFILE),perf)
# ThinLTO. Full LTO was tried (Nov 16): with the AutoFDO profile applied its single
# vmlinux link needed ~40 GB of RAM (ld.lld; killed by the OOM killer / systemd-oomd)
# for an expected gain within noise of ThinLTO.
KERNEL_LTO := thin
NEO_KERNEL_TAG := NEO VORTEX SPECIAL OPTIMIZATION
else
KERNEL_LTO := none
NEO_KERNEL_TAG := NEO VORTEX DEBUG BUILD
endif

# uname -v: "#1 SMP PREEMPT <date> <tag>", /proc/version: built by neo@ingres-lab
TARGET_KERNEL_ADDITIONAL_FLAGS += \
    KBUILD_BUILD_USER=neo \
    KBUILD_BUILD_HOST=ingres-lab \
    KBUILD_BUILD_TAG="$(NEO_KERNEL_TAG)"

# Tune scheduling for the Cortex-A710 (SM8450 mid cores; the X2 prime core has a near-identical
# pipeline). Measured on the device (bench/results/table-tuning.txt): no effect outside the 1-2%
# noise floor, and the image grows ~2%. Kept on request; remove this line to go back to generic.
# KCFLAGS applies to the built-in kernel; under ThinLTO -mtune becomes a per-function IR attribute.
TARGET_KERNEL_ADDITIONAL_FLAGS += KCFLAGS=-mtune=cortex-a710

# Profile-guided (AutoFDO) kernel. The current neo/autofdo/kernel.afdo (v2, 2026-11-16) was recorded on a full-LTO
# profiling kernel built without a profile, with simpleperf's own functions (kallsyms, perf_output_*) filtered out
# (bench/: kbuild_variant.sh, prof_session.sh, make_profile.sh). The measurements below are from the v1 ThinLTO profile.
# neo/autofdo/kernel.afdo was recorded on the device itself (CoreSight ETE trace of the
# running kernel under a mixed app/network/storage/process workload, converted with llvm-profgen). It only affects the
# built-in kernel (loadable modules and the EL2 hypervisor/vDSO/EFI-stub code are excluded in the kernel tree).
# Measured (bench/, vs the same kernel without it, noise floor +-2%): +13% / +6% / +7% geometric mean on the kernel
# micro-benchmarks for the A510 / A710 / X2 cores, -23% instruction-cache misses on process exec, -14% on wakeups.
# AFDO_HOT_CUTOFF=900000 treats the hottest 90% of samples as hot; the compiler default (99%) bloated .text by 50%.
# Set NEO_KERNEL_AUTOFDO=false in the environment to build without the profile.
NEO_KERNEL_AUTOFDO ?= true
ifeq ($(NEO_KERNEL_AUTOFDO),true)
TARGET_KERNEL_ADDITIONAL_FLAGS += \
    CLANG_AUTOFDO_PROFILE=$(abspath device/xiaomi/ingres/neo/autofdo/kernel.afdo) \
    AFDO_HOT_CUTOFF=900000
endif

# The firmware traps EL2 accesses to the SVE/SME registers; without these the
# 6.6 kernel dies silently in setup_arch before any console output.
BOARD_KERNEL_CMDLINE += arm64.nosve arm64.nosme

TARGET_KERNEL_SOURCE := kernel/xiaomi/sm8450
TARGET_KERNEL_EXT_MODULE_ROOT := kernel/xiaomi/sm8450-modules
TARGET_KERNEL_NO_GCC := true

# Display: build the HAL from the 5.10 sm8450 tree. The sm8450-6.6 HAL's
# HWResourceInfo layout does not match the HyperOS libsdmextension blob and
# the composer aborts in SdmPropUtils with std::length_error.
USE_DEVICE_SPECIFIC_DISPLAY := true
DEVICE_SPECIFIC_DISPLAY_PATH := hardware/qcom-caf/sm8450/display
QCOM_DISPLAY_SOONG_NAMESPACE := hardware/qcom-caf/sm8450/display

# Audio: the 5.10 HIDL stack (primary HAL, AGMIPC@1.0 daemon, PAL/AGM) from the
# sm8450 tree. sm8450-6.6 only has the all-AIDL HAL, whose AGM client needs an
# in-process IAGM server the legacy audio.service never starts.
USE_DEVICE_SPECIFIC_AUDIO := true
DEVICE_SPECIFIC_AUDIO_PATH := hardware/qcom-caf/sm8450/audio
QCOM_AUDIO_SOONG_NAMESPACE := hardware/qcom-caf/sm8450/audio
QCOM_AUDIO_USE_LEGACY_STHAL := true

# 6.6 module set (differs from the 5.10 common list):
# - cvp-kernel is not present in the Sony 6.6 modules tree
# - kgsl, mm/sync/synx/securemsm are external modules in 6.6
# - touch is the in-kernel Xiaomi fts_spi driver; Sony's touch-drivers/st
#   would also bind st,spi, so the touch-drivers tree is left out
# Xiaomi drivers ported to 6.6 (not in the sm8450-common lists)
# tipc: netmgrd and qcrild (libnetmgr) exit when netmgr_tipc_init fails, and
# nothing else loads it on 6.6
# rmnet_ctl: built for waipio now; rmnet_core hard-depends on it
# mt7601u: MT7601U USB Wi-Fi dongle over OTG (monitor/injection), pulls in mac80211
# ntsync: Wine NT sync primitives (/dev/ntsync)
# cdsp-loader: creates /sys/kernel/boot_cdsp/boot, which vendor init.qti.kernel.rc writes on early-boot to
#   start the compute DSP. Without it the CDSP stays offline and every CDSP offload (camera AI nodes,
#   SNPE/QNN HTP, BITML) gets ECONNREFUSED; MiuiCamera's HAL then crashes.
# ch341/cp210x: USB serial adapters; btusb: USB Bluetooth adapters (OTG)
# rtw88_*u / mt76x2u / 88XXau: more USB Wi-Fi dongles for monitor/injection (RTL8822BU, RTL8821CU,
#   RTL8822CU, RTL8723DU, MT7612U, RTL8812AU/8821AU/8814AU from the aircrack-ng driver)
# nxp-nci: NXP SN1xx NFC (DT qcom,sn-nci @ i2c-1 0x28). Creates /dev/nq-nci, which the
#   android.hardware.nfc-service.nxp HAL opens; without it NFC never initializes.
BOARD_VENDOR_KERNEL_MODULES_LOAD += \
    nxp-nci.ko \
    hwid.ko \
    fts_touch_spi.ko \
    mi_thermal_interface.ko \
    xm_gamekey.ko \
    aw8697-haptic.ko \
    leds-aw22xxx.ko \
    cs35l41_dlkm.ko \
    btpower.ko \
    bt_fm_slim.ko \
    tipc.ko \
    rmnet_ctl.ko \
    mt7601u.ko \
    ntsync.ko \
    ch341.ko \
    cp210x.ko \
    btusb.ko \
    cdsp-loader.ko \
    rtw88_8822bu.ko \
    rtw88_8821cu.ko \
    rtw88_8822cu.ko \
    rtw88_8723du.ko \
    mt76x2u.ko \
    88XXau.ko

# Recovery (OrangeFox/TWRP/Lineage) needs touch: in normal boot fts_touch_spi.ko loads late from
# vendor_dlkm, but recovery only sees the vendor ramdisk, so ship it (and its hwid dependency)
# there and load it in recovery mode only. Firmware st_fts_l10*.ftb comes from /vendor/firmware.
BOOT_KERNEL_MODULES += \
    hwid.ko \
    fts_touch_spi.ko
BOARD_VENDOR_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD += \
    hwid.ko \
    fts_touch_spi.ko

TARGET_KERNEL_EXT_MODULES := \
    rtl8812au:kbuild \
    qcom/opensource/mmrm-driver \
    qcom/opensource/mm-drivers/sync_fence \
    qcom/opensource/mm-drivers/msm_ext_display \
    qcom/opensource/synx-kernel \
    qcom/opensource/securemsm-kernel \
    nxp/opensource/driver \
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
    qcom/opensource/audio-kernel \
    qcom/opensource/bt-kernel \
    qcom/opensource/eva-kernel \
    qcom/opensource/video-driver \
    qcom/opensource/display-drivers/msm \
    qcom/opensource/graphics-kernel \
    qcom/opensource/wlan/platform \
    qcom/opensource/wlan/qcacld-3.0/.qca6490

# ingres has a qca6490 (0x1103); the qca6750 build exports the same symbols
# and loads first, so qca_cld3_qca6490 fails with "duplicate symbol"
BOARD_VENDOR_KERNEL_MODULES_LOAD := $(filter-out qca_cld3_qca6750.ko,$(BOARD_VENDOR_KERNEL_MODULES_LOAD))

BOARD_BOOT_HEADER_VERSION := 4
TARGET_NEEDS_DTBOIMAGE := true
BOARD_USES_QCOM_MERGE_DTBS_SCRIPT := true

# The release userspace blobs (adpl) link the legacy rmnet control API. With
# TARGET_KERNEL_VERSION=6.6, qcom_defs.mk moves taro/sm8450 into UM_6_6_FAMILY,
# which removes the opt-in that enabled USE_OLD_RMNET_DATA on 5.10. Select the
# legacy API explicitly so librmnetctl exports the symbols adpl needs.
$(call soong_config_set,rmnetctl,old_rmnet_data,true)
