#!/bin/sh
# throttle-compare.sh "<setting>" ... - for each setting: cool down to <=COOL (4 min max),
# then DUR s of stress on 4 cores. Metrics: iterations (work done), average frequency from the
# time_in_state delta, tmax. A setting is 'reset' | 'all <delta>' | 'split <low delta> <high delta>'
# (split: OPPs < MIN_KHZ get default+low delta, >= MIN_KHZ get default+high delta, floor 850 mV).
K=/sys/kernel/krait_uv; C0=/sys/devices/system/cpu/cpu0/cpufreq
DUR=${DUR:-120}; COOL=${COOL:-55000}; MIN=${MIN_KHZ:-1134000}
modprobe krait-uv 2>/dev/null
apply() {
	case "$1" in
	split*) set -- $1; low=$2; top=$3
		while read khz cur def; do
			if [ "$khz" -ge "$MIN" ]; then v=$((def + top)); else v=$((def + low)); fi
			[ $v -lt 850000 ] && v=850000
			echo "$khz $v" > $K/set
		done < $K/table ;;
	*) echo "$1" > $K/set ;;
	esac
}
for setting in "$@"; do
	apply "$setting"
	n=0; while [ $n -lt 120 ]; do
		t=0; for z in /sys/class/thermal/thermal_zone[1-3]/temp; do v=$(cat $z); [ $v -gt $t ] && t=$v; done
		[ $t -le $COOL ] && break; n=$((n+1)); sleep 2
	done
	echo "===== '$setting' start_temp=$t cooled_${n}x2s ($(date +%T))"
	grep -E '^(384000|1242000|1944000) ' $K/table | tr '\n' ';'; echo
	cp $C0/stats/time_in_state /tmp/tis0
	sh /tmp/stress.sh $DUR 4 | grep -E 'tmax|RESULT'
	paste /tmp/tis0 $C0/stats/time_in_state | awk '{d=$4-$2; s+=d; w+=$1*d} END {printf "avg_freq_cpu0=%.0f kHz (ticks %d)\n", w/s, s}'
	echo "throughput=$(cat /tmp/stress-res.* | sed 's/.*it=\([0-9]*\).*/\1/' | paste -sd+ | bc) iterations (4 cores)"
done
echo reset > $K/set
