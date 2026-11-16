#!/system/bin/sh
# Neo: collect everything needed to debug a random shutdown or a wrong battery percentage.
# In the ROM as a command: adb root && adb shell neo-collect-logs   (or: su -c neo-collect-logs)
# The same data is behind Settings > Neo > Logs and battery recorder > Share logs.
# Output: /sdcard/Download/shutdown-logs-<date>.tar.gz
d=/data/local/tmp/sd-$(date +%Y%m%d-%H%M%S); mkdir -p $d
{ echo "== boot reason"; getprop sys.boot.reason; getprop sys.boot.reason.last; getprop persist.sys.boot.reason.history; getprop ro.boot.bootreason
  echo "== uptime"; uptime; cat /proc/uptime
  echo "== build"; getprop ro.build.fingerprint; uname -a
  echo "== battery"; dumpsys battery; for f in capacity temp status health voltage_now current_now charge_type; do echo $f=$(cat /sys/class/power_supply/battery/$f 2>&1); done
  echo "== thermal"; for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type) $(cat $z/temp)"; done
  echo "== gauge (qcom-battery)"; for f in /sys/class/qcom-battery/*; do [ -f $f ] && echo "$(basename $f)=$(cat $f 2>/dev/null | head -c 160)"; done
  echo "== power_supply"; for p in /sys/class/power_supply/*; do echo "-- $p"; for f in $p/*; do [ -f $f ] && echo "$(basename $f)=$(cat $f 2>/dev/null | head -c 100)"; done; done
  echo "== pstore"; ls -la /sys/fs/pstore /sys/fs/pstore/* 2>&1
  echo "== bootstat"; ls -la /data/misc/bootstat; cat /data/misc/bootstat/* 2>&1 | strings | head -80
} > $d/info.txt 2>&1
cp -r /sys/fs/pstore $d/pstore 2>/dev/null
cp -r /data/local/tmp/mon $d/mon 2>/dev/null
cp -r /data/system/dropbox $d/dropbox 2>/dev/null
# persistent log (all buffers incl. the kernel log, kept across restarts by logcatd) and the recorder lines
cp -r /data/misc/logd $d/logd 2>/dev/null
cat /data/misc/logd/logcat* 2>/dev/null | grep -a -E "NeoBat|NeoSnap|NeoBoot" > $d/neobat.txt 2>/dev/null
logcat -b all -d -v threadtime 2>/dev/null | grep -a -E "NeoBat|NeoSnap|NeoBoot" >> $d/neobat.txt
# the previous power cycle: why the PMIC reset (PS_HOLD = software, KPDPWR_N_S2 = power-key/hardware reset)
dmesg 2>/dev/null | grep -a "PMIC PON log" > $d/pmic-reset-reason.txt
cp /data/misc/bootstat/* $d/ 2>/dev/null
cp -r /data/tombstones $d/tombstones 2>/dev/null
cp -r /data/anr $d/anr 2>/dev/null
dmesg > $d/dmesg.txt 2>&1
logcat -b all -d -v threadtime > $d/logcat-all.txt 2>&1
logcat -b events -d -v threadtime > $d/logcat-events.txt 2>&1
dumpsys batterystats --checkin > $d/batterystats-checkin.txt 2>&1
dumpsys batterystats > $d/batterystats.txt 2>&1
dumpsys thermalservice > $d/thermalservice.txt 2>&1
dumpsys power > $d/power.txt 2>&1
dumpsys activity exit-info > $d/exit-info.txt 2>&1
dumpsys dropbox --print > $d/dropbox-print.txt 2>&1
out=/sdcard/Download/shutdown-logs-$(date +%Y%m%d-%H%M%S)
if tar czf $out.tar.gz -C $d . 2> $d/tar-error.txt && [ -s $out.tar.gz ]; then
  rm -rf $d
  ls -l $out.tar.gz
  echo "OK: send $out.tar.gz"
else
  # tar failed or produced an empty file: keep the data as a plain folder instead
  rm -f $out.tar.gz
  cp -r $d $out 2>&1
  rm -rf $d
  echo "WARNING: could not make an archive ($(cat $out/tar-error.txt 2>/dev/null))"
  echo "Logs are in the folder $out : send that whole folder"
  du -sh $out
fi
