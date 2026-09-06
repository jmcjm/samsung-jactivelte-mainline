# postmarketOS on the Samsung Galaxy S4 Active (GT-I9295, jactivelte)

A mainline Linux 7.1 port of the Galaxy S4 Active, built on top of the existing
postmarketOS port for the regular Galaxy S4 (GT-I9505, `samsung-jflte`) and the
[apq8064-mainline](https://github.com/apq8064-mainline/linux) kernel tree. The
phone runs headless as a small home server, but the display works too.

This is not upstreamed to postmarketOS (yet). Everything needed to rebuild the
kernel and the system image is in this repository; the kernel tree itself is
not vendored, see below.

## What works

| Component | Status |
|---|---|
| Boot via lk2nd (extlinux from the pmOS boot partition) | works |
| eMMC, USB gadget networking, WiFi (BCM4335), Bluetooth firmware | works |
| Display: JDI 1080x1920 TFT with a Renesas DSI controller | **works** (fbcon, DPMS on/off, cold start after cutting the rails) |
| Backlight (DCS brightness through the panel controller) | works |
| Touchscreen (Synaptics RMI4) | works, unbound by default on the headless setup |
| Keys (power, home, volume) | works after fixing the inverted IRQ polarity the bootloader leaves behind |
| Battery gauge, charger with a constant-voltage limit (3.90 V, about 60 %) | works |
| cpufreq 384 to 1944 MHz on the real clock, L2 at 1188 MHz, thermal throttling | works |
| Runtime undervolting (`krait-uv` module) | works |
| LED, sensors | LED works, sensors untested |
| GPU (Adreno 320) | driver binds, firmware `qcom/a300_pm4.fw` / `a300_pfp.fw` not installed yet, so no acceleration so far |
| Modem, audio, camera | not touched |

## Repository layout

```
patches/    everything that differs from the upstream kernel tree and pmaports
pmaports/   device-samsung-jactivelte package for pmaports
docs/       reverse-engineering notes (bootloader panel init)
work/       helper scripts used on the desktop and on the phone
WORKLOG.md  the full work log
```

`patches/` contains:

| file | purpose |
|---|---|
| `01-register-panel-and-dtb.patch` | Kconfig/Makefile entries for the panel driver and the DTB |
| `03-kernel-config-charger.patch` | `CONFIG_CHARGER_MAX77693=y` in the pmaports kernel config |
| `04-hfpll-max-rate-1944.patch` | lets the Krait HFPLLs run at 1944 MHz (mainline caps them at 1800 and silently runs 1782) |
| `05-dsi-phy-timing-override-debug.patch` | debug only: override the D-PHY timings from the kernel command line |
| `06-krait-uv-build.patch` | Kconfig/Makefile entries for the `krait-uv` module |
| `panel-samsung-renesas-tft.c` | the panel driver |
| `qcom-apq8064-samsung-jactivelte.dts` | the device tree |
| `krait-uv.c` | runtime OPP voltage adjustment module |
| `config-postmarketos-qcom-apq8064.armv7` | the complete kernel config used |

## Rebuilding

1. pmbootstrap from git (the PyPI releases are all yanked):

   ```sh
   git clone https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git ~/.local/share/pmbootstrap
   cd ~/.local/share/pmbootstrap && python3 -m venv .venv && .venv/bin/pip install -e .
   pmbootstrap -c ~/.config/pmbootstrap_jactivelte.cfg init
   ```

   Device `samsung-jactivelte` (after step 2), UI `none`, OpenRC, channel `edge`.
   The apq8064 family only exists in edge; the stable branch does not carry
   `linux-postmarketos-qcom-apq8064` at all.

2. pmaports: copy `pmaports/device-samsung-jactivelte` into
   `device/testing/` of your pmaports checkout, apply
   `patches/03-kernel-config-charger.patch` or simply drop
   `patches/config-postmarketos-qcom-apq8064.armv7` over the kernel config in
   `device/testing/linux-postmarketos-qcom-apq8064/`, then
   `pmbootstrap checksum device-samsung-jactivelte linux-postmarketos-qcom-apq8064`.

3. Kernel:

   ```sh
   git clone --depth 1 --branch qcom-apq8064-v7.1 https://github.com/apq8064-mainline/linux.git
   cd linux
   git apply ../patches/01-register-panel-and-dtb.patch
   git apply ../patches/04-hfpll-max-rate-1944.patch
   git apply ../patches/06-krait-uv-build.patch
   cp ../patches/panel-samsung-renesas-tft.c drivers/gpu/drm/panel/
   cp ../patches/qcom-apq8064-samsung-jactivelte.dts arch/arm/boot/dts/qcom/
   cp ../patches/krait-uv.c drivers/cpufreq/
   printf '.git/\n' > .gitignore
   ```

   The last line matters: `pmbootstrap build --src` rsyncs the tree with
   `--exclude-from=.gitignore`, and the kernel's own `.gitignore` drops the
   tracked `kernel/time/timeconst.bc`, which breaks the build.

   The `qcom-apq8064-v7.2` branch builds with exactly the same patch set
   (checked 2026-09-06: the fork's apq8064 patches are unchanged, all four
   patches apply, the DTB is byte-identical). It has not been flashed yet, and
   pmaports still ships 7.1, so the package version comes from its APKBUILD
   until that is bumped. See WORKLOG.md for the details.

   ```sh
   pmbootstrap -c <cfg> build --src ./linux linux-postmarketos-qcom-apq8064
   pmbootstrap -c <cfg> build device-samsung-jactivelte
   pmbootstrap -c <cfg> install --password <password>
   ```

4. Flashing: lk2nd (`lk2nd.img` from the apq8064-mainline project) goes to the
   `BOOT` partition with heimdall from Download Mode, one heimdall command per
   Download Mode session (`--no-reboot`/`--resume` do not work on this
   bootloader). Then `pmbootstrap flasher flash_rootfs` through lk2nd's fastboot.
   The pmOS image lands on the `userdata` partition with its own partition table
   and is mounted through loop devices.

5. Updating just the kernel later: copy `vmlinuz` and the DTB from the built
   package to `/boot/` on the phone; lk2nd reads `/boot/extlinux/extlinux.conf`.
   `boot.img` is not used at all.

## Things that cost days, so you do not repeat them

- **PMIC pin numbers in the device tree are physical (from 1).** The
  `pinctrl-ssbi-gpio`/`-mpp` `of_xlate` subtracts one. PMIC GPIO 31 is
  `<&pm8921_gpio 31>`, MPP2 is `<&pm8921_mpps 2>`. Off-by-one here drove the
  panel reset onto MPP1 and the backlight enable onto the Home key.
- **Volume keys read as pressed.** The bootloader leaves PMIC GPIO 35/37 with
  an inverted interrupt polarity and `pm8xxx_gpio_get()` reads inputs through
  the IRQ block. Without `qcom,no-inversion` in the gpio-keys pinconf the
  postmarketOS initramfs sees Volume Down held and drops into its debug shell,
  where `buffyboard` paints vertical stripes on the framebuffer and spins a
  core at 1944 MHz. That looked exactly like a dying display controller.
- **Panel LDOs need a load request** or the RPM keeps them in low-power mode.
- **Cold start of the panel needs a non-continuous DSI clock**, see
  `docs/aboot-panel-init.md`.
- **The pixel clock is not free**: VCO in 2 MHz steps, pixel = VCO/6.
- **`&dsi0` must stay enabled** even with no panel: `mdp` has
  `assigned-clock-parents` pointing at the DSI PHY PLL and boot hangs without a
  single log line otherwise.
- **Use `poweroff`, not `reboot`, when unattended.** apq8064 occasionally hangs
  on a warm reboot; a shutdown never does.
- **`apq8064` HFPLLs are capped at 1800 MHz in mainline.** With the 1944 MHz OPP
  the OPP layer picks the voltage for 1944 and the PLL rounds to 1782 without
  telling anyone; `scaling_cur_freq` reports the request, `cpuinfo_cur_freq` the
  truth.

## Operating notes for the headless setup

- `/etc/local.d/display-off.start` detaches fbcon and blanks the panel after
  boot, which also cuts its rails. Turn it back on with
  `echo 0 > /sys/class/graphics/fb0/blank` (and `echo 1 >
  /sys/class/vtconsole/vtcon1/bind` for the console). Delete the script to keep
  the display on permanently.
- `/etc/local.d/krait-uv.start` applies -50 mV below 1134 MHz and -75 mV above,
  with the regulator floor at 850 mV. -100 mV on the top states hangs this unit.
- The charger limit lives in the DTS (`maxim,constant-microvolt`), verified
  through `/sys/kernel/debug/regmap/0-0066/registers` (`bb: ca` is 3.90 V).
- The panel driver exposes debug knobs under
  `/sys/module/panel_samsung_renesas_tft/parameters/` (`ddic_read` logs the
  controller's DCS status registers, `skip_power` keeps the controller alive
  across DPMS cycles, `lpm`, `eot`, `noncont_clk`, `bl_dcs`, `enable_cmd`,
  `brightness`).

## License

Kernel patches, the panel driver, the device tree and the module are GPL-2.0.
Scripts and notes are provided under the same terms.
