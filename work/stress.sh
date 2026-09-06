#!/bin/sh
# stress.sh <seconds> [cores=4] - load with an integrity check (sha256 + gzip round trip).
# Output: a RESULT ok|FAIL line, iterations per core, max temperature, new dmesg lines.
D=${1:-60}; N=${2:-4}
B=/tmp/stress-blob
[ -s $B ] || head -c 6000000 /dev/urandom > $B
REF=$(sha256sum $B | cut -d' ' -f1)
rm -f /tmp/stress-res.*
DM0=$(dmesg | wc -l)
end=$(($(date +%s)+D))
c=0
while [ $c -lt $N ]; do
	taskset -c $c sh -c "
		it=0; err=0
		while [ \$(date +%s) -lt $end ]; do
			h=\$(sha256sum $B | cut -d' ' -f1); [ \"\$h\" = \"$REF\" ] || err=\$((err+1))
			gzip -1 -c $B | gzip -dc | cmp -s - $B || err=\$((err+1))
			it=\$((it+1))
		done
		echo \"cpu$c it=\$it err=\$err\" > /tmp/stress-res.$c" &
	c=$((c+1))
done
tmax=0
while [ $(date +%s) -lt $end ]; do
	for z in /sys/class/thermal/thermal_zone[1-3]/temp; do t=$(cat $z); [ $t -gt $tmax ] && tmax=$t; done
	sleep 2
done
wait
cat /tmp/stress-res.*
ERR=$(cat /tmp/stress-res.* | sed 's/.*err=//' | paste -sd+ | bc 2>/dev/null || cat /tmp/stress-res.* | sed 's/.*err=//' | tr '\n' ' ')
NEW=$(dmesg | tail -n +$((DM0+1)) | grep -viE 'sudo|audit' | head -5)
echo "tmax=$tmax dmesg_new=$(dmesg | wc -l | xargs -I{} expr {} - $DM0)"
[ -n "$NEW" ] && echo "$NEW"
case "$ERR" in 0|"") echo "RESULT ok";; *) echo "RESULT FAIL err=$ERR";; esac
