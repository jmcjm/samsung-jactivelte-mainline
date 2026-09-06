#!/bin/sh
# UV ladder: successive shifts of every OPP, each step = a 4-core stress run.
# Stops at the first FAIL. Resets to the device tree values at the end.
K=/sys/kernel/krait_uv
modprobe krait-uv 2>/dev/null; [ -d $K ] || { echo "module missing"; exit 1; }
STEPS="${STEPS:--25000 -37500 -50000 -62500 -75000}"
DUR=${DUR:-90}
for d in $STEPS; do
	echo "===== all $d  ($(date +%T))"
	echo "all $d" > $K/set || { echo "set failed"; break; }
	grep -E '^(384000|1242000|1944000) ' $K/table | tr '\n' ';'; echo
	sh /tmp/stress.sh $DUR 4
	R=$(sh -c 'tail -1 /tmp/stress-last 2>/dev/null'); 
	sh -c 'true'
	LAST=$(cat /tmp/stress-res.* | grep -c 'err=0')
	[ "$LAST" = "4" ] || { echo "STOP at $d"; break; }
	tail -4 /sys/devices/system/cpu/cpu0/cpufreq/stats/time_in_state | tr '\n' ' '; echo
done
echo reset > $K/set
echo "===== reset done, uptime $(cut -d' ' -f1 /proc/uptime)"
