# postmarketOS on the Samsung Galaxy S4 Active (GT-I9295, jactivelte)

Rebuild instructions live in `README.md`. This file is the work log: what was
tried, what failed and why the port ended up the way it did. The patches and the
device package are next to it, in `patches/` and `pmaports/`.

Status as of 2026-09-05. Goal: run postmarketOS on the S4 Active, reusing the
existing port for the regular S4 (GT-I9505, `samsung-jflte`).

## The conclusion in one sentence

The mainline 7.1 kernel from the jflte port **boots on the S4 Active** and drives
almost all of the hardware; the only hard blocker is the display panel (a Renesas
TFT instead of an AMOLED), which needs its own driver and device tree.

## Hardware

| | GT-I9505 (jflte) | GT-I9295 (jactivelte) |
|---|---|---|
| SoC | APQ8064AB Snapdragon 600 | identical |
| Panel | Samsung Magna OCTA AMOLED | **Renesas TFT** + `LCD_EN2` |
| Touchscreen | Synaptics RMI4 | identical |
| WiFi/BT | BCM4335 | identical |
| MUIC / fuel gauge | max77693 / max17048 | identical |

Source of the differences: the diff between `jf_eur_defconfig` and
`jactive_eur_defconfig` in the LineageOS kernel (`android_kernel_samsung_jf`,
branch `lineage-16.0`). The whole diff is 16 lines; two of them matter:

```
-CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_VIDEO_FULL_HD_PT_PANEL=y
+CONFIG_FB_MSM_MIPI_RENESAS_TFT_VIDEO_FULL_HD_PT_PANEL=y
+CONFIG_FB_MSM_ENABLE_LCD_EN2=y
```

## Starting state of the phone

- ROM: `omni_jactivelte`, Android 6.0.1
- Recovery: TWRP 3.3.1-0 (the `recovery` partition, **left untouched**)
- eMMC: 14.7 GiB (`016GE4`), 29 partitions, MSDOS/PIT table
- Bootloader unlocked, Download Mode works

## Backup

26 partitions, 15 GB, **every one verified by MD5 against the phone**
(`manifest.txt`). Deliberately skipped: `cache`, `hidden`, `fota`.

Restoring a single partition from TWRP:

```
adb push efs.img /tmp/efs.img
adb shell dd if=/tmp/efs.img of=/dev/block/mmcblk0p10 bs=1M
```

Restoring the pre-experiment state (Download Mode, needs heimdall):

```
heimdall flash --BOOT <backup dir>/boot.img
```

Notes:
- `modemst1`, `modemst2` and `fsg` are **all zeroes** (MD5 `d1dd210d…`). They were
  like that before anything was touched. The IMEI lives in `efs.img`.
- The `boot` partition now holds **lk2nd**, not the original Android kernel.

## What was established about the port

### pmaports

`gitlab.com/postmarketOS/pmaports` is a **frozen mirror** from before the
migration — it shows the old, downstream port. The current source is
`gitlab.postmarketos.org/postmarketOS/pmaports`, branch **`main`** (formerly
`master`).

Current state of the jflte port (`device/testing/device-samsung-jflte`, pkgver 6):

```
depends: linux-postmarketos-qcom-apq8064 (7.1), firmware-samsung-jflte, mesa-dri-gallium
deviceinfo_dtb="qcom-apq8064-samsung-jflte"
deviceinfo_append_dtb="true"
deviceinfo_flash_method="fastboot"      # through lk2nd
deviceinfo_flash_offset_base="0x80200000"    kernel="0x00008000"
deviceinfo_flash_offset_ramdisk="0x03200000" tags="0x02e00000"
deviceinfo_flash_pagesize="2048"
```

Kernel: `github.com/apq8064-mainline/linux`, branch `qcom-apq8064-v7.1` (alive,
last push 2026-08-31). The tree carries exactly **one** Samsung DTS for this SoC:
`arch/arm/boot/dts/qcom/qcom-apq8064-samsung-jflte.dts`. For `jactivelte` there is
nothing — no DTS, no device package, not a single issue or MR in pmaports.

### lk2nd

The image from `raw.githubusercontent.com/apq8064-mainline/linux/master/lk2nd.img`
(244 KB) knows these devices: `samsung,espresso10att`, `expressatt`,
`expressltexx`, **`jflte`**, `loganrelte`, `serranolte` — matched by model
(`GT-I9505`, `I9195`, `GT-I8730`). **`GT-I9295` is not on the list.**

It **still boots on the Active** and exposes fastboot. Consequences of the missing
match:
- the volume keys do not work in lk2nd (no key GPIO configuration),
- fastboot answers only the **first** request of a session and then hangs — the
  phone has to be restarted between operations.

## Result of the hardware test

Method: `fastboot boot` of an image with the mainline 7.1 kernel and the
`qcom-apq8064-samsung-jflte` DTB, with the `dsi@4700000` and
`display-controller@5100000` nodes set to `status = "disabled"`. Nothing was
written to any partition.

Without that modification the kernel dies before the initramfs (black screen, no
USB enumeration). With it, it comes up and brings up a USB gadget CDC NCM.

### Works

| Subsystem | Evidence from `dmesg` |
|---|---|
| Mainline 7.1 kernel | `7.1.0-postmarketos-qcom-apq8064`, `bcdDevice=7.01` |
| eMMC + 29 partitions | `mmcblk0: mmc0:0001 016GE4 14.7 GiB`, `p1…p29` |
| USB gadget (networking) | `cdc_ncm … CDC NCM`, ping 172.16.42.1 = 0% loss, telnet:23 |
| Synaptics touchscreen | `rmi4_f01: found RMI device, Synaptics, product: SY 04` |
| Battery / charger | `power_supply/battery`, `capacity = 60` |
| Keys | `pmic8xxx_pwrkey` → input0, `gpio-keys` → input3 |
| MUIC | `max77693-muic/dock` → input2 |
| CPU + thermals | `nproc = 4`, thermal 48/48/45 °C |

### Broken / to do

1. **The panel** — the only hard blocker.
2. `qcom-cpufreq-nvmem … failed with error -2` — no speed bin in the DTS, no
   frequency scaling.
3. `max77693-charger/haptic/led: Failed to locate of_node` — the jflte DTS
   describes only part of the MUIC's functions.
4. `thermal_zone0: Temperature check failed (-61)`.

The `/dev/fb0 did not appear` and `failed to mount subpartitions` errors are
**expected** — they follow from the disabled display and from having no rootfs on
the phone.

## Data for writing the panel driver

From `drivers/video/msm/mipi_renesas_tft_video_full_hd_pt.c` (LineageOS jf
kernel), variant `CONFIG_MACH_JACTIVE_EUR`:

### Timings

