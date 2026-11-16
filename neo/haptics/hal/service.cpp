/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "NeoVibrator"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "NeoVibrator.h"

using aidl::android::hardware::vibrator::NeoVibrator;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    auto device = std::make_shared<neo_haptics::HapticDevice>();
    const bool ok = device->init();
    if (!ok) LOG(ERROR) << "haptic motor unavailable, registering a vibrator without capabilities";

    auto vibrator = ndk::SharedRefBase::make<NeoVibrator>(device, ok);
    const std::string instance = std::string(NeoVibrator::descriptor) + "/default";
    const binder_status_t status = AServiceManager_addService(vibrator->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "failed to register " << instance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;   // joinThreadPool should never return
}
