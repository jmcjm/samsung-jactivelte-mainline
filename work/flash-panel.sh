#!/bin/sh
# Run on the phone as root: stage a new vmlinuz + dtb from /tmp, keep the old pair.
set -e
BK=$1
[ -n "$BK" ] || { echo "usage: flash-panel.sh <backup-suffix>"; exit 1; }
[ -e /boot/vmlinuz.$BK ] || cp /boot/vmlinuz /boot/vmlinuz.$BK
[ -e /boot/dtb.$BK ] || cp /boot/qcom-apq8064-samsung-jactivelte.dtb /boot/dtb.$BK
cp /tmp/vmlinuz /boot/vmlinuz
cp /tmp/qcom-apq8064-samsung-jactivelte.dtb /boot/qcom-apq8064-samsung-jactivelte.dtb
sync; sync
md5sum /boot/vmlinuz /boot/qcom-apq8064-samsung-jactivelte.dtb /tmp/vmlinuz /tmp/qcom-apq8064-samsung-jactivelte.dtb
ls -la /boot/ | grep -E "vmlinuz|dtb"