```
1080 x 1920, physically 62 x 111 mm, bpp 24, RGB888
h_back_porch = 100   h_front_porch = 97   h_pulse_width = 14
v_back_porch =   6   v_front_porch =  8   v_pulse_width =  2
clk_rate = 906 000 000        (alternative variant: 898 000 000)
4 data lanes, t_clk_post = 0x04, t_clk_pre = 0x1c, force_clk_lane_hs = 1
```

### Power-on sequence (DCS)

```
0x51 0xB6    display brightness
0x55 0x00    CABC off
0x53 0x2C    backlight control (BCTRL | DD | BL)
0x35 0x01    tearing effect on
0x11         sleep out            -> wait 120 ms
0x29         display on
```

Power-off: `0x28` (display off, 40 ms), then `0x10` (sleep in).

The `JACTIVE_*` variants **skip** the gamma commands (`0xF0`/`0xFA`/`0xFB`) —
those sit behind an `#if` for other models. Brightness goes through DCS `0x51`,
not through a separate PWM driver.

### Power and GPIOs (`arch/arm/mach-msm/board-8064-display.c`)

```
LCD_22V_EN      = TLMM gpio 33
LCD_22V_EN_2    = TLMM gpio 20     (only with CONFIG_FB_MSM_ENABLE_LCD_EN2, rev >= 16)
LED_DRIVER      = PMIC gpio 31
MLCD_RST        = PMIC gpio 43     (for JACTIVE_EUR it is NOT touched on power-on)
```

Power-on order:

```
1. regulator LVS1 (IOVDD) on
2. regulator L15 on
3. gpio 33 -> 1
4. [rev >= 16]  wait 10 ms, gpio 20 -> 1
5. [rev >= 15]  regulator L16 on, wait 10 ms
6. LED_DRIVER -> 1
```

Power-off in reverse order, with `usleep(2000)` between VDD off and AVDD off.

## Desktop environment

- pmbootstrap 3.11.1 in `~/.local/share/pmbootstrap` (git clone + venv; every
  release on PyPI is yanked, so installing from the repo is the only option)
- config for this port: `~/.config/pmbootstrap_v3_jflte.cfg` (device
  `samsung-jflte`, UI console, OpenRC, channel edge)
- a separate work dir, `~/.local/var/pmbootstrap-jflte` — **deliberately**, so
  that the chroots of an existing setup for `xiaomi-olive` do not get wiped
- heimdall 2.2.2 lives in the chroot; it needs
  `mount --bind /dev/bus/usb <chroot>/dev/bus/usb`, otherwise libusb cannot open
  the device
- heimdall on this bootloader **cannot stand** `--no-reboot`/`--resume` — the
  second command of a session ends with `Failed to begin session!`. One command
  per entry into Download Mode.

## Next steps

1. A mainline panel driver based on the data above (template:
   `panel-samsung-magna-octa.c` from the same tree).
2. `qcom-apq8064-samsung-jactivelte.dts` — a copy of jflte with the panel swapped,
   `LCD_EN2` added and the LVS1/L15/L16 regulators wired up.
3. Kernel build through `pmbootstrap build --src`.
4. Testing through `fastboot boot` (the phone can be rebooted from software in the
   initramfs debug shell, so the build → test loop needs no hands on the hardware).
5. On success: `device-samsung-jactivelte` in pmaports and an upstream MR.

---

## Port progress — 2026-09-06

Written and tested on the hardware:

- `drivers/gpu/drm/panel/panel-samsung-renesas-tft.c` — the panel driver
  (compatible `samsung,renesas-tft-fhd`)
- `arch/arm/boot/dts/qcom/qcom-apq8064-samsung-jactivelte.dts` — the device tree
- entries in `drivers/gpu/drm/panel/{Kconfig,Makefile}` and
  `arch/arm/boot/dts/qcom/Makefile`

Sources: the `linux/` checkout next to this repository (a clone of
`apq8064-mainline/linux`, branch `qcom-apq8064-v7.1`).

### State: the panel initialises

```
panel-samsung-renesas-tft 4700000.dsi.0: probe start
mdp4 5100000.display-controller: bound 4700000.dsi
mdp4 5100000.display-controller: bound 4300000.gpu
panel-samsung-renesas-tft 4700000.dsi.0: probe done

/dev/fb0                          1080,1920, 32 bpp
/sys/class/drm/card0-DSI-1        connected, 1080x1920
/sys/class/backlight/4700000.dsi.0
```

Writing 8 MB to `/dev/fb0` goes through at 17 MB/s with no DSI errors in `dmesg`.

### Three traps, one iteration each

**1. The kernel's own `.gitignore` eats a source file.** `pmbootstrap build --src`
rsyncs with `--exclude-from=<src>/.gitignore`, and the kernel ignores the `*.bc`
pattern — which drops the tracked `kernel/time/timeconst.bc` and ends the build
with `No rule to make target`. Workaround: replace `.gitignore` with a single
`.git/` entry (the original saved as `.gitignore.orig`).

**2. pinctrl in the panel node blocks the probe silently.** Declaring `pinctrl-0`
on pins that are also used through `enable-gpios` ends with:

```
apq8064-pinctrl 800000.pinctrl: error -EINVAL: pin-33 (800000.pinctrl:545)
```

`pinctrl_bind_pins()` runs **before** probe, so the driver never starts and leaves
no log at all — the only trace is an entry in
`/sys/kernel/debug/devices_deferred`. Fix: do not declare pinctrl; `gpiod` puts
the pin into GPIO mode by itself.

**3. The pixel clock is not arbitrary.** `clk_rcg_pixel_set_rate()` in
`drivers/clk/qcom/clk-rcg.c` only accepts rates tied to the DSI PLL by one of the
fractions `{1/1, 1/2, 1/3, 3/16}`, with a 100 kHz tolerance. The 60 Hz clock
computed from the downstream porches (149.96 MHz) misses and yields:

```
dsi_link_clk_set_rate_v2: Failed to set rate pixel clk, -22
```

Setting `.clock = 149666` (that is, 449/3 MHz) solves it — the PLL settles on a
894 MHz VCO, `dsi1_pixel_src` on 149 MHz, the errors disappear. Worth remembering:
**msm never reads `dsi->hs_rate`** — the bit clock follows from `mode->clock`
alone.

### Work cycle

The phone reboots remotely (`reboot -f` in the debug shell returns to lk2nd), so
the build → `fastboot boot` → telnet loop runs without touching the hardware. A
full iteration with ccache (`--lax`) takes about 2 minutes.

---

## Final state — 2026-09-06, headless server

The goal changed along the way: the display was dropped and the phone runs as a
server.

### Works

