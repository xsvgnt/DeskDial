# Third-party notices

DeskDial's own code is under the MIT licence (`LICENSE`). These parts come
from elsewhere and keep their own terms.

## Included in this repository

| What | Where | Licence |
|---|---|---|
| JC3636W518 display driver (`scr_st77916.h`): ST77916 panel init sequence, CST816S touch, backlight and LVGL glue | `firmware/src/board/jc3636w518/` | Provided by the board vendor in its JC3636W518 example package and included unmodified. Not covered by DeskDial's MIT licence; copyright belongs to the vendor. |
| Montserrat Medium, converted to LVGL bitmap fonts (`font_latin_14/16/22.c`, `font_num_34.c`) | `firmware/src/fonts/` | SIL Open Font License 1.1, © The Montserrat Project Authors. Text: `licenses/Montserrat-OFL.txt`. |
| Font Awesome 5 Free glyphs U+F001, U+F04B, U+F04C (music note, play, pause), converted to an LVGL bitmap font (`font_icons_48.c`) | `firmware/src/fonts/` | Font Awesome Free: font files under SIL OFL 1.1, icons under CC BY 4.0, © Fonticons, Inc. Text: `licenses/FontAwesome-LICENSE.txt`. |
| `lv_conf.h` | `firmware/include/` | Based on LVGL's `lv_conf_template.h` v8.4.0, MIT licence, © LVGL Kft. |

The fonts were converted with [lv_font_conv](https://github.com/lvgl/lv_font_conv)
1.5.2 from the TTF/WOFF files shipped with LVGL 8.4.0; the exact command is
in the header of each `.c` file.

## Downloaded at build time (not in this repository)

| What | Used by | Licence |
|---|---|---|
| [LVGL](https://github.com/lvgl/lvgl) 8.4.0 | firmware, simulator | MIT |
| [ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) 0.1.4 | firmware | Apache 2.0 |
| [ESP32_IO_Expander](https://github.com/esp-arduino-libs/ESP32_IO_Expander) 0.0.2 | firmware | Apache 2.0 |
| [Arduino core for the ESP32](https://github.com/espressif/arduino-esp32) | firmware | LGPL 2.1 |
| [LibreHardwareMonitorLib](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) 0.9.6 | Windows companion, hardware module | MPL 2.0 |
| [System.IO.Ports](https://www.nuget.org/packages/System.IO.Ports) | Windows companion | MIT |

The published `DeskDial.exe` bundles LibreHardwareMonitorLib (hardware
builds) and System.IO.Ports. LibreHardwareMonitor's source is available from
its repository above. Reading CPU and motherboard sensors also needs the
[PawnIO](https://pawnio.eu) driver, which users install themselves.
