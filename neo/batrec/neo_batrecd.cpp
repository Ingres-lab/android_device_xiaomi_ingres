/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 *
 * neo_batrecd: battery / charger / suspend black-box recorder.
 *
 * It writes one line every 10 s into the system log (tag NeoBat) and a larger snapshot every
 * 5 minutes (tag NeoSnap). logcatd keeps every log buffer on disk across restarts and the bug
 * report behind Settings > Neo > Logs collects them, so no file handling or data-partition
 * SELinux type is needed here. The aim is to catch what the phone was doing in the minutes
 * before a random shutdown or a hang, which no log has shown so far.
 *
 * Mode (persist.sys.neo.batrec.mode, set from NeoParts):
 *   quiet   default. Reads nothing that asks the ADSP battery manager a question: uptime,
 *           suspend counters, wake-up sources, cpuidle, thermal zones except "battery".
 *   active  also reads the battery, USB and Xiaomi gauge nodes (every read is a message to
 *           the ADSP). Everything that logged healthy sessions before ran like this, so it may
 *           hide the slow-update fault: compare the two.
 *   off     records nothing.
 */

#define LOG_TAG "neo_batrecd"

#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/log.h>

namespace {

constexpr int kTickSec = 10;
constexpr int kSnapEveryTicks = 30;  // 5 minutes

const char* kModeProp = "persist.sys.neo.batrec.mode";

// Battery and USB supply: every read asks the ADSP.
const char* kPsBattery[][2] = {
    {"cap", "capacity"},         {"v", "voltage_now"},      {"ocv", "voltage_ocv"},
    {"i", "current_now"},        {"temp", "temp"},          {"st", "status"},
    {"hl", "health"},            {"cc", "charge_counter"},  {"cf", "charge_full"},
    {"cfd", "charge_full_design"}, {"cyc", "cycle_count"},  {"tte", "time_to_empty_avg"},
    {"ttf", "time_to_full_avg"}, {"ctype", "charge_type"},  {"cccm", "constant_charge_current_max"},
};
const char* kPsUsb[][2] = {
    {"uon", "online"}, {"utype", "usb_type"}, {"ucur", "current_max"}, {"uv", "voltage_now"},
};
// Xiaomi gauge / charger nodes (/sys/class/qcom-battery).
const char* kQcom[][2] = {
    {"raw", "fg_raw_soc"},      {"rsoc", "fg1_rsoc"},        {"gv1", "fg1_cell1_vol"},
    {"gv2", "fg1_cell2_vol"},   {"fcc", "fg1_fcc"},          {"qmax", "fg1_qmax"},
    {"rm", "fg1_rm"},           {"tremq", "fg1_tremq"},      {"tfullq", "fg1_tfullq"},
    {"gcyc", "fg1_cycle"},      {"soh", "soh"},              {"op", "op_mode"},
    {"qct", "quick_charge_type"}, {"rtype", "real_type"},    {"fast", "fastchg_mode"},
    {"chgen", "charging_enabled"}, {"insusp", "input_suspend"}, {"night", "night_charging"},
    {"smart", "smart_batt"},    {"rchg", "restrict_chg"},    {"rcur", "restrict_cur"},
    {"aid", "adapter_id"},      {"bqbv", "bq2597x_bus_voltage"}, {"bqbi", "bq2597x_bus_current"},
    {"bqv", "bq2597x_battery_voltage"}, {"bqok", "bq2597x_chip_ok"},
    {"smb", "master_smb1396_iin"}, {"smb2", "slave_smb1396_iin"}, {"dieT", "die_temperature"},
    {"connT", "connector_temp"}, {"res", "resistance"},      {"shut", "shutdown_delay"},
    {"auth", "authentic"},      {"chipok", "chip_ok"},       {"ocvdev", "current_deviation"},
};

std::string read1(const std::string& path, size_t max = 80) {
    std::string s;
    if (!android::base::ReadFileToString(path, &s)) return "-";
    s = android::base::Trim(s);
    std::replace(s.begin(), s.end(), '\n', '|');
    std::replace(s.begin(), s.end(), ' ', '_');
    if (s.size() > max) s.resize(max);
    return s.empty() ? "-" : s;
}

std::vector<std::string> list(const std::string& dir, const char* prefix) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (prefix && strncmp(e->d_name, prefix, strlen(prefix))) continue;
        out.push_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// logd takes ~4000 bytes per line; split long text on field boundaries.
void emit(const char* tag, const std::string& line) {
    constexpr size_t kMax = 3000;
    size_t pos = 0;
    while (pos < line.size()) {
        size_t end = std::min(line.size(), pos + kMax);
        if (end < line.size()) {
            size_t sp = line.rfind(' ', end);
            if (sp != std::string::npos && sp > pos) end = sp;
        }
        __android_log_print(ANDROID_LOG_INFO, tag, "%s", line.substr(pos, end - pos).c_str());
        pos = end;
        while (pos < line.size() && line[pos] == ' ') pos++;
    }
}

void add(std::string& line, const char* key, const std::string& value) {
    line += key;
    line += '=';
    line += value;
    line += ' ';
}

std::string uptime_field(int idx) {
    std::string s;
    if (!android::base::ReadFileToString("/proc/uptime", &s)) return "-";
    auto parts = android::base::Split(android::base::Trim(s), " ");
    return idx < (int)parts.size() ? parts[idx] : "-";
}

void tick(const std::string& mode, long seq) {
    std::string l;
    add(l, "seq", std::to_string(seq));
    add(l, "mode", mode);
    add(l, "up", uptime_field(0));
    add(l, "idle", uptime_field(1));
    add(l, "load", android::base::Split(read1("/proc/loadavg", 40), "_")[0]);
    // Suspend counters: success stays 0 while the phone never deep-sleeps.
    add(l, "sus_ok", read1("/sys/power/suspend_stats/success"));
    add(l, "sus_fail", read1("/sys/power/suspend_stats/fail"));
    add(l, "sus_dev", read1("/sys/power/suspend_stats/last_failed_dev"));
    add(l, "sus_err", read1("/sys/power/suspend_stats/last_failed_errno"));
    add(l, "sus_step", read1("/sys/power/suspend_stats/last_failed_step"));
    if (mode == "active") {
        for (auto& e : kPsBattery) add(l, e[0], read1(std::string("/sys/class/power_supply/battery/") + e[1]));
        for (auto& e : kPsUsb) add(l, e[0], read1(std::string("/sys/class/power_supply/usb/") + e[1]));
        for (auto& e : kQcom) add(l, e[0], read1(std::string("/sys/class/qcom-battery/") + e[1]));
    }
    emit("NeoBat", l);
}

void snapshot(const std::string& mode) {
    // Which kernel wake-up sources have been busy, and which held suspend off.
    std::string w = "wakeup(name:active/wakeups/total_ms/prevent_ms) ";
    int n = 0;
    for (const auto& d : list("/sys/class/wakeup", "wakeup")) {
        const std::string base = "/sys/class/wakeup/" + d + "/";
        const std::string total = read1(base + "total_time_ms", 12);
        const std::string prevent = read1(base + "prevent_suspend_time_ms", 12);
        if ((total == "0" || total == "-") && (prevent == "0" || prevent == "-")) continue;
        w += read1(base + "name", 40) + ":" + read1(base + "active_count", 10) + "/" +
             read1(base + "wakeup_count", 10) + "/" + total + "/" + prevent + " ";
        if (++n >= 60) break;
    }
    emit("NeoSnap", w);

    // CPU idle states (time spent, entries) and current frequency per CPU.
    std::string c = "cpu ";
    for (const auto& cpu : list("/sys/devices/system/cpu", "cpu")) {
        if (cpu.size() < 4 || !isdigit(cpu[3])) continue;
        const std::string base = "/sys/devices/system/cpu/" + cpu + "/";
        c += cpu + "[f=" + read1(base + "cpufreq/scaling_cur_freq", 10);
        for (const auto& st : list(base + "cpuidle", "state")) {
            c += " " + st + "=" + read1(base + "cpuidle/" + st + "/time", 14) + "/" +
                 read1(base + "cpuidle/" + st + "/usage", 12);
        }
        c += "] ";
    }
    emit("NeoSnap", c);

    // Thermal zones. The "battery" zone is read from the ADSP, so quiet mode skips it.
    std::string t = "thermal ";
    for (const auto& z : list("/sys/class/thermal", "thermal_zone")) {
        const std::string base = "/sys/class/thermal/" + z + "/";
        const std::string type = read1(base + "type", 30);
        if (type == "battery" && mode != "active") continue;
        t += type + "=" + read1(base + "temp", 10) + " ";
    }
    emit("NeoSnap", t);

    // Memory pressure.
    std::string m = "mem ";
    std::string meminfo;
    if (android::base::ReadFileToString("/proc/meminfo", &meminfo)) {
        for (const auto& line : android::base::Split(meminfo, "\n")) {
            if (android::base::StartsWith(line, "MemAvailable") ||
                android::base::StartsWith(line, "SwapFree") ||
                android::base::StartsWith(line, "Dirty:")) {
                m += android::base::StringReplace(android::base::Trim(line), " ", "", true) + " ";
            }
        }
    }
    emit("NeoSnap", m);
}

void boot_line() {
    struct utsname u;
    uname(&u);
    std::string l;
    add(l, "fingerprint", android::base::GetProperty("ro.build.fingerprint", "-"));
    add(l, "display", android::base::GetProperty("ro.build.display.id", "-"));
    add(l, "kernel", u.release);
    add(l, "boot_reason", android::base::GetProperty("sys.boot.reason", "-"));
    add(l, "bootreason_hw", android::base::GetProperty("ro.boot.bootreason", "-"));
    add(l, "history", android::base::GetProperty("persist.sys.boot.reason.history", "-"));
    add(l, "last", android::base::GetProperty("sys.boot.reason.last", "-"));
    add(l, "slot", android::base::GetProperty("ro.boot.slot_suffix", "-"));
    add(l, "mem_sleep", read1("/sys/power/mem_sleep"));
    emit("NeoBoot", l);
}

}  // namespace

int main() {
    boot_line();
    long seq = 0;
    while (true) {
        const std::string mode = android::base::GetProperty(kModeProp, "quiet");
        if (mode == "quiet" || mode == "active") {
            tick(mode, seq);
            if (seq % kSnapEveryTicks == 0) snapshot(mode);
            seq++;
        }
        poll(nullptr, 0, kTickSec * 1000);
    }
}
