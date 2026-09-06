# What the stock bootloader does with the panel

Reverse-engineered from the `aboot` partition of a GT-I9295 (Samsung's LK-based
bootloader, Thumb-2, MBN header of 0x28 bytes, load address `0x88e00000`, so
`file offset = virtual address - 0x88e00000 + 0x28`). Tools: `llvm-objcopy -I binary`
to wrap the image into an ELF, `llvm-objdump --triple=thumbv7-none-eabi`, and a
Python scan for LK-style DSI packets (`len 00 39 c0` / `len 00 29 c0` long writes,
`xx yy 15 80` / `xx yy 23 80` short writes).

## Two panel configurations

`aboot` carries two `msm_panel_info` initialisers. The one that matters for
this device (function at file offset 0x1b264) sets:

| field | value |
|---|---|
| xres x yres | 1080 x 1920, 24 bpp |
| clk_rate | 906 000 000 (DSI bit clock) |
| porches | hbp 100, hfp 100, hpw 14, vbp 6, vfp 8, vpw 2 |
| lanes | 4, no lane swap, RGB888, virtual channel 0 |
| t_clk_post / t_clk_pre | 0x04 / 0x1c |
| video mode | burst, hfp/hbp/hsa power stop off, BLLP and EOF-BLLP power stop on |
| commands | 5 entries at 0x88f2ed44 |

The five commands, all DCS long writes:

```
51 80      display brightness
55 00      CABC off
53 2c      write control display: BCTRL, DD, BL
11         sleep out
29         display on
```

There are no manufacturer commands. The 35-entry table sitting next to it
(`B0 04 ... B0 03`, gamma in `C8/C9/CA`, MADCTL, pixel format) belongs to the
other configuration, a 600x1024 panel at 384 MHz, and must not be sent to this
one.

The PHY table (`regulator {03 0a 04 00 20}`, `timing {5c 37 39 00 62 57 3b 3b
44 03 04 a0}`, `ctrl {5f 00 00 10}`, `strength {ff 00 06 00}`) is identical to
the downstream `dsi_video_mode_phy_db`. Writing that timing table into the
mainline 28nm-8960 PHY registers breaks the link (the panel stops showing an
image while DCS reads still work), so the timings computed by `msm_dsi_dphy_timing_calc()`
are the ones to use.

## Power sequence (`panel_power`, file offset 0x1b108)

```
pm8921_ldo_set_voltage(LDO_2, 1.2 V)           DSI PLL supply
pm8921_ldo_set_voltage(LDO_15, 1.8 V)          panel VDD
mdelay(20)
pm8921_low_voltage_switch_enable(LVS1)         panel IOVDD
mdelay(10)
gpio 33 = 1                                    LCD_22V_EN
if hw_rev > 15: mdelay(10); gpio 20 = 1        LCD_22V_EN_2
mdelay(10)
if hw_rev > 14: pm8921_ldo_set_voltage(LDO_16, 3.0 V); mdelay(10)   panel AVDD
PMIC GPIO 31 = 1                               LED driver enable
mdelay(200)
```

Reset (`panel_reset`, file offset 0x1b0a8) drives PM8921 MPP2 through
register 0x51 (0x25 = digital output high on VIO_1, 0x24 = low):

```
MPP2 = 1; mdelay(10); MPP2 = 0; mdelay(10); MPP2 = 1; mdelay(10)
```

Power off: MPP2 low, 20 ms, LDO16 off, LDO15 off, gpio 20 low, 10 ms, gpio 33
low.

Only then does `aboot` initialise the DSI PHY and send the five commands, with
the video engine still off, and start video afterwards.

## What that meant for the mainline driver

- The mainline msm host runs before the panel's `prepare()` (`prepare_prev_first`),
  so the panel gets its rails while the clock lane already carries a continuous
  HS clock. The controller then answers DCS reads and executes sleep-out, but
  its video receiver never locks and the screen stays black after every cold
  start. A non-continuous clock (`MIPI_DSI_CLOCK_NON_CONTINUOUS`) keeps the lane
  in LP-11 while idle and the controller comes up every time.
- The RPM keeps PM8921 LDOs in low-power mode until a consumer asks for
  current; downstream calls `regulator_set_optimum_mode(..., 100000)`. The panel
  driver requests 100 mA on L15 and L16 and the DTS allows it with
  `regulator-allow-set-load`.
- The DSI PLL feedback divider only produces VCO rates in 2 MHz steps and the
  pixel clock is VCO/6 for RGB888 on four lanes, so the mode clock must be a
  multiple of 1/3 MHz: 151 000 kHz (VCO 906 MHz, as in aboot).
