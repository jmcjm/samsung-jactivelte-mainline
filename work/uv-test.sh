#!/bin/sh
# Live test of the krait-uv module: -25 mV at 1782 MHz on every core, load cpu3, then reset.
set -e
K=/sys/kernel/krait_uv
C3=/sys/devices/system/cpu/cpu3/cpufreq
R3=/sys/class/regulator/regulator.4
insmod /tmp/krait-uv.ko 2>&1 || true
dmesg | grep -i krait_uv | tail -3
echo ---TABLE; cat $K/table
echo userspace > $C3/scaling_governor
bounce() { echo 384000 > $C3/scaling_setspeed; echo 1782000 > $C3/scaling_setspeed; }
bounce; echo "before: f=$(cat $C3/cpuinfo_cur_freq) uV=$(cat $R3/microvolts)"
echo "1782000 1137500" > $K/set
bounce; echo "after -25mV: f=$(cat $C3/cpuinfo_cur_freq) uV=$(cat $R3/microvolts)"
grep '^1782000 ' $K/table
echo ---LOAD-10s
taskset -c 3 sh -c 'end=$(($(date +%s)+10)); while [ $(date +%s) -lt $end ]; do head -c 4000000 /dev/zero | sha256sum >/dev/null; done' &
P=$!; wait $P
echo "during/after load: f=$(cat $C3/cpuinfo_cur_freq) uV=$(cat $R3/microvolts) temp=$(cat /sys/class/thermal/thermal_zone3/temp)"
echo reset > $K/set
bounce; echo "after reset: f=$(cat $C3/cpuinfo_cur_freq) uV=$(cat $R3/microvolts)"
echo "all -12500" > $K/set; grep -E '^(384000|1782000) ' $K/table
echo reset > $K/set; grep -E '^(384000|1782000) ' $K/table
echo schedutil > $C3/scaling_governor
echo "ALIVE uptime: $(cut -d' ' -f1 /proc/uptime)"
