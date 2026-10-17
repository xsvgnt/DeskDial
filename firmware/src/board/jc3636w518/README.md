# JC3636W518 board support

| File | Origin |
|---|---|
| `scr_st77916.h` | **Provided by the board vendor** in the JC3636W518 example package (ST77916 panel init sequence, CST816S touch, backlight and LVGL display glue). Included unmodified. It is not covered by this project's MIT licence; its copyright belongs to the vendor. |
| `pincfg.h` | Written for DeskDial (MIT): the board's pin assignments, using the macro names the vendor driver expects. |

Only `src/display.cpp` includes the vendor driver, since it defines its
variables and functions directly in the header.
