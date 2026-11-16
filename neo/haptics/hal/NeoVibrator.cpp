/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "NeoVibrator"

#include "NeoVibrator.h"

#include <android-base/logging.h>
#include <android/binder_status.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "neo_waves.h"

namespace aidl::android::hardware::vibrator {

namespace {

using neo_haptics::Wave;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

constexpr int32_t kCompositionSizeMax = 16;
constexpr int32_t kCompositionDelayMax = 1000;
constexpr int32_t kMergeGapMs = 40;          // gaps up to this are streamed as silence
constexpr int32_t kShortBurstMaxMs = 250;    // longer on() requests use the driver's loop effect
constexpr int32_t kSamplesPerMs = neo_haptics::kSampleRate / 1000;

const Wave* find(const Wave* table, size_t n, const char* name) {
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(table[i].name, name)) return &table[i];
    }
    return nullptr;
}

const Wave* fx(const char* name) {
    return find(neo_haptics::k_fx_table, sizeof(neo_haptics::k_fx_table) / sizeof(Wave), name);
}

const Wave* prim(CompositePrimitive p) {
    const char* name = nullptr;
    switch (p) {
        case CompositePrimitive::CLICK: name = "CLICK"; break;
        case CompositePrimitive::THUD: name = "THUD"; break;
        case CompositePrimitive::SPIN: name = "SPIN"; break;
        case CompositePrimitive::QUICK_RISE: name = "QUICK_RISE"; break;
        case CompositePrimitive::SLOW_RISE: name = "SLOW_RISE"; break;
        case CompositePrimitive::QUICK_FALL: name = "QUICK_FALL"; break;
        case CompositePrimitive::LIGHT_TICK: name = "LIGHT_TICK"; break;
        case CompositePrimitive::LOW_TICK: name = "LOW_TICK"; break;
        default: return nullptr;
    }
    return find(neo_haptics::k_prim_table, sizeof(neo_haptics::k_prim_table) / sizeof(Wave), name);
}

std::vector<int8_t> scaled(const Wave* w, float scale) {
    scale *= neo_haptics::HapticDevice::strength();  // master strength from NeoParts
    std::vector<int8_t> out(w->len);
    for (size_t i = 0; i < w->len; i++) {
        const float v = w->data[i] * scale;
        out[i] = (int8_t)std::max(-127.0f, std::min(127.0f, std::round(v)));
    }
    return out;
}

int32_t durationMs(size_t samples) {
    return (int32_t)((samples + kSamplesPerMs - 1) / kSamplesPerMs);
}

float strengthScale(EffectStrength s) {
    switch (s) {
        case EffectStrength::LIGHT: return 0.70f;
        case EffectStrength::STRONG: return 1.35f;
        default: return 1.0f;
    }
}

}  // namespace

NeoVibrator::NeoVibrator(std::shared_ptr<neo_haptics::HapticDevice> device, bool deviceOk)
    : mDevice(std::move(device)), mDeviceOk(deviceOk) {}

NeoVibrator::~NeoVibrator() {
    cancelJob();
}

ndk::ScopedAStatus NeoVibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = mDeviceOk ? (IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                                 IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS)
                              : 0;
    return ndk::ScopedAStatus::ok();
}

void NeoVibrator::cancelJob() {
    {
        std::lock_guard<std::mutex> l(mJobMutex);
        mCancel = true;
    }
    mJobCv.notify_all();
    if (mThread.joinable()) mThread.join();
}

void NeoVibrator::startJob(std::vector<Segment> segments, int32_t totalMs,
                           std::shared_ptr<IVibratorCallback> callback, int32_t constantMs) {
    cancelJob();
    {
        std::lock_guard<std::mutex> l(mJobMutex);
        mCancel = false;
    }
    mThread = std::thread([this, segments = std::move(segments), totalMs, callback, constantMs]() {
        const auto start = steady_clock::now();
        auto waitUntil = [&](int32_t ms) {
            std::unique_lock<std::mutex> l(mJobMutex);
            return !mJobCv.wait_until(l, start + milliseconds(ms), [this] { return mCancel; });
        };
        if (constantMs > 0) {
            if (!mDevice->playConstant(constantMs)) LOG(ERROR) << "playConstant failed";
        }
        for (const Segment& s : segments) {
            if (!waitUntil(s.startMs)) return;   // cancelled
            if (!mDevice->playWave(s.samples)) LOG(ERROR) << "playWave failed";
        }
        if (!waitUntil(totalMs)) return;
        if (constantMs > 0) mDevice->stop();
        if (callback) callback->onComplete();
    });
}