| Item | Details |
|---|---|
| System | postmarketOS edge, OpenRC, kernel 7.1 on eMMC, hostname `s4active` |
| Access | SSH, key authentication |
| Resources | 71 MB of 2 GB RAM used, 10.4 GB free of 11.1 GB |
| WiFi | BCM4335, firmware and calibration from the jflte package (identical `boardtype`) |
| Network | WiFi on the LAN with a static DHCP lease |
| LED | `an30259a`, breathing `#33ccff` (R2/G8/B10), 1.5/1.5/1 s cycle |
| **Charging** | **limit 3.85 V ≈ 50%**, register `CHG_CNFG_04` = `0x08` |

### Does not work

**The panel** — the one part that would not give in. The
`panel-samsung-renesas-tft` driver probes correctly, `/dev/fb0` appears, the
`card0-DSI-1` connector reports `connected`, writes to the framebuffer go through
at 24 MB/s, the backlight **works** (after fixing the PMIC GPIO offset), but the
screen stays black.

Cause established: in lk2nd the display works because **Samsung's own bootloader
(SBL)** lights the panel before lk2nd starts, and lk2nd only paints into a
framebuffer that is already alive. The kernel takes the hardware over and has to
initialise the pipeline from scratch. The downstream kernel for `JACTIVE_EUR`
deliberately never touches the panel reset and sends only a handful of DCS
commands — the full sequence sits in the bootloader, for which there are no
sources.

Two ways forward: dump the sequence out of the bootloader and reproduce it in the
driver, or take the "continuous splash" route — do not touch the panel at all and
inherit the state left by the SBL.

### Charging limit

The DTS node (`arch/arm/boot/dts/qcom/qcom-apq8064-samsung-jactivelte.dts`):

```dts
charger {
	compatible = "maxim,max77693-charger";
	maxim,constant-microvolt = <3850000>;
};
```

The value was picked for **cell life, not runtime**: on a device that is
permanently on the charger, degradation is driven by calendar ageing at a high
state of charge, not by cycle count. 3.85 V parks the cell around 50%, that is, at
the minimum of that curve. The cost: backup runtime drops from about 26 h (the
factory 4.35 V) to about 15 h.

Requires `CONFIG_CHARGER_MAX77693=y` — in the base apq8064 config the driver is
disabled, and the i9505 DTS never described the charger, so this driver had never
run on this phone. Verified by reading the register:

```
/sys/kernel/debug/regmap/0-0066/registers → bb: c8
0xc8 & 0x1f = 0x08 = 8 → 8 * 25 mV + 3650 mV = 3850 mV
```

### Boot traps

**lk2nd reads `extlinux`, not `boot.img`.** Updating the kernel means copying
`vmlinuz` and the DTB to `/boot` and rebooting — replacing `/boot/boot.img`
achieves nothing.

**The system runs from an image through a loop device.** `flash_rootfs` writes the
whole image, with its own partition table, to `mmcblk0p29`, so `/dev/loop0p1` is
`/boot` and `/dev/loop0p2` is `/`.

**Writes to `/boot` can be delayed.** A cmdline marker test gave a false negative
after a soft reboot and only showed up after a hard reset. Run `sync` twice and
verify `md5sum /boot/vmlinuz` against the package.

**`reboot` can hang the device — use `poweroff`.** This is a known platform bug,
documented on the jflte port wiki: after a `reboot` the phone sometimes does not
come back and sits with a black screen, and only holding the power button saves
it. Shutting down with `poweroff` never does this. The symptom is easy to mistake
for a bug of one's own — in this session it was blamed first on the charger
driver, then on the panel, before the reboot command itself turned out to be the
culprit.

**Tests through `fastboot boot` are trustworthy** (the image lands in RAM,
bypassing lk2nd), but once the system is installed on eMMC only `/boot/vmlinuz`
counts. Confusing the two cost an hour of panel work on a stale kernel.

## CPU frequency scaling — 2026-09-06

Running. `cpufreq-dt` through `qcom-cpufreq-nvmem`, governor `schedutil`, range
384–1890 MHz across 15 points, all four cores scale (confirmed by the distribution
in `stats/time_in_state`). Built-in configuration: `CONFIG_KRAITCC=y`,
`CONFIG_ARM_QCOM_CPUFREQ_NVMEM=y`, `CONFIG_QCOM_SPM=y`.

Two causes of the earlier bootloops, long confused with each other:

1. **A pointless patch to `gcc-msm8960.c`.** I added `[PLL11] = &hfpll2.clkr` and
   `[PLL13] = &hfpll3.clkr`, believing mainline omitted the HFPLLs of cores 2 and
   3. In fact it registers them further down as `PLL16` and `PLL17`. Registering
   the same `clk_hw` twice corrupts the clock list, and `clk_hfpll_init`
   reconfigures the PLL that clocks a running core. The patch was reverted; the
   DTS uses `<&gcc PLL16>` and `<&gcc PLL17>`.

2. **Disabled DSI.** `status = "disabled"` on `&dsi0` hangs the boot, because
   `mdp` stays enabled and has `assigned-clock-parents` pointing at the PLL in the
   DSI PHY. DSI has to stay `okay` even though the panel shows nothing anyway.

`CONFIG_QCOM_SPM=y` is required — `spm.c` is the only provider of the
`saw0_vreg`…`saw3_vreg` regulators that `cpu-supply` needs. Without it the cpufreq
module loads with RC=0, but the probe ends in a silent `EPROBE_DEFER` and the
`/sys/devices/system/cpu/cpu0/cpufreq` directory never appears at all.

### Temperature in htop

The core sensors work (`cpu0-thermal`…`cpu3-thermal`, hwmon3–6). `htop` shows the
temperatures after `apk add lm-sensors`. The package metadata is misleading: htop
3.x loads libsensors through `dlopen`, so it shows up neither in `apk info -R htop`
nor in `ldd` — and is used regardless. Only the `battery` zone stays broken.

### To do

- Real OC: the 1890 and 1944 MHz OPPs are in the table, but without the HFPLL
  patch they do nothing — see the "OC, L2 and voltages" section.
- Fix the battery temperature sensor (`thermal_zone0`, error -61).

### Thermal throttling and sensors — 2026-09-06

`qcom-apq8064.dtsi` has thermal trip points but no `cooling-maps`, so the 75 °C
trip did nothing and the only working protection was the critical shutdown at
110 °C. Added: `#cooling-cells = <2>` in the CPU nodes and `cooling-maps` in every
zone, with **each zone limiting all four cores**.

The cpu0 sensor (`tsens 7`) is faulty on this unit — it sticks at 69–70 °C and
stops reacting to load. The `cpu0-thermal` zone was disabled; core 0 is protected
by the remaining zones. As a side effect this fixes htop, which used to replicate
that dead reading across all cores.

Measurement after the changes (all four cores loaded): the frequency oscillates
between 1782 and 1242 MHz in step with cooling (cpufreq reported 1944 MHz, but the
clock was actually at 1782 MHz — see the OC section below), the temperature stays
between 69 and 81 °C, and the sensors keep updating after the test ends.

