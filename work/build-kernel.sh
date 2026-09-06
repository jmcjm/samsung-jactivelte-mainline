#!/bin/sh
# Rebuild the kernel package from the local source tree.
# PMB_CFG   pmbootstrap config for this port (see README)
# KERNEL_SRC  checkout of apq8064-mainline/linux with the patches applied
PMB_CFG=${PMB_CFG:-$HOME/.config/pmbootstrap_v3_jflte.cfg}
KERNEL_SRC=${KERNEL_SRC:-$PWD/linux}
PMB="pmbootstrap -c $PMB_CFG"
$PMB checksum linux-postmarketos-qcom-apq8064 || exit 1
$PMB build --src "$KERNEL_SRC" linux-postmarketos-qcom-apq8064
