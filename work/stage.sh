#!/bin/sh
set -e
echo ---MD5-DTB; md5sum /tmp/qcom-apq8064-samsung-jactivelte.dtb /boot/qcom-apq8064-samsung-jactivelte.dtb
echo ---STAGE-KERNEL; cp /tmp/vmlinuz /boot/vmlinuz.oc1944; sync; sync; md5sum /boot/vmlinuz.oc1944; ls -la /boot/ | grep vmlinuz
echo ---MODULE-INSTALL
D=/lib/modules/7.1.0-postmarketos-qcom-apq8064/kernel/drivers/cpufreq; mkdir -p $D; cp /tmp/krait-uv.ko.zst $D/; depmod -a; modprobe -n -v krait-uv | head -2