### Touchscreen versus frequency — 2026-09-06

Touching the screen pushed the clock up: the `rmi4_i2c` driver generated thousands
of interrupts on core 0, which `schedutil` read as load. Since the panel shows
nothing anyway, the driver is unbound at boot by
`/etc/local.d/touchscreen-off.start` (an unbind on the i2c bus, no DTS changes).

### OC, L2 and voltages — 2026-09-06, part two

Speed bin and PVS from qfprom (offset `0xc0`, word `0x00010902`): speed 2,
**PVS 2**. The kernel uses the `opp-microvolt-speed2-pvs2-v0` column; this is
visible directly in `/sys/kernel/debug/opp/cpu0/opp:*/supply-0/u_volt_target`
(1782 MHz → 1162.5 mV, 384 MHz → 900 mV). The earlier "PVS 1" assumption was
wrong.

L2 was raised from 384 MHz to 1188 MHz through `assigned-clocks = <&kraitcc 4>` in
the `kraitcc` node, confirmed in `clk_summary` (`hfpll_l2` 1188000000). That is the
only part of the "OC" that genuinely worked from the start.

**The 1890 and 1944 MHz OPPs did not work.** `dev_pm_opp_set_rate()` asks
`clk_round_rate()` first, and `clk_hfpll_determine_rate()` clamps the request to
the driver's `max_rate`. Mainline's `gcc-msm8960.c` has
`max_rate = 1800000000` for every HFPLL, so 1944 and 1890 MHz both round down to
1782 MHz (66 × 27 MHz). cpufreq knows nothing about it: `scaling_cur_freq` — and
htop behind it — reports 1944, while `cpuinfo_cur_freq` (read from the clock) and
`clk_summary` show 1782. The voltage is chosen for the OPP found after rounding,
so it was correct for 1782 MHz. The phone was running below the factory 8064AB
maximum (1890 MHz), and every earlier measurement "at 1944 MHz" was really at
1782 MHz.

Fix: `patches/04-hfpll-max-rate-1944.patch` raises `max_rate` to 1944 MHz in
`hfpll0_data`, `hfpll1_8064_data`, `hfpll2_data` and `hfpll3_data` (the L2 PLL
stays at 1800 MHz). After every kernel change, check through `cpuinfo_cur_freq`,
never through `scaling_cur_freq`.

### Live undervolting — 2026-09-06

The question was whether voltages can be lowered without reboots. Not from
userspace: `/sys/kernel/debug/opp/*/supply-0/u_volt_*` is mode 0444, and
`microvolts` in the regulator sysfs is read-only. The kernel does export
`dev_pm_opp_adjust_voltage()`, though, so the `drivers/cpufreq/krait-uv.c` module
was written (`CONFIG_KRAIT_UV=m`, copy in `patches/krait-uv.c`) with a
`/sys/kernel/krait_uv/{table,set}` interface: a single point, a shift of all
points, or a reset to the device tree values. Range 850–1300 mV in 12.5 mV steps.
A voltage takes effect at the next OPP change of the given core; `rmmod` restores
the DTS values.

Test on a running kernel (module inserted with `insmod`, no reboot): 1782 MHz at
1137.5 mV instead of 1162.5 mV, 10 s of SHA-256 on core 3, 52 °C, the system
survives; `reset` restores 1162.5 mV. The module is installed in
`/lib/modules/…/kernel/drivers/cpufreq/` and `modprobe krait-uv` works; it is not
in `/etc/modules` — on-demand loading is an advantage here, because after a hang
the phone comes back on the DTS voltages.

The kernel with the HFPLL patch and the module (md5
`65d0316214c1153416877038b4451e7a`, kept as `/boot/vmlinuz.oc1944`) was activated
on 2026-09-06 at 13:05; the DTB was unchanged. After the reboot **1944 MHz is
real**: `clk_summary` shows 1944000000 on `hfpll0`–`hfpll3`, `cpuinfo_cur_freq`
shows 1944000 on all four cores at 1237.5 mV (the PVS 2 column). Under 20 s of
full four-core load the throttling cycles 1944 → 1674 → 1890 → 1944 MHz at
63–79 °C, and the system stays stable. The previous kernel is kept as
`/boot/vmlinuz.ok` (1782 MHz for real) and `/boot/vmlinuz.working` (no cpufreq).

### UV ladder and throttling measurement — 2026-09-06, afternoon

Decision: what matters is UV (less throttling), not more OC. Method: the
`krait-uv` module, and at every step 90 s of four-core load with an integrity check
(`sha256sum` of a random blob against a reference plus a `gzip` round trip and
`cmp`), scripts `work/stress.sh`, `work/uv-ladder.sh`, `work/uv-top-ladder.sh`. The
case was heat-soaked throughout, 85–91 °C under load — for UV stability that is
the harsher condition, so the passes are trustworthy, but performance comparisons
between steps are not.

Results. Shifting all points: −25, −37.5, −50, −62.5, −75 mV all clean (from −50
onwards the points at or below 702 MHz sit on the regulator's 850 mV floor and
cannot go lower). Splitting into "everything below 1134 MHz at −50, the top at a
delta": −75 and −87.5 clean over 90 s, **−100 mV = a hang after a minute and a
hardware reset** (pstore empty, so a watchdog). On request, −87.5 was tried as the
working setting: **a hang during the second pass at 91 °C**, without a reset,
requiring the power button. A single 90 s pass in the ladder is not enough to call
a step stable.

ABBA throttling measurement (reset, UV, UV, reset), 90 s each, cooling to 55 °C
before each pass, profile −50 mV low / −75 mV high:

| pass | average cpu0 clock | work (iterations) | max °C |
|---|---|---|---|
| DTS | 1467 MHz | 212 | 88 |
| UV −75 | 1498 MHz | 217 | 86 |
| UV −75 | 1473 MHz | 214 | 88 |
| DTS | 1306 MHz | 197 | 90 |

On average +7 % clock and +5 % work done under full, sustained load; the thermal
drift of the case (DTS 1467 → 1306 MHz between the first and the last pass) is
larger than the UV effect. The bottleneck is the sealed IP67 case, not the
voltage. The −50/−75 mV profile was installed permanently as
`/etc/local.d/krait-uv.start` (copy in `work/`; the script loads the module
itself, so `/etc/modules` is not needed). The margin to a hang is 12.5–25 mV;
after a hang the server will apply the UV again on the next boot, so disabling it
means deleting that file or running `echo reset > /sys/kernel/krait_uv/set` live.

A kernel with 1998/2052/2106 MHz points (turbo mode, boost) was built, shelved and
then **deleted** along with the source changes: OC became secondary, because the
sealed case chokes the phone long before the voltage limit. The tree, `patches/`
and the phone all describe only the state that is actually flashed: HFPLL up to
1944 MHz, 16 OPPs, L2 at 1188 MHz.

