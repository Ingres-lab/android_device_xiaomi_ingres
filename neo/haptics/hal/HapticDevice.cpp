/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "NeoVibrator"

#include "HapticDevice.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>

namespace neo_haptics {

namespace {
constexpr char kInputName[] = "aw8697_haptic";
constexpr char kDefaultSysDir[] =
        "/sys/devices/platform/soc/8c0000.qcom,qupv3_2_geni_se/884000.i2c/i2c-3/3-005a";
constexpr size_t kPeriodBytes = 512;   // custom_wave period size reported by the driver
constexpr int kCustomDataLen = 3;
}  // namespace

bool HapticDevice::init() {
    std::lock_guard<std::mutex> lock(mMutex);

    for (int i = 0; i < 32 && mInputFd < 0; i++) {
        const std::string path = "/dev/input/event" + std::to_string(i);
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        char name[64] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, kInputName)) {
            mInputFd = fd;
            LOG(INFO) << kInputName << " at " << path;
        } else {
            close(fd);
        }
    }
    if (mInputFd < 0) {
        LOG(ERROR) << kInputName << " not found";
        return false;
    }

    mSysDir = kDefaultSysDir;
    std::string info;
    if (!android::base::ReadFileToString(mSysDir + "/custom_wave", &info)) {
        LOG(ERROR) << "cannot read " << mSysDir << "/custom_wave";
        return false;
    }
    const size_t p = info.find("custom_wave_id=");
    if (p != std::string::npos) mCustomWaveId = atoi(info.c_str() + p + strlen("custom_wave_id="));
    LOG(INFO) << "custom wave id " << mCustomWaveId << ", driver: " << android::base::Trim(info);
    return true;
}

float HapticDevice::strength() {
    const int32_t pct = android::base::GetIntProperty("persist.sys.neo.haptic.strength", 100);
    return std::max(25, std::min(150, pct)) / 100.0f;
}

bool HapticDevice::uploadLocked(int type, int32_t lengthMs) {
    struct ff_effect e;
    memset(&e, 0, sizeof(e));
    short data[kCustomDataLen] = {(short)mCustomWaveId, 0, 0};
    e.type = type;
    e.id = mEffectId;
    e.replay.length = lengthMs;
    if (type == FF_PERIODIC) {
        e.u.periodic.waveform = FF_CUSTOM;
        e.u.periodic.magnitude = (short)(0x7fff * std::min(1.0f, strength()));
        e.u.periodic.custom_len = kCustomDataLen;
        e.u.periodic.custom_data = data;
    } else {
        // the built-in loop cannot go above full scale; above 100 % only the waveforms get louder
        e.u.constant.level = (short)(0x7fff * std::min(1.0f, strength()));
    }
    if (ioctl(mInputFd, EVIOCSFF, &e) < 0) {
        PLOG(ERROR) << "EVIOCSFF";
        return false;
    }
    mEffectId = e.id;
    return true;
}

void HapticDevice::removeEffectLocked() {
    if (mEffectId >= 0) {
        struct input_event ev = {};
        ev.type = EV_FF;
        ev.code = mEffectId;
        ev.value = 0;
        (void)!write(mInputFd, &ev, sizeof(ev));
        ioctl(mInputFd, EVIOCRMFF, mEffectId);
        mEffectId = -1;
    }
}

bool HapticDevice::playWave(const std::vector<int8_t>& samples) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mInputFd < 0 || samples.empty()) return false;
    if (samples.size() > kMaxStreamBytes) {
        LOG(ERROR) << "wave too long: " << samples.size();
        return false;
    }

    removeEffectLocked();
    // Uploading the custom-wave effect resets the driver's ring buffer.
    if (!uploadLocked(FF_PERIODIC, 1000)) return false;

    int w = open((mSysDir + "/custom_wave").c_str(), O_WRONLY | O_CLOEXEC);
    if (w < 0) {
        PLOG(ERROR) << "open custom_wave";
        return false;
    }
    size_t off = 0;
    bool ok = true;
    while (off < samples.size()) {
        const size_t n = std::min(kPeriodBytes, samples.size() - off);
        if (write(w, samples.data() + off, n) < 0) {
            PLOG(ERROR) << "write custom_wave";
            ok = false;
            break;
        }
        off += n;
    }
    // An exact multiple of the period needs an empty write to mark the end of the stream.
    if (ok && samples.size() % kPeriodBytes == 0) (void)!write(w, samples.data(), 0);
    close(w);
    if (!ok) return false;

    struct input_event ev = {};
    ev.type = EV_FF;
    ev.code = mEffectId;
    ev.value = 1;
    if (write(mInputFd, &ev, sizeof(ev)) < 0) {
        PLOG(ERROR) << "play";
        return false;
    }
    return true;
}

bool HapticDevice::playConstant(int32_t durationMs) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mInputFd < 0) return false;
    removeEffectLocked();
    if (!uploadLocked(FF_CONSTANT, durationMs)) return false;
    struct input_event ev = {};
    ev.type = EV_FF;
    ev.code = mEffectId;
    ev.value = 1;
    return write(mInputFd, &ev, sizeof(ev)) >= 0;
}

void HapticDevice::stop() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mInputFd >= 0) removeEffectLocked();
}

float HapticDevice::resonantFrequencyHz() {
    std::string v;
    if (!android::base::ReadFileToString(mSysDir + "/f0", &v)) return 0.0f;
    const int tenths = atoi(v.c_str());
    return tenths > 0 ? tenths / 10.0f : 0.0f;
}

}  // namespace neo_haptics