ndk::ScopedAStatus NeoVibrator::off() {
    std::lock_guard<std::mutex> l(mApiMutex);
    cancelJob();
    mDevice->stop();
    return ndk::ScopedAStatus::ok();
}

std::vector<int8_t> NeoVibrator::sineBurst(int32_t ms, float amplitude) const {
    amplitude *= neo_haptics::HapticDevice::strength();  // master strength from NeoParts
    float f0 = mDevice->resonantFrequencyHz();
    if (f0 < 50.0f || f0 > 400.0f) f0 = neo_haptics::kF0Hz;
    const int n = ms * kSamplesPerMs;
    const int attack = std::min(n / 2, 2 * kSamplesPerMs);
    const int release = std::min(n / 2, 6 * kSamplesPerMs);
    std::vector<float> w(n);
    for (int i = 0; i < n; i++) {
        float env = 1.0f;
        if (i < attack) env = 0.5f - 0.5f * std::cos(M_PI * i / attack);
        if (i >= n - release) env = std::min(env, (float)(0.5f - 0.5f * std::cos(M_PI * (n - 1 - i) / release)));
        w[i] = amplitude * env * std::sin(2.0 * M_PI * f0 * i / neo_haptics::kSampleRate);
    }
    // Brake: a short opposing-phase burst so the mass stops instead of ringing down.
    const int brake = (int)(0.6f * neo_haptics::kSampleRate / f0);
    for (int i = 0; i < brake; i++) {
        const float t = (float)i / std::max(1, brake - 1);
        const float env = 0.5f - 0.5f * std::cos(2.0 * M_PI * t * 0.8) * (1.0f - t);
        w.push_back(-0.4f * amplitude * env *
                    std::sin(2.0 * M_PI * f0 * i / neo_haptics::kSampleRate));
    }
    std::vector<int8_t> out(w.size());
    for (size_t i = 0; i < w.size(); i++) {
        out[i] = (int8_t)std::max(-127.0f, std::min(127.0f, std::round(w[i] * 127.0f)));
    }
    return out;
}