### The screen after boot — 2026-09-06

The backlight stays on for the first 300 s after boot (`consoleblank=300`, fbcon
attached to `/dev/fb0`), and every key press lights it for another 5 minutes,
because the VT layer unblanks the console on keyboard events. The panel driver
only turns the backlight off (a PMIC GPIO) on DPMS off, so `bl_power` on its own
does nothing. `/etc/local.d/display-off.start` detaches fbcon and writes `4` to
`fb0/blank`: the panel and the backlight go off for good, while `dsi0` stays
enabled (disabling it hangs the boot, see the bootloop section). Safer than
`drm_kms_helper.fbdev_emulation=0` on the cmdline, because it does not touch
`extlinux.conf`.

Once turned off, the panel **cannot be lit again** from the kernel: another
`prepare` goes through with no DCS errors, the backlight GPIO goes high, and the
screen stays dark. The backlight worked after boot only because the panel had been
initialised by the SBL and the driver inherited that state; once the panel rails
are cut, the state is gone until the next power cycle. For a headless server that
is a feature — after `display-off.start` nothing will light the screen again.

### Charging limit 3.85 → 3.90 V — 2026-09-06

On request (the battery should sit closer to 60% than 50%),
`maxim,constant-microvolt` was raised to 3900000. That is the only route — the
`max77693-charger` driver reads the CV voltage from the DT at probe time only,
sysfs has no attribute for it, and the debugfs regmap is read-only. Rebuild the
DTB, reboot, verify through the register: `bb: ca` → 10 × 25 mV + 3650 mV =
3900 mV. After the tests the battery sat at 37% and 3.74 V, charging in Fast mode.
Flashed package: `linux-postmarketos-qcom-apq8064-7.1_p20260906140520-r2`
(vmlinuz `a1854d181ce788f6230288f5fd29c9cf`, DTB
`d0a1698b527b805322db2e5486d44a2a`); the previous pair kept as
`/boot/vmlinuz.oc1944` and `/boot/dtb.oc1944`.

State of the phone after this session: cpufreq 384–1944 MHz (for real), L2
1188 MHz, throttling with `cooling-maps`, UV −50/−75 mV applied at boot from
`/etc/local.d`, screen blanked at boot, touchscreen unbound, charging limit
3.90 V.

---

## The screen works — 2026-09-06, afternoon

Goal of the session: "let us have a go at the screen, maybe it can be brought up".
It could: the panel shows the fbcon console (the login text), a cold start after
cutting the rails works, and DPMS on/off cycles work. The earlier conclusion about
a "sequence locked inside the bootloader" was wrong — the bootloader does nothing
that cannot be reproduced in mainline. What was actually in the way, in the order
it was found:

### aboot taken apart

The backup copy of `aboot.img` (Qualcomm LK, Thumb-2, base `0x88e00000`, MBN
header 0x28) contains two panel configurations. The one for our TFT (`1080×1920`,
`clk_rate 906 MHz`, 4 lanes, burst, RGB888, porches 100/100/14 and 6/8/2,
t_clk_pre `0x1c`, t_clk_post `0x04`) sends **five commands**: `51 80`, `55 00`,
`53 2C`, `11`, `29` — exactly what downstream does. The impressive 35-command
vendor init table (`B0 04 … B0 03`) sits right next to it, but belongs to the
600×1024 panel at 384 MHz. The power sequence (`panel_power`, `panel_reset`) read
out of the code: L15 1.8 V → 20 ms → LVS1 → 10 ms → GPIO 33 → 10 ms → GPIO 20 →
10 ms → L16 **3.0 V** → 10 ms → LED driver (PMIC GPIO 31) → 200 ms → MPP2 high
10 ms / low 10 ms / high 10 ms.

### Four real bugs

1. **PMIC pin numbering.** The `of_xlate` of the `pinctrl-ssbi-*` drivers
   subtracts `PM8XXX_*_PHYSICAL_OFFSET` (= 1), so the DT takes the physical
   number. `reset-gpios = <&pm8921_mpps 1>` was driving MPP1, and
   `backlight-gpios = <&pm8921_gpio 30>` the Home key. Proof:
   `echo 607 > /sys/class/gpio/export` (pm8921 MPP2) succeeded despite a driver
   holding it, and register `0x51` never moved on DPMS. Fix: `<&pm8921_mpps 2>`,
   `<&pm8921_gpio 31>`.
2. **Panel LDOs in LPM.** The PM8921 RPM keeps an LDO in low-power mode until a
   consumer requests a load (downstream: `regulator_set_optimum_mode(…, 100000)`).
   The driver calls `regulator_set_load(…, 100000)` on L15/L16, the DTS has
   `regulator-allow-set-load` on l2/l8/l11/l15/l16, and L16 is fixed at 3.0 V.
3. **The pixel clock.** The PLL divider only produces a VCO in 2 MHz steps, and
   pixel = VCO/6. `149666` asked for 897.996 MHz → the PLL clamped to 896 → the
   RCG rejected it (`Failed to set rate pixel clk, -22` on every start; it only
   ever worked thanks to the /3 divider left by the bootloader). Now
   `.clock = 151000`, VCO 906 MHz.
4. **Volume keys read as "pressed".** aboot leaves GPIO 35/37 with an inverted IRQ
   polarity, and `pm8xxx_gpio_get()` reads the level through the interrupt block.
   Without `qcom,no-inversion` the initramfs dropped into the debug shell
   (`check_keys`), and its `buffyboard` painted vertical stripes over `/dev/fb0`
   and spun a core at 1944 MHz — hence the "stripes and extreme heat" during the
   first attempts at lighting the panel, which for an hour looked like the DDIC
   being damaged. The old DTB masked this, because `gpio-keys` failed with EBUSY
   on GPIO 30.

### Checked and ruled out

Each item is a separate hardware test with a 12-second light-up and a finger on
the case: backlight at 6 % (heat unchanged), no rail switching and no reset
(unchanged), the D-PHY timings from the aboot table (**breaks the link** — no
image, reads still work), no EOT packets (no effect), no DCS commands during video
(no effect). The only thing all the striped boots had in common was the initramfs
debug shell.

### DDIC reads

`0x04 = 50 10 10`, `0xDA/DB/DC = 50/10/10`, `0x0C = 0x70` (24 bpp), `0xBF` all
zeroes. After a real reset `0x0A = 0x08`, after sleep-out `0x18`, after display-on
an image. The reads generate `dsi_err_worker: status=4` (`DSI_ERR_STATE_FIFO`) —
harmless, and disabled by default (`ddic_read=1`).

### Cold start: a non-continuous clock

