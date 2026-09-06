#!/bin/sh
# uv-top-ladder.sh: the low OPPs (< MIN_KHZ) go to default+LOW_DELTA (floor 850 mV),
# the high ones (>= MIN_KHZ) walk through the shifts in STEPS.
# Each step = DUR s of 4-core stress with an integrity check. Stop at the first FAIL, then reset.
K=/sys/kernel/krait_uv
modprobe krait-uv 2>/dev/null; [ -d $K ] || { echo "module missing"; exit 1; }
MIN=${MIN_KHZ:-1134000}; DUR=${DUR:-90}; LOW=${LOW_DELTA:--50000}
STEPS="${STEPS:--75000 -87500 -100000 -112500 -125000 -137500 -150000}"
apply() {  # apply <delta for the high OPPs>
	while read khz cur def; do
		if [ "$khz" -ge "$MIN" ]; then v=$((def + $1)); else v=$((def + LOW)); fi
		[ $v -lt 850000 ] && v=850000
		echo "$khz $v" > $K/set || echo "set $khz $v failed"
	done < $K/table
}
for d in $STEPS; do
	echo "===== low $LOW / top>=${MIN} $d  ($(date +%T))"
	apply $d
	grep -E '^(384000|1026000|1134000|1674000|1944000) ' $K/table | tr '\n' ';'; echo
	sh /tmp/stress.sh $DUR 4
	ok=$(cat /tmp/stress-res.* | grep -c 'err=0')
	[ "$ok" = "4" ] || { echo "STOP at $d"; break; }
done
echo reset > $K/set
echo "===== reset done, uptime $(cut -d' ' -f1 /proc/uptime)"
