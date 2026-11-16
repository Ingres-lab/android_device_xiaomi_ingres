/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/vibrator/BnVibrator.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "HapticDevice.h"

namespace aidl::android::hardware::vibrator {

class NeoVibrator : public BnVibrator {
  public:
    explicit NeoVibrator(std::shared_ptr<neo_haptics::HapticDevice> device, bool deviceOk);
    ~NeoVibrator() override;

    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* maxDelayMs) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* supported) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive,
                                            int32_t* durationMs) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* resonantFreqHz) override;
    ndk::ScopedAStatus getQFactor(float* qFactor) override;
    ndk::ScopedAStatus getFrequencyResolution(float* freqResolutionHz) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* freqMinimumHz) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* durationMs) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* supported) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;

  private:
    struct Segment {
        int32_t startMs;
        std::vector<int8_t> samples;
    };

    void startJob(std::vector<Segment> segments, int32_t totalMs,
                  std::shared_ptr<IVibratorCallback> callback, int32_t constantMs);
    void cancelJob();
    std::vector<int8_t> sineBurst(int32_t durationMs, float amplitude) const;

    std::shared_ptr<neo_haptics::HapticDevice> mDevice;
    const bool mDeviceOk;
    std::atomic<float> mAmplitude{1.0f};

    std::mutex mApiMutex;
    std::mutex mJobMutex;
    std::condition_variable mJobCv;
    bool mCancel = false;
    std::thread mThread;
};

}  // namespace aidl::android::hardware::vibrator