With all of the above in place, the panel still only worked with the state
inherited from aboot. After every rail cut (`display-off.start`, DPMS off) the
next light-up gave a black screen even though the controller answered (`0x0A`:
`0x08` → `0x18` after sleep-out, ID readable). Bisected without reboots
(`skip_power=1` keeps the DDIC alive): full `power_on` — OK, reset pulse — OK,
reads disabled — OK, rails cut — black. Trying to reverse the order
(`prepare_prev_first = false`, powering up with the host disabled, commands in
`enable()` with video already running) killed the controller outright: after POR
no read came back at all.

The cause: the DSI host starts before `prepare()`, and with a continuous HS clock
the controller is powered up while it sees HS on the clock lane instead of LP-11.
Its video receiver never synchronises, even though commands go through. aboot does
not have this problem, because it powers the panel with the link sitting at 0 V and
only then initialises the PHY. `MIPI_DSI_CLOCK_NON_CONTINUOUS` (the clock lane in
LP-11 outside of transmission) settles it: four consecutive cut-and-relight cycles
from POR produced a live image (scrolling text on tty1 as the test — a static login
screen could have been a frozen frame). Downstream forced a continuous clock
(`force_clk_lane_hs = 1`), but its start-up order is different, so it is not
affected.

### Driver state

`panel-samsung-renesas-tft.c`: the sequence from aboot, a reset with 20 ms after
the last edge (the first read after 10 ms came back empty), sleep-in in `disable()`
(in `unprepare()` the DSI host is already off and the command ends with `wait for
video done timed out`), 8-bit brightness from the backlight class, and the
diagnostic parameters `lpm`, `skip_power`, `skip_reset`, `enable_cmd`, `bl_dcs`,
`eot`, `brightness`, `noncont_clk`, `ddic_read`. The PHY timing override hook
(`msm.dsi_phy_timing=…`) was taken out of the tree and remains as
`patches/05-dsi-phy-timing-override-debug.patch`.

Additionally `CONFIG_SOFTLOCKUP_DETECTOR=y` (cmdline `softlockup_panic=1`), so
that a hang ends in a panic and a warm reboot with pstore preserved.

### Final state — 2026-09-06 16:28

Package `linux-postmarketos-qcom-apq8064-7.1_p20260906162412-r2`: `/boot/vmlinuz`
`b7cc781a504067ff38f31017a5090d78`, DTB `8896ad6a011b7f36bd0c0c1424edc994`. The
cmdline carries no experiment parameters (only `softlockup_panic=1`). Verification
boot: no debug shell (the keys read correctly), the panel with a console from
15.6 s, `display-off.start` blanking it and cutting the rails at 27.6 s, a
relight from POR at 48 s with scrolling text, then off again. Zero DSI errors in
dmesg (sleep-in from `disable()` goes through, `wait for video done timed out` is
gone). The `vmlinuz.bat390` + `dtb.bat390` pair is the previous state (3.90 V
limit, dead panel). Server: `display-off.start` as before, relight with
`echo 0 > /sys/class/graphics/fb0/blank` (fbcon:
`echo 1 > /sys/class/vtconsole/vtcon1/bind`).

### GPU and GUI — state and plan (2026-09-06)

The Adreno 320 is bound to msm (`bound 4300000.gpu`, `/dev/dri/renderD128`
exists), but the `qcom/a300_pm4.fw` and `a300_pfp.fw` firmware files are missing,
so the GPU never starts. The `linux-firmware-qcom` package is in the edge repo;
alternatively the files can be pulled from `system.img` in the backup. The firmware
is requested at 14.8 s, before the rootfs is mounted — so either it goes into the
initramfs, or `4300000.gpu` gets rebound after boot. Available for a GUI:
`weston` 16, `weston-backend-drm`, `mesa-dri-gallium` (freedreno a3xx), `seatd`.
On the Nexus 7 2013 (the same SoC) pmOS runs Phosh this way. Decision: keep this in
a personal GitHub repository, with no pmOS upstreaming for now; GPU and GUI in the
next round.

### The qcom-apq8064-v7.2 branch — 2026-09-06, evening

apq8064-mainline published `qcom-apq8064-v7.2`. Checked what a move would cost:

- **Fork patch set is the same.** The apq8064-specific commits on top of Linux
  7.2 are the ones from 7.1 (jflte DTS, `qcom,no-inversion`, mdp4 clock
  fixes, a3xx VBIF drain, MAX77693, ICE4) plus new msm8960-only work (Riva,
  SMEM, SPS, huashan Wi-Fi). Only "pinctrl: qcom: Register functions before
  enabling pinctrl" is gone, because it landed upstream in 7.2. Every
  apq8064 DTS file is byte-identical between the two branches, so the DTB
  built from our DTS has the same md5 as the one on the phone.
- **Nothing in the 7.1→7.2 base delta touches our path.** DSI/mdp4 changes are
  an internal DRM rename (`drm_atomic_state` → `drm_atomic_commit` in the
  helper signatures) and the removal of `drm_connector_attach_encoder`
  calls; `drm_panel_init()` was removed, which does not matter because the
  panel driver already uses `devm_drm_panel_alloc()`. `pinctrl-ssbi-gpio`
  only had a typo renamed. tsens gained read retries and IRQ wake plumbing
  in the common core, the 8960 backend is untouched. msm dropped the a3xx
  hardcoded perf counters in favour of a generic perfcntr layer.
- **All four patches apply cleanly** (`git apply --check` on 01, 04, 05 and 06).
- **Config:** `make olddefconfig` on 7.2 with our config adds only new
  "is not set" symbols. One real finding, valid for 7.1 too:
  `CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC` is an `int`, so the `=y` in our config
  was rejected by syncconfig and reset to 0; the phone's `/proc/config.gz`
  confirms it. Panic-on-softlockup has only ever come from
  `softlockup_panic=1` on the cmdline. Fixed to `=1` in the config.
- **Build:** a worktree of the 7.2 tip with the patches applied built through
  pmbootstrap without a single warning in the log
  (`linux-postmarketos-qcom-apq8064-7.1_p20260906170835-r2`, kernel
  `7.2.0-postmarketos-qcom-apq8064`, 561 modules incl. brcmfmac and
  `krait-uv.ko`, vmlinuz `4e9a0f9ca4e4e69c987f8c198d7e6214`). The package
  name still says 7.1 because pmaports' APKBUILD has not been bumped: upstream
  pmaports is still on 7.1 (`085b3970`), no branch carries a 7.2 bump yet.