ndk::ScopedAStatus NeoVibrator::on(int32_t timeoutMs,
                                   const std::shared_ptr<IVibratorCallback>& callback) {
    if (!mDeviceOk) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    if (timeoutMs < 0) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    std::lock_guard<std::mutex> l(mApiMutex);
    if (timeoutMs == 0) {
        cancelJob();
        mDevice->stop();
        return ndk::ScopedAStatus::ok();
    }
    if (timeoutMs <= kShortBurstMaxMs) {
        std::vector<Segment> seg;
        seg.push_back({0, sineBurst(timeoutMs, 0.9f * mAmplitude.load())});
        startJob(std::move(seg), timeoutMs, callback, 0);
    } else {
        startJob({}, timeoutMs, callback, timeoutMs);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK, Effect::THUD,
                     Effect::POP,   Effect::HEAVY_CLICK,  Effect::TEXTURE_TICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::perform(Effect effect, EffectStrength strength,
                                        const std::shared_ptr<IVibratorCallback>& callback,
                                        int32_t* _aidl_return) {
    if (!mDeviceOk) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    const char* name = nullptr;
    switch (effect) {
        case Effect::CLICK: name = "impact_medium"; break;
        case Effect::DOUBLE_CLICK: name = "tick_double"; break;
        case Effect::TICK: name = "selection"; break;
        case Effect::THUD: name = "impact_soft"; break;
        case Effect::POP: name = "impact_rigid"; break;
        case Effect::HEAVY_CLICK: name = "impact_heavy"; break;
        case Effect::TEXTURE_TICK: name = "scroll_detent"; break;
        default: return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (strength != EffectStrength::LIGHT && strength != EffectStrength::MEDIUM &&
        strength != EffectStrength::STRONG) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const Wave* w = fx(name);
    if (!w) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);

    std::lock_guard<std::mutex> l(mApiMutex);
    std::vector<Segment> seg;
    seg.push_back({0, scaled(w, strengthScale(strength))});
    const int32_t ms = durationMs(w->len);
    startJob(std::move(seg), ms, callback, 0);
    *_aidl_return = ms;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::setAmplitude(float amplitude) {
    if (amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (!mDeviceOk) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    mAmplitude.store(amplitude);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::setExternalControl(bool) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus NeoVibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    *maxDelayMs = kCompositionDelayMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::getCompositionSizeMax(int32_t* maxSize) {
    *maxSize = kCompositionSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
    *supported = {CompositePrimitive::CLICK,      CompositePrimitive::THUD,
                  CompositePrimitive::SPIN,       CompositePrimitive::QUICK_RISE,
                  CompositePrimitive::SLOW_RISE,  CompositePrimitive::QUICK_FALL,
                  CompositePrimitive::LIGHT_TICK, CompositePrimitive::LOW_TICK};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                     int32_t* durationMsOut) {
    if (primitive == CompositePrimitive::NOOP) {
        *durationMsOut = 0;
        return ndk::ScopedAStatus::ok();
    }
    const Wave* w = prim(primitive);
    if (!w) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    *durationMsOut = durationMs(w->len);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::compose(const std::vector<CompositeEffect>& composite,
                                        const std::shared_ptr<IVibratorCallback>& callback) {
    if (!mDeviceOk) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    if (composite.size() > (size_t)kCompositionSizeMax) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    for (const auto& e : composite) {
        if (e.delayMs > kCompositionDelayMax || e.delayMs < 0 || e.scale < 0.0f || e.scale > 1.0f) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (e.primitive != CompositePrimitive::NOOP && !prim(e.primitive)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    std::vector<Segment> segments;
    int32_t t = 0;   // end of the previous primitive
    for (const auto& e : composite) {
        t += e.delayMs;
        if (e.primitive == CompositePrimitive::NOOP) continue;
        const Wave* w = prim(e.primitive);
        std::vector<int8_t> s = scaled(w, e.scale);
        if (!segments.empty()) {
            Segment& last = segments.back();
            const int32_t lastEnd = last.startMs + durationMs(last.samples.size());
            const int32_t gap = t - lastEnd;
            if (gap >= 0 && gap <= kMergeGapMs &&
                last.samples.size() + (size_t)gap * kSamplesPerMs + s.size() <=
                        neo_haptics::HapticDevice::kMaxStreamBytes) {
                last.samples.insert(last.samples.end(), (size_t)gap * kSamplesPerMs, 0);
                last.samples.insert(last.samples.end(), s.begin(), s.end());
                t = last.startMs + durationMs(last.samples.size());
                continue;
            }
        }
        segments.push_back({t, std::move(s)});
        t += durationMs(w->len);
    }
    const int32_t total = t;

    std::lock_guard<std::mutex> l(mApiMutex);
    if (segments.empty()) {
        if (callback) callback->onComplete();
        return ndk::ScopedAStatus::ok();
    }
    startJob(std::move(segments), total, callback, 0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus NeoVibrator::getSupportedAlwaysOnEffects(std::vector<Effect>*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::alwaysOnEnable(int32_t, Effect, EffectStrength) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::alwaysOnDisable(int32_t) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getResonantFrequency(float*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getQFactor(float*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getFrequencyResolution(float*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getFrequencyMinimum(float*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getBandwidthAmplitudeMap(std::vector<float>*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getPwlePrimitiveDurationMax(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getPwleCompositionSizeMax(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::getSupportedBraking(std::vector<Braking>*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
ndk::ScopedAStatus NeoVibrator::composePwle(const std::vector<PrimitivePwle>&,
                                            const std::shared_ptr<IVibratorCallback>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace aidl::android::hardware::vibrator
