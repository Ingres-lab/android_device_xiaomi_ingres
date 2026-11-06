/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 */

// Lights HAL for the rear "X" lights (AW22xxx RGB controller).
//
// Android drives three lights that this phone has no dedicated LED for: BATTERY, NOTIFICATIONS and
// ATTENTION (plus the PRIORITY_NOTIFICATIONS effect used for calls from favourite contacts). They
// are all mapped onto the one controller. What each *event* looks like is configured in NeoParts
// (persist.sys.neo.led.<event>.*), and when several are active at once the highest priority wins:
//
//     call > notification > low battery > charging > idle effect
//
// A call is: the ATTENTION light, a PRIORITY_NOTIFICATIONS effect, or sys.neo.led.call=ringing
// (published by NeoParts from the telephony state, because a plain call notification arrives on the
// ordinary NOTIFICATIONS light with nothing that marks it as a call).

#define LOG_TAG "neo_lights"

#include <aidl/android/hardware/light/BnLights.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <sys/system_properties.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using ::aidl::android::hardware::light::BnLights;
using ::aidl::android::hardware::light::FlashMode;
using ::aidl::android::hardware::light::HwLight;
using ::aidl::android::hardware::light::HwLightEffect;
using ::aidl::android::hardware::light::HwLightState;
using ::aidl::android::hardware::light::LightType;
using ::android::base::GetBoolProperty;
using ::android::base::GetIntProperty;
using ::android::base::GetProperty;
using ::android::base::ReadFileToString;
using ::android::base::StringPrintf;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

namespace {

constexpr const char* kLedDir = "/sys/class/leds/aw22xxx_led/";
constexpr const char* kBatteryStatus = "/sys/class/power_supply/battery/status";
constexpr const char* kProp = "persist.sys.neo.led.";
constexpr const char* kCallState = "sys.neo.led.call";
constexpr const char* kApply = "persist.sys.neo.led.apply";
constexpr const char* kBootGlow = "vendor.neo.led.boot";

constexpr int kIdBattery = 1;
constexpr int kIdNotifications = 2;
constexpr int kIdAttention = 3;
constexpr int kIdPriority = 4;

// Index into aw22xxx_cfg_name[] in the kernel driver
constexpr int kEffectOff = 0;
constexpr int kEffectSteady = 1;
constexpr int kEffectBreath = 2;
constexpr int kEffectFlower = 5;

// Order is priority: the first active event wins.
enum Event { CALL = 0, NOTIF, LOW, CHARGE, EVENT_COUNT };

struct EventDef {
    const char* name;
    bool enabledByDefault;
    int defaultEffect;  // -1 = derive from what Android asked for
};

constexpr EventDef kEvents[EVENT_COUNT] = {
        {"call", true, kEffectFlower},
        {"notif", true, -1},
        {"low", true, -1},
        {"charge", false, -1},
};

// What Android last asked a light to do
struct Request {
    bool on = false;
    int color = 0;  // 0xRRGGBB
    bool timed = false;
    int onMs = 0;
    int offMs = 0;
};

int64_t nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The chip takes each zone's colour bytes as blue, red, green.
std::string chipRgb(int rrggbb) {
    return StringPrintf("%02x%02x%02x", rrggbb & 0xff, (rrggbb >> 16) & 0xff, (rrggbb >> 8) & 0xff);
}

void writeLed(const char* name, const std::string& value) {
    if (!WriteStringToFile(value, std::string(kLedDir) + name)) {
        PLOG(WARNING) << "write " << name << "=" << value;
    }
}

bool isCharging() {
    std::string s;
    if (!ReadFileToString(kBatteryStatus, &s)) return false;
    s = Trim(s);
    return s == "Charging" || s == "Full";
}

class Lights : public BnLights {
  public:
    Lights() {
        std::thread([this] { watch(); }).detach();
        std::lock_guard<std::mutex> lock(mLock);
        evaluateLocked();
    }