- **Flashed the same evening.** The whole package went on with
  `apk add --allow-untrusted`, because the module tree changes to `7.2.0-…`
  and Wi-Fi (brcmfmac) is a module. The install triggered mkinitfs, which
  regenerated the initramfs with the 7.2 modules, `initramfs-extra`,
  `boot.img` and `extlinux.conf`. Two things learned there:
  - `boot-deploy` composes the cmdline only from
    `/usr/lib/kernel-cmdline.d/*.conf` (one parameter per line), so anything
    added to `extlinux.conf` by hand is lost on the next regeneration.
    `softlockup_panic=1` and the `zrodlo_rozruchu=extlinux` marker now live
    in `60-local.conf` there.
  - The old `7.1.0` module tree was not owned by the package (it had been
    copied over by hand), so apk left it in place; harmless.
  Cold boot on 7.2: up in about four minutes from power-on to SSH as before,
  `uname -r` `7.2.0-postmarketos-qcom-apq8064`, Wi-Fi, `krait_uv`, charger,
  LED, touchscreen unbound, 1944 MHz, panel probed and blanked by
  `display-off.start`; a relight/blank cycle went through with no DSI
  messages; dmesg is line-for-line the 7.1 one. Rollback copies:
  `/boot/*.71final` and `/root/modules-7.1.0.tar.gz`. The project's `linux/`
  checkout now sits on `qcom-apq8064-v7.2` with the patches reapplied.

### GPU acceleration and a GUI — 2026-09-06, evening

Started the GUI round: GPU firmware, Weston as the reference test, then Phosh.

- **Firmware.** The pmOS repository has `firmware-qcom-adreno-a300` (the package
  `device-samsung-jflte` depends on), which ships `qcom/a300_pm4.fw` and
  `a300_pfp.fw`; nothing has to be pulled from the stock image. The earlier
  worry about the firmware being requested before the rootfs is mounted was
  wrong: msm loads the GPU firmware lazily on the first open of the DRM device
  (`msm_open()` → `load_gpu()`, retried as long as `priv->gpu` is NULL), so a
  plain `head -c0 /dev/dri/renderD128` after `apk add` brought the GPU up on
  the running system (`loaded qcom/a300_pm4.fw from new location`, devfreq at
  450 MHz; the DTSI only has the 27 and 450 MHz operating points).
