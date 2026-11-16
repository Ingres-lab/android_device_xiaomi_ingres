/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace neo_haptics {

/**
 * Access to the AW86927 haptic motor driver (input device "aw8697_haptic").
 *
 * Short waveforms are streamed through the driver's real-time "custom wave" mode: a force-feedback
 * effect whose first custom value is the custom-wave id announces the stream, the 8-bit samples go
 * into the driver's ring buffer through sysfs `custom_wave` (512-byte periods, a final short write
 * ends the stream), then the effect is played. Long constant vibrations use the driver's built-in
 * RAM-loop effect (FF_CONSTANT), which needs no streaming.
 */
class HapticDevice {
  public:
    /** Finds the input device and the sysfs directory. Returns false if the motor is absent. */
    bool init();

    /** Streams up to kMaxStreamBytes of 24 kHz int8 samples and starts playback. Non-blocking. */
    bool playWave(const std::vector<int8_t>& samples);

    /** Plays the driver's built-in loop effect for durationMs. */
    bool playConstant(int32_t durationMs);

    /** Stops any playback. */
    void stop();

    /** Measured resonance in Hz (driver `f0`, tenths of a hertz), or 0 if unknown. */
    float resonantFrequencyHz();

    /**
     * Master strength set in NeoParts > Haptics (persist.sys.neo.haptic.strength, percent).
     * Read on every call, so a change applies to the next vibration. 0.25 .. 1.5.
     */
    static float strength();

    /** One write to the ring buffer must stay below its 8 KiB size; keep a margin. */
    static constexpr size_t kMaxStreamBytes = 7000;

  private:
    bool uploadLocked(int type, int32_t lengthMs);
    void removeEffectLocked();

    std::mutex mMutex;
    int mInputFd = -1;
    std::string mSysDir;
    int mCustomWaveId = 197;
    int mEffectId = -1;
};

}  // namespace neo_haptics
