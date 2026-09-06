#!/bin/sh
cat /proc/uptime; md5sum /boot/vmlinuz /boot/qcom-apq8064-samsung-jactivelte.dtb
C0=/sys/devices/system/cpu/cpu0/cpufreq
echo "boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>&1) cpuinfo_max=$(cat $C0/cpuinfo_max_freq) scaling_max=$(cat $C0/scaling_max_freq)"
cat $C0/scaling_available_frequencies; cat $C0/scaling_boost_frequencies 2>/dev/null
grep -E 'hfpll[0-3] |hfpll_l2 ' /sys/kernel/debug/clk/clk_summary | awk '{print $1, $5}'
for r in 1 2 3 4; do echo "spm$((r-1)) max=$(cat /sys/class/regulator/regulator.$r/max_microvolts)"; done
modprobe krait-uv; tail -4 /sys/kernel/krait_uv/table
echo "display: bind=$(cat /sys/class/vtconsole/vtcon1/bind) dpms=$(cat /sys/class/drm/card0-DSI-1/dpms)"