- **Weston 16 finds the GPU and then aborts.** `weston --backend=drm` (root,
  seatd) reports `GL renderer: FD320`, EGL 1.5 (Mesa 26.1.6), GL ES 2.0, and
  `weston-simple-egl` runs. A few seconds later the compositor dies on its own
  assertion, `state-propose.c:874: plane->type == WDRM_PLANE_TYPE_OVERLAY
  (0 == 2)`. mdp4 creates two primary planes (RGB1 for DMA_P, RGB2 for DMA_E)
  and gives every plane `possible_crtcs = 0xff`, so the other CRTC's primary
  plane ends up in Weston's list of overlay candidates. Weston 16 has no
  non-atomic fallback any more (`WESTON_DISABLE_ATOMIC` ends in "Kernel DRM
  KMS does not support DRM_CLIENT_CAP_ATOMIC"), so the fix went into the
  kernel: `patches/07-mdp4-primary-plane-possible-crtcs.patch` sets
  `plane->possible_crtcs = drm_crtc_mask(crtc)` for the primary planes right
  after `mdp4_crtc_init()`, matching the fixed RGB1→DMA_P / RGB2→DMA_E paths
  the driver documents itself. Ten `PRIMARY_INTF_UDERRUN` (`errors: 00000100`)
  interrupts fired in the 150 ms around the crash and never otherwise.
  Rebuilt as `linux-postmarketos-qcom-apq8064-7.1_p20260906180456-r2`
  (vmlinuz `b8cbd8b592d4daef12d9a41eeb8c317e`, DTB unchanged) and installed
  with `apk add --allow-untrusted`.
- **Phosh.** `postmarketos-ui-phosh` plus `-openrc` (856 MiB, 869 packages).
  phoc on its own worked on the unpatched kernel: wlroots only uses the primary
  and cursor planes. The greeter (greetd + phrog) took four fixes, three of
  them self-inflicted or packaging bugs:
  - Starting busybox `syslogd` to capture logs replaced `/dev/log`, which
    belongs to pmOS's `logbookd`. The phrog session script pipes its output
    through `logger`, so the greeter died with "Broken pipe" and greetd
    reported "greeter exited without creating a session". `rc-service logbookd
    restart` recreates the socket; read the logs with `logread`. After that,
    OpenRC remembers greetd as "crashed" and `start` only warns, so
    `rc-service greetd zap` first.
  - `stevia` (the renamed `phosh-osk-stub`) does not depend on its own
    `stevia-schemas` subpackage. The OSK aborts with "Settings schema
    'mobi.phosh.osk' is not installed", gnome-session treats the OSK as a
    required component and marks the whole greeter session failed.
    `apk add stevia-schemas`.
  - elogind refused the power key, gpio-keys and MUIC evdev nodes ("Could not
    take device: No such device"): the udev rules that tag devices for the
    seat came with elogind in the same transaction, and the input devices
    created at boot had no `seat` tag. `udevadm trigger --subsystem-match=input`
    (or the next boot) fixes it; the touchscreen, bound later, was fine.
  - jflte forces `MESA_GLES_VERSION_OVERRIDE=2.0` because freedreno's GLES 3.0
    on a3xx is unstable. greetd builds the session environment from scratch,
    so neither `/etc/conf.d/greetd` nor `/etc/profile.d` reaches phoc. Added
    `session optional pam_env.so` to `/etc/pam.d/greetd` and the variable to
    `/etc/environment`; verified in phoc's `/proc/<pid>/environ`.
  Result: the phrog greeter renders at 1080x1920 (screenshot with `grim` as
  the greetd user), phoc has `libgallium`, `libGLESv2` and `libEGL` mapped, so
  it is the GPU and not pixman. RSS: phoc 120 MiB, phrog 137 MiB; 241 MiB used
  in total with the greeter up. `seatd` is installed but not enabled: elogind
  is the seat provider, and a running seatd only makes libseat try it first
  and log a permission error.
- **Headless bits undone.** `display-off.start` and `touchscreen-off.start`
  moved to `/root/headless-scripts/`, the touchscreen bound again (Synaptics
  SY 04 on event1), `greetd` added to the default runlevel. The device package
  gained the `firmware-qcom-adreno-a300` dependency and jflte's
  `adreno-a3xx-quirks.sh` in `/etc/profile.d/`.
- **Verification after a power cycle** (kernel with patch 07,
  vmlinuz `b8cbd8b592d4daef12d9a41eeb8c317e`): greetd brings the phrog
  greeter up on its own, the OSK runs, no input-device errors, 236 MiB used.
  Weston 16 now survives `weston-simple-egl` and a screenshot on the
  patched kernel (`weston-screenshooter` needs `--debug`; the picture shows
  the desktop shell and the EGL triangle). `glmark2-es2-wayland` reports
  `GL_RENDERER: FD320` and runs `build` at 43 FPS at 540x960, then the GPU
  hangs in the `texture` scene: `hangcheck detected gpu lockup rb 0`,
  offending task weston. The first recovery worked, the second one failed
  (`gpu hw init failed: -22`, i.e. `a3xx_me_init()` got no answer after the
  reset), after which phoc reports `GL_CONTEXT_LOST` and cannot create an
  FBO, the greeter is gone and only a power cycle brings the GPU back. This
  is the freedreno/a3xx instability the jflte package warns about; the
  GLES 2.0 override does not prevent it. The device tree is not the
  difference: jflte enables the GPU the same way and the DTSI declares no
  supplies for it. Plain UI work (greeter, shm textures, the Weston desktop)
  has not triggered it so far. The full dmesg of the hang is in
  `docs/gpu-hang-glmark2-2026-09-06.log` (excerpt).
- **Two side effects of the UI packages** found on that boot:
  - `postmarketos-base-ui` ships `50-random-mac.conf` for NetworkManager
    (`wifi.cloned-mac-address=stable`), so the phone connected with a random
    MAC and got a pool address instead of its static lease. Override in
    `/etc/NetworkManager/conf.d/60-local-mac.conf` with
    `wifi.cloned-mac-address=permanent`. `50-nftables.conf` and the
    `nftables` service also came along; SSH still passes.
  - `/sys/class/udc/` is empty although `ci_hdrc.0` is bound, so the USB
    gadget (and the fallback network on it) is gone. Whether this comes
    from the 7.2 move or from `postmarketos-usb-moded` is still open; the
    cable was moved to a charger before it could be checked.

### First real use of Phosh — 2026-09-06, late evening

Six complaints after logging in: shadows and flicker in the greeter, a
laggy Phosh, no sound over Bluetooth, the battery draining on the charger,
the Menu and Back keys dead, sensors untested. Plus two full system hangs.
What each of them turned out to be:

- **Greeter artifacts.** phrog is GTK4; on this freedreno the GTK GL
  renderer draws smeared shadows. Phosh itself is GTK3 through cairo and was
  clean, which pointed straight at the client. `GSK_RENDERER=cairo` in
  `/etc/environment` (reaching sessions through the same `pam_env` hook as
  the GLES override) puts GTK4 on software rendering.
- **The lag was the kernel, not the touchscreen.** Touch reports arrive at
  45-80 Hz while scrolling, so sampling is fine. `perf record -a` during
  scrolling showed `a3xx_pm_suspend` eating 17 % of all CPU time, with the
  GPU's `runtime_suspended_time` at zero after 20 minutes of uptime. The
  fork's "drm/msm/a3xx: Drain VBIF before GPU suspend" halts the VBIF
  before every runtime suspend and waits with `spin_until()`, which
  busy-loops for `ADRENO_IDLE_TIMEOUT` (one second, no sleeping). On this
  unit the halt is never acknowledged, so every suspend attempt burned a
  core for a second, failed with -EBUSY and was retried 66 ms later, all
  day long: the SoC sat at 73-75 C at idle, the cooling maps throttled the
  cores to 1.5-1.7 GHz, and the UI stuttered. `echo on >
  .../4300000.gpu/power/control` (now `/etc/local.d/gpu-runtime-pm-off.start`)
  dropped idle system time from 4-30 % to 0-1 % and the temperature to
  46-50 C; the user's verdict was "a million times better". Downstream
  KGSL only halts the VBIF on reset, with a 100 ms limit, and proceeds
  whether or not it is acknowledged. Patch 08 does the same in
  `a3xx_vbif_halt()`: bounded poll with `usleep_range()`, one warning on
  timeout, then the normal suspend. Built, not yet verified on the phone.
- **The status LED cost 5 % of a core.** With the display on, feedbackd
  from the Phosh session owns the RGB LED (it resets the trigger to
  `pattern` and the brightness to 0), so the headless breathing script
  fought with it, and the software `pattern` trigger pushed ~40 brightness
  updates per second over the bit-banged `i2c-gpio` bus behind the AN30259A,
  visible in perf as `__timer_udelay` from `i2c_outb`. `led-status.start`
  is retired to `/root/headless-scripts/`; the LED is the notification LED now.
- **Two hard hangs**, one while using Phosh and one right after a greeter
  restart, both with the WiFi association still alive but no traffic. The
  second one ended in a spontaneous reset with an empty pstore, which
  points at a PMIC or watchdog reset rather than a panic. The undervolt
  (-50/-75 mV) was validated with a CPU-only stress ladder on the headless
  setup; the GUI adds GPU and panel load on the same rails. `krait-uv.start`
  is disabled until the GUI has run a day without a hang. No hang since,
  but the GPU spin above also disappeared in the same window, so this is
  not settled.
- **Bluetooth audio.** `postmarketos-ui-phosh` does not pull the audio
  backend: no `pipewire-pulse`, no `pipewire-spa-bluez`, hence only the dummy
  output. `apk add postmarketos-base-ui-audio
  postmarketos-base-ui-audio-backend-pipewire`; takes effect on the next login
  because PipeWire starts from the session's autostart.
- **Charging.** The input limit is 1.9 A (`CHGIN_ILIM = 0x5f`), the MUIC
  reports DCP, so the 500 mA seen on the meter is not a limit but the
  phone's draw while the charger sits in top-off at the deliberate 3.90 V
  constant-voltage ceiling. Under GUI load the battery drifts below that
  and the charger tops it up again; the level hovers around 50-55 % by
  design. If the phone is going to be used with the screen on, raising
  `maxim,constant-microvolt` (4.10 V is about 80 %) gives headroom; that
  is a trade against cell ageing and stays the owner's call.
- **Menu and Back keys.** The S4 Active has physical keys where the S4
  has capacitive ones; downstream `jactive_eur-gpio.h` puts them on PM8921
  GPIO 4 and 5. Added to `gpio-keys` in the DTS with the same pinconf
  group as Home and the volume keys (the `qcom,no-inversion` quirk
  included). Built and installed, needs a power cycle.
- **Sensors.** No IIO device exists because every sensor on this board sits
  behind Samsung's SSP sensor hub (an STM32 on SPI, `CONFIG_SENSORS_SSP`
  downstream). Mainline's `ssp_sensors` driver targets the Gear 2 hub and
  its protocol; getting this hub to talk is a separate project.
- **Side finding:** brcmfmac polls the BCM4335 over SDIO with CMD52 about
  25 times a second because the qcom mmci variant has no SDIO IRQ support
  and no `host-wake` interrupt is wired up; each poll bounces the SDCC3
  host through runtime PM. Downstream uses TLMM GPIO 61 or 65 for host-wake
  depending on the board revision; neither pin pulsed under traffic in a
  quick `gpiomon` test, which proves nothing until the driver enables the
  OOB signal. Left alone, it is a few per cent of a core.
