#!/bin/bash
# Flash a kernel that has kraitcc built as a module.
# The whole module tree goes to the phone (ABI match, so WiFi keeps working), but
# krait-cc and qcom-cpufreq-nvmem are TAKEN OUT of it and loaded by hand from /root.
#
# PHONE     host or address of the phone
# PHONE_PW  password for sudo on the phone
# PKG_DIR   pmbootstrap package output directory
set -eu
PHONE=${PHONE:?set PHONE to the phone's ssh target}
PW=${PHONE_PW:?set PHONE_PW to the phone's sudo password}
PKG_DIR=${PKG_DIR:-$HOME/.local/var/pmbootstrap-jflte/packages/edge/armv7}
S=$(cd "$(dirname "$0")" && pwd)
A="$S/apk-mod"
KVER=7.1.0-postmarketos-qcom-apq8064

# 0. Unpack the newest package
PKG=$(ls -t "$PKG_DIR"/linux-postmarketos-qcom-apq8064-*.apk | head -1)
echo "[*] package: $(basename "$PKG")"
rm -rf "$A" && mkdir -p "$A"
tar -xzf "$PKG" -C "$A" 2>/dev/null || true
MODS="$A/usr/lib/modules/$KVER"

VM="$A/boot/vmlinuz"
DTB="$A/boot/dtbs/qcom-apq8064-samsung-jactivelte.dtb"
KC="$MODS/kernel/drivers/clk/qcom/krait-cc.ko.zst"
QC="$MODS/kernel/drivers/cpufreq/qcom-cpufreq-nvmem.ko.zst"
for f in "$VM" "$DTB" "$KC" "$QC"; do [ -f "$f" ] || { echo "MISSING: $f"; exit 1; }; done

# 1. The modules under test, unpacked separately
rm -rf "$S/mod-test" && mkdir -p "$S/mod-test"
zstd -dq -o "$S/mod-test/krait-cc.ko" "$KC"
zstd -dq -o "$S/mod-test/qcom-cpufreq-nvmem.ko" "$QC"

# 2. The module tree WITHOUT those two
rm -f "$KC" "$QC"
tar -czf "$S/modules.tar.gz" -C "$A/usr/lib/modules" "$KVER"
echo "[*] module tarball: $(du -h "$S/modules.tar.gz" | cut -f1)"

echo "[*] sending..."
scp -q "$VM" "$DTB" "$S/modules.tar.gz" "$S/mod-test/krait-cc.ko" \
       "$S/mod-test/qcom-cpufreq-nvmem.ko" $PHONE:/tmp/

ssh $PHONE "echo '$PW' | sudo -S sh -c '
set -e
cd /tmp
cp vmlinuz /boot/vmlinuz
cp qcom-apq8064-samsung-jactivelte.dtb /boot/qcom-apq8064-samsung-jactivelte.dtb
rm -rf /usr/lib/modules/$KVER
tar -xzf /tmp/modules.tar.gz -C /usr/lib/modules/
depmod -a $KVER
mkdir -p /root/mod-test
cp /tmp/krait-cc.ko /tmp/qcom-cpufreq-nvmem.ko /root/mod-test/
sync
echo \"--- check: would udev find krait-cc? ---\"
modprobe -n krait-cc 2>&1 || echo \"  (it will not - good)\"
echo \"--- modules under test ---\"
ls -la /root/mod-test/
echo \"--- md5 of /boot ---\"
md5sum /boot/vmlinuz /boot/qcom-apq8064-samsung-jactivelte.dtb
'" 2>&1 | grep -vE '^\[sudo|^Password'

echo "[*] md5 locally:"; md5sum "$VM" "$DTB"
