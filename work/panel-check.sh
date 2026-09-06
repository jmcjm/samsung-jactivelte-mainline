#!/bin/sh
# Run on the phone as root after boot: everything the panel bring-up needs.
R=/sys/kernel/debug/regmap/500000.ssbi:pmic/registers
echo "---UPTIME"; uptime
echo "---MD5"; md5sum /boot/vmlinuz /boot/qcom-apq8064-samsung-jactivelte.dtb
echo "---DMESG-PANEL"
dmesg | grep -E "renesas|msm_dsi|dsi_cmds|dsi_link|mdp4|drm\]|fb0" | grep -v "dependency cycle"
echo "---MPP2 (0x25 high / 0x24 low)"; grep -E "^051: " $R
echo "---GPIO"; grep -E "^ gpio(20|33) " /sys/kernel/debug/gpio
grep -E "^ gpio31 " /sys/kernel/debug/gpio
echo "---REGULATORS"
for r in /sys/class/regulator/regulator.*; do
	n=$(cat $r/name)
	case $n in l15|l16|lvs1) echo "$n $(cat $r/state) $(cat $r/microvolts 2>/dev/null)";; esac
done
echo "---CLK"
grep -E "dsi0vco|dsi1pll |dsi1_pixel_src|dsi1_byte_clk|dsi1_esc_clk|mdp_pclk1" /sys/kernel/debug/clk/clk_summary
echo "---DRM"
cat /sys/class/drm/card0-DSI-1/status /sys/class/drm/card0-DSI-1/dpms /sys/class/drm/card0-DSI-1/enabled
grep -E "active=|mode:" /sys/kernel/debug/dri/0/state | head -3
echo "---PARAMS"
for p in /sys/module/panel_samsung_renesas_tft/parameters/*; do echo "$(basename $p)=$(cat $p)"; done
echo "---BL"; cat /sys/class/backlight/*/brightness /sys/class/backlight/*/bl_power 2>/dev/null
