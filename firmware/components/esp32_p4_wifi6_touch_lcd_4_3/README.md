# HD28091 display adapter BSP

This branch uses the HD28091C40-V1 ST7701S panel and Rev A adapter.
The panel is 480x640 and the runtime rotates a 640x480 logical framebuffer.
The supplier initialization table preserves the panel voltage, Gamma and GIP settings.

`sdkconfig.defaults` selects two DSI data lanes at 500 Mbps per lane through
`CONFIG_BSP_LCD_DSI_DATA_LANES` and `CONFIG_BSP_LCD_DSI_LANE_BITRATE_MBPS`.
DSI PHY clock selection uses the SDK default for each chip revision.
The v3 DPI clock is 20 MHz and v1 uses 25 MHz. Horizontal timing is
480/20/4/328 and vertical timing is 640/16/4/14, giving approximately
35.7 Hz on v3 and 44.6 Hz on v1.

LCD and touch use independent RC resets. The BSP waits for power to settle,
uses software LCD reset and a write-only DSI command path, and keeps reset GPIOs
unassigned. Rev A powers the backlight continuously. The runtime uses DCS
display off/on to reveal the first rendered frame.

CST328 uses I2C address 0x1A and CST3530 uses 0x58 on GPIO7/GPIO8.
The default is 15 ms polling with INT unconnected. The HD28091 CST3530
module's 240x320 coordinate range is scaled and rotated into the 640x480
interface. Wiring TP_INT to GPIO22 and defining `BSP_LCD_TOUCH_INT=22`
enables the optional CST328 interrupt path.

## Backlight

```c
bsp_display_brightness_init();
bsp_display_backlight_on();
bsp_display_backlight_off();
bsp_display_brightness_set(100);
```

## Dependencies

| Capability | Implementation |
|---|---|
| Display | Local `esp_lcd_st7701` 2.0.2 override |
| Touch | Project CST328 and CST3530 polling drivers and `espressif/esp_lcd_touch_gt911` fallback |
| Audio | `espressif/esp_codec_dev` |
| SD card | ESP-IDF |

Display profile and runtime diagnostics are available in `diagnostics/status`.
Source adaptation, build targets and hardware validation status are recorded in
[the hardware adaptation guide](../../../docs/hd28091-hardware-adaptation.md).