    ndk::ScopedAStatus setLightState(int32_t id, const HwLightState& state) override {
        Request r;
        r.color = state.color & 0xffffff;
        r.on = r.color != 0;
        r.timed = state.flashMode == FlashMode::TIMED && state.flashOnMs > 0 && state.flashOffMs > 0;
        r.onMs = state.flashOnMs;
        r.offMs = state.flashOffMs;

        std::lock_guard<std::mutex> lock(mLock);
        switch (id) {
            case kIdBattery:
                mBattery = r;
                break;
            case kIdNotifications:
                mNotification = r;
                break;
            case kIdAttention:
                mAttention = r;
                break;
            default:
                return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
        evaluateLocked();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getLights(std::vector<HwLight>* lights) override {
        lights->push_back({kIdBattery, 0, LightType::BATTERY});
        lights->push_back({kIdNotifications, 0, LightType::NOTIFICATIONS});
        lights->push_back({kIdAttention, 0, LightType::ATTENTION});
        lights->push_back({kIdPriority, 0, LightType::PRIORITY_NOTIFICATIONS});
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus setLightEffects(const std::vector<HwLightEffect>& effects) override {
        std::lock_guard<std::mutex> lock(mLock);
        for (const HwLightEffect& e : effects) {
            if (e.lightId != kIdPriority) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
            }
            // Treat the effect as "a call is ringing for about as long as it would play".
            int64_t ms = static_cast<int64_t>(std::max(e.iterations, 1)) *
                         std::max<int64_t>(e.frames.size(), 1) * std::max(e.framePeriodMillis, 100);
            ms = std::clamp<int64_t>(ms, 3000, 30000);
            mPriorityUntilMs = nowMs() + ms;
        }
        evaluateLocked();
        return ndk::ScopedAStatus::ok();
    }

  private:
    // Wake up when a property changes (NeoParts bumps persist.sys.neo.led.apply; the telephony state
    // is sys.neo.led.call) or when a timed priority effect should end.
    void watch() {
        uint32_t serial = __system_property_area_serial();
        std::string lastKey;
        while (true) {
            const bool timerPending = [this] {
                std::lock_guard<std::mutex> lock(mLock);
                return mPriorityUntilMs > nowMs();
            }();
            timespec timeout{timerPending ? 0 : 10, timerPending ? 500 * 1000000 : 0};
            uint32_t newSerial = serial;
            __system_property_wait(nullptr, serial, &newSerial, &timeout);
            serial = newSerial;

            const std::string key = GetProperty(kCallState, "") + "|" + GetProperty(kApply, "") + "|" +
                                    GetProperty(kBootGlow, "");
            std::lock_guard<std::mutex> lock(mLock);
            if (key != lastKey || mPriorityUntilMs != 0) {
                if (key != lastKey) mApplied.clear();  // settings changed: write again
                lastKey = key;
                evaluateLocked();
            }
        }
    }

    bool active(Event ev) {
        switch (ev) {
            case CALL:
                return mAttention.on || nowMs() < mPriorityUntilMs ||
                       GetProperty(kCallState, "") == "ringing";
            case NOTIF:
                return mNotification.on;
            case LOW:
                return mBattery.on && !isCharging();
            case CHARGE:
                return mBattery.on && isCharging();
            default:
                return false;
        }
    }

    // The request that brought the event about, for deriving a colour and speed it was not
    // configured with.
    const Request& sourceFor(Event ev) {
        static const Request none;
        switch (ev) {
            case CALL:
                return mAttention.on ? mAttention : none;
            case NOTIF:
                return mNotification;
            default:
                return mBattery;
        }
    }

    void evaluateLocked() {
        if (mPriorityUntilMs != 0 && nowMs() >= mPriorityUntilMs) mPriorityUntilMs = 0;

        // neo_lights.rc owns the controller while the boot glow plays
        if (GetBoolProperty(kBootGlow, false)) return;

        for (int i = 0; i < EVENT_COUNT; i++) {
            const Event ev = static_cast<Event>(i);
            if (!active(ev)) continue;

            const std::string p = std::string(kProp) + kEvents[ev].name + ".";
            if (!GetBoolProperty(p + "enable", kEvents[ev].enabledByDefault)) continue;
            const int effect = GetIntProperty(p + "effect", kEvents[ev].defaultEffect);
            if (effect == kEffectOff) continue;
            applyEvent(ev, p, effect);
            return;
        }
        applyIdle();
    }

    void applyEvent(Event ev, const std::string& p, int effect) {
        const Request& src = sourceFor(ev);
        const bool derive = effect < 0;
        const int cfg = GetIntProperty(p + "cfg", derive ? 2 : 1);
        std::string rgb = GetProperty(p + "rgb", "");
        if (rgb.size() != 6) rgb = src.on ? chipRgb(src.color) : "ff00b4";
        int frq = GetIntProperty(p + "frq", 0);
        if (frq <= 0) frq = src.timed ? std::clamp(src.onMs + src.offMs, 300, 4000) : 1500;
        const int e = derive ? (src.timed ? kEffectBreath : kEffectSteady) : effect;
        apply(GetIntProperty(std::string(kProp) + "imax", 3), rgb, frq, e, cfg);
    }

    void applyIdle() {
        std::string rgb = GetProperty(std::string(kProp) + "rgb", "");
        if (rgb.size() != 6) rgb = "ff00b4";
        apply(GetIntProperty(std::string(kProp) + "imax", 3), rgb,
              GetIntProperty(std::string(kProp) + "frq", 1500),
              GetIntProperty(std::string(kProp) + "effect", 0),
              GetIntProperty(std::string(kProp) + "cfg", 1));
    }

    void apply(int imax, const std::string& rgb, int frq, int effect, int cfg) {
        const std::string key = StringPrintf("%d %s %d %d %d", imax, rgb.c_str(), frq, effect, cfg);
        if (key == mApplied) return;
        mApplied = key;
        LOG(INFO) << "light: " << key;

        // Same order as neo_lights.rc: the controller starts playing when cfg is written.
        writeLed("imax", std::to_string(imax));
        for (int zone = 0; zone < 9; zone++) {
            writeLed("rgb", StringPrintf("%d %s", zone, rgb.c_str()));
        }
        writeLed("frq", std::to_string(frq));
        writeLed("effect", std::to_string(effect));
        writeLed("cfg", std::to_string(cfg));
    }

    std::mutex mLock;
    Request mBattery, mNotification, mAttention;
    int64_t mPriorityUntilMs = 0;
    std::string mApplied;
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto lights = ndk::SharedRefBase::make<Lights>();
    const std::string instance = std::string(Lights::descriptor) + "/default";
    CHECK_EQ(STATUS_OK, AServiceManager_addService(lights->asBinder().get(), instance.c_str()));
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // joinThreadPool() should not return
}
