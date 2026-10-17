// DeskDial firmware: interfaces between the modules.
//
//   main.cpp           setup/loop, 100 ms UI tick, pages and swipes
//   link.cpp           serial framing, handshake, link state
//   display.cpp        panel/touch/backlight (vendor driver), brightness and
//                      its dialog, settings (NVS), PC-off mode
//   media_page.cpp     "now playing" page            (DESKDIAL_MEDIA)
//   hardware_page.cpp  loads/temperatures/memory page (DESKDIAL_HARDWARE)
//
// Everything runs in the Arduino loop task (serial parsing, LVGL, UI state),
// so no locking is needed anywhere.
// SPDX-License-Identifier: MIT
#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include "config.h"
#include "protocol.h"

// ----------------------------------------------------------------- fonts --
LV_FONT_DECLARE(font_latin_14); // Montserrat + Latin-1, see src/fonts
LV_FONT_DECLARE(font_latin_16);
LV_FONT_DECLARE(font_latin_22);
#if DESKDIAL_MEDIA
LV_FONT_DECLARE(font_icons_48); // play / pause / music note
#endif
#if DESKDIAL_HARDWARE
LV_FONT_DECLARE(font_num_34); // digits for temperatures
#endif

// ---------------------------------------------------------------- colours --
#define COL_ACCENT 0x1DB954 // progress arc, slider, Save button
#define COL_TRACK 0x202020  // unfilled arcs and bars, empty artwork
#define COL_GREY 0xA0A0A0   // secondary text

// ------------------------------------------------------------- UI helpers --
void ui_show(lv_obj_t *o, bool visible);
lv_obj_t *ui_new_screen(); // black, not scrollable, swipes switch pages
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text,
                   lv_align_t align, lv_coord_t x, lv_coord_t y);

// ------------------------------------------------------------------ link --
struct LinkState
{
  bool up = false;          // a frame arrived within LINK_TIMEOUT_MS
  uint8_t pc_features = 0;  // FEATURE_* the PC announced (0 = not known yet)
  uint32_t last_rx_ms = 0;
};
extern LinkState link_state;

void link_begin();          // serial port and receive buffer
void link_poll();           // parse incoming bytes, dispatch frames
void link_tick(uint32_t t); // timeout and handshake retry
void link_reset();          // forget the link (PC went away)
void link_send(uint8_t type, const uint8_t *payload, uint32_t len);
void link_send_hello(bool reply = false); // HELLO, then the current page
uint8_t board_features();   // FEATURE_* compiled into this firmware
uint32_t rd32(const uint8_t *p);

// ------------------------------------------------------------------ main --
void app_fatal(const char *msg); // never returns
void app_on_link_lost();         // no frame for LINK_TIMEOUT_MS
void app_on_pc_off();            // called by display.cpp
void app_on_pc_back();
// Frames other than HELLO, for the page modules; false if nobody wanted it.
bool app_on_frame(uint8_t type, const uint8_t *p, uint32_t len);

// ----------------------------------------------------------------- pages --
uint8_t page_current();
void page_refresh_available(); // after the PC announced its modules

// --------------------------------------------------------------- display --
void display_begin();            // settings, panel, touch, backlight, LVGL
uint8_t settings_page();         // last page shown (saved)
void settings_save_page(uint8_t page);
void display_tick(uint32_t t, bool idle); // dims the backlight 10 s into idle
bool display_dialog_open();
void display_open_brightness_dialog();

// Long press: true once `target` has been held SETTINGS_HOLD_MS.
struct LongPress
{
  uint32_t start = 0;
  bool fired = false; // lets the owner swallow the click that ends the press
};
bool long_press_tick(LongPress &lp, lv_obj_t *target, uint32_t t, bool enabled);

// PC presence from the USB heartbeat. True while the PC is off: the loop
// then does nothing else.
void power_begin();
bool power_tick(uint32_t t);

// ----------------------------------------------------------------- media --
#if DESKDIAL_MEDIA
void media_begin();             // buffers and screen
lv_obj_t *media_screen();
bool media_on_frame(uint8_t type, const uint8_t *p, uint32_t len);
void media_tick(uint32_t t, bool visible);
bool media_idle();              // nothing playing (dims the backlight)
void media_reset();             // link lost / PC back
void media_on_hidden();         // page switched away
#ifdef DESKDIAL_SIM
uint32_t media_debug_position();
#endif
#endif

// -------------------------------------------------------------- hardware --
#if DESKDIAL_HARDWARE
void hardware_begin();
lv_obj_t *hardware_screen();
bool hardware_on_frame(uint8_t type, const uint8_t *p, uint32_t len);
void hardware_tick(uint32_t t, bool visible);
void hardware_reset();          // link lost / PC back
#endif
