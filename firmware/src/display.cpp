// Panel, touch and backlight; brightness and its dialog; saved settings;
// "PC is off" mode.
//
// This is the only file that includes the board driver: the vendor header
// defines its variables and functions directly, so it can only be compiled
// once. The simulator (test/sim) swaps in its own driver with -DBOARD_DRIVER.
// SPDX-License-Identifier: MIT
#include "app.h"
#include <Preferences.h>
#include "soc/usb_serial_jtag_reg.h"

#ifndef BOARD_DRIVER
#define BOARD_DRIVER "board/jc3636w518/scr_st77916.h"
#endif
#include BOARD_DRIVER // scr_lvgl_init(), set_brightness(), screen_switch(), touch

// ---------------------------------------------------------------- settings --
// Namespace and keys in NVS; survive reflashing unless the flash is erased.
static Preferences prefs;
static const char *NVS_NAMESPACE = "deskdial";
static uint8_t bright_full = BRIGHT_DEFAULT; // user setting, 10..100 %
static uint8_t saved_page = 0;
static bool dimmed = false;

static uint8_t clamp_brightness(int v)
{
  v = (v + 5) / 10 * 10; // snap to the 10 % grid
  return (uint8_t)(v < 10 ? 10 : (v > 100 ? 100 : v));
}

static void settings_load()
{
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bright_full = clamp_brightness(prefs.getUChar("bright", BRIGHT_DEFAULT));
  saved_page = prefs.getUChar("page", PAGE_MEDIA);
  prefs.end();
}

static void settings_save_brightness(uint8_t v)
{
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar("bright", v);
  prefs.end();
}

uint8_t settings_page() { return saved_page; }

void settings_save_page(uint8_t page)
{
  if (page == saved_page)
    return;
  saved_page = page;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar("page", page);
  prefs.end();
}

// --------------------------------------------------------------- UI helpers --
void ui_show(lv_obj_t *o, bool visible)
{
  if (visible)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text,
                   lv_align_t align, lv_coord_t x, lv_coord_t y)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  lv_obj_align(l, align, x, y);
  return l;
}

// ------------------------------------------------------------------ setup --
static lv_obj_t *scr_off; // "PC is off"

void display_begin()
{
  settings_load();   // saved values, or the defaults on first boot
  scr_lvgl_init();   // vendor init: panel, touch, LVGL; backlight on at 100 %
  set_brightness(bright_full);

  scr_off = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_off, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr_off, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr_off, LV_OBJ_FLAG_SCROLLABLE);
  ui_label(scr_off, &font_latin_22, COL_GREY, "PC is off", LV_ALIGN_CENTER, 0, 0);
}

// ------------------------------------------------------------- backlight --
// Dims 10 s into idle (nothing playing on the media page); any touch or a
// track starting restores it. The hardware page is never dimmed.
void display_tick(uint32_t t, bool idle)
{
  static uint32_t idle_since = 0;
  if (!idle)
    idle_since = 0;
  else if (!idle_since)
    idle_since = t | 1; // 0 means "not idle"
  bool dim = idle && !display_dialog_open() && t - idle_since >= DIM_AFTER_MS &&
             lv_disp_get_inactive_time(NULL) >= DIM_AFTER_MS;
  if (dim != dimmed)
  {
    set_brightness(dim ? BRIGHT_DIM : bright_full);
    dimmed = dim;
  }
}

bool long_press_tick(LongPress &lp, lv_obj_t *target, uint32_t t, bool enabled)
{
  if (!enabled || !lv_obj_has_state(target, LV_STATE_PRESSED))
  {
    lp.start = 0;
    return false;
  }
  if (!lp.start)
    lp.start = t | 1;
  else if (t - lp.start >= SETTINGS_HOLD_MS)
  {
    lp.start = 0;
    lp.fired = true;
    return true;
  }
  return false;
}

// ------------------------------------------------------ brightness dialog --
static lv_obj_t *dlg_backdrop = nullptr, *dlg_slider, *dlg_value;

bool display_dialog_open() { return dlg_backdrop != nullptr; }

static void dlg_show_value(int pct)
{
  lv_label_set_text_fmt(dlg_value, "%d %%", pct);
}

static void dlg_close(bool save)
{
  if (!dlg_backdrop)
    return;
  int pct = lv_slider_get_value(dlg_slider) * 10;
  if (save && pct != bright_full)
  {
    bright_full = (uint8_t)pct;
    settings_save_brightness(bright_full);
  }
  lv_obj_del_async(dlg_backdrop); // we're inside a child's event: delete after it
  dlg_backdrop = nullptr;
  set_brightness(bright_full); // Cancel: back to the saved value
  dimmed = false;
}

static void on_dlg_slider(lv_event_t *e)
{
  int pct = lv_slider_get_value(dlg_slider) * 10;
  dlg_show_value(pct);
  set_brightness(pct); // live preview
}

static void on_dlg_button(lv_event_t *e)
{
  dlg_close((bool)(uintptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *make_dlg_button(lv_obj_t *parent, const char *text, bool primary, lv_coord_t x)
{
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 100, 44);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -14);
  lv_obj_set_style_radius(b, 22, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(primary ? COL_ACCENT : 0x3A3A3A), 0);
  lv_obj_add_event_cb(b, on_dlg_button, LV_EVENT_CLICKED, (void *)(uintptr_t)primary);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &font_latin_16, 0);
  lv_obj_set_style_text_color(l, primary ? lv_color_black() : lv_color_white(), 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

void display_open_brightness_dialog()
{
  if (dlg_backdrop)
    return;

  // Full-screen backdrop on the top layer: dims the UI and swallows touches.
  // Translucency through bg_opa only (never animated opa on parents).
  dlg_backdrop = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(dlg_backdrop);
  lv_obj_set_size(dlg_backdrop, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(dlg_backdrop, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(dlg_backdrop, LV_OPA_70, 0);
  lv_obj_add_flag(dlg_backdrop, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(dlg_backdrop, LV_OBJ_FLAG_SCROLLABLE);

  // Panel fits inside the 254 px square that the round screen shows whole
  lv_obj_t *panel = lv_obj_create(dlg_backdrop);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 250, 200);
  lv_obj_center(panel);
  lv_obj_set_style_radius(panel, 18, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x181818), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x303030), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  ui_label(panel, &font_latin_22, 0xFFFFFF, "Brightness", LV_ALIGN_TOP_MID, 0, 16);
  dlg_value = ui_label(panel, &font_latin_16, COL_GREY, "", LV_ALIGN_TOP_MID, 0, 48);

  dlg_slider = lv_slider_create(panel);
  lv_obj_set_size(dlg_slider, 190, 10);
  lv_obj_align(dlg_slider, LV_ALIGN_TOP_MID, 0, 92);
  lv_slider_set_range(dlg_slider, 1, 10); // x10 = 10..100 %, step 10
  lv_slider_set_value(dlg_slider, bright_full / 10, LV_ANIM_OFF);
  lv_obj_set_ext_click_area(dlg_slider, 20); // easier to grab on a small screen
  lv_obj_set_style_bg_color(dlg_slider, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dlg_slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dlg_slider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(dlg_slider, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_pad_all(dlg_slider, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(dlg_slider, on_dlg_slider, LV_EVENT_VALUE_CHANGED, NULL);
  dlg_show_value(bright_full);

  make_dlg_button(panel, "Cancel", false, -58);
  make_dlg_button(panel, "Save", true, 58);

  set_brightness(bright_full); // in case the screen was dimmed
  dimmed = false;
}

// ============================================================ PC presence ==
// A powered, awake USB host sends a start-of-frame packet every 1 ms, even when
// no program has the port open. The USB Serial/JTAG peripheral latches the
// 11-bit frame number: if it stops changing, the PC is off, asleep or gone.
// While it is, the board does the minimum: backlight off, no LVGL rendering
// (the panel keeps the last image in its own RAM), slower CPU, and one cheap
// touch read every 100 ms.
static bool pc_off = false;
static uint32_t last_sof_ms = 0, last_frame = 0xFFFFFFFF, peek_until = 0;

static bool usb_host_alive(uint32_t t)
{
#if ARDUINO_USB_MODE // native USB Serial/JTAG (this board)
  uint32_t f = REG_GET_FIELD(USB_SERIAL_JTAG_FRAM_NUM_REG, USB_SERIAL_JTAG_SOF_FRAME_INDEX);
  if (f != last_frame)
  {
    last_frame = f;
    last_sof_ms = t;
  }
  return t - last_sof_ms < PC_OFF_AFTER_MS;
#else
  return true; // USB-OTG/TinyUSB mode: frame register unused, feature off
#endif
}

void power_begin()
{
  last_sof_ms = millis(); // grace period while USB enumerates
}

static void enter_pc_off()
{
  pc_off = true;
  if (dlg_backdrop) // drop an open brightness dialog without saving
  {
    lv_obj_del(dlg_backdrop);
    dlg_backdrop = nullptr;
  }
  app_on_pc_off();

  lv_scr_load(scr_off);
  lv_refr_now(NULL);    // draw "PC is off" once, while the backlight is still on
  screen_switch(false); // ...then go dark; the panel retains the image
  peek_until = 0;
#if PC_OFF_CPU_MHZ
  setCpuFrequencyMhz(PC_OFF_CPU_MHZ);
#endif
}

static void leave_pc_off()
{
#if PC_OFF_CPU_MHZ
  setCpuFrequencyMhz(240);
#endif
  pc_off = false;
  peek_until = 0;
  app_on_pc_back(); // loads the current page, says HELLO
  dimmed = false;
  screen_switch(true);
  set_brightness(bright_full);
  lv_disp_trig_activity(NULL);
}

static void pc_off_step(uint32_t t)
{
  if (peek_until && (int32_t)(t - peek_until) >= 0)
  {
    screen_switch(false);
    peek_until = 0;
  }
  ESP_PanelTouchPoint p;
  if (touch && touch->readPoints(&p, 1) > 0)
  {
    if (!peek_until)
    {
      screen_switch(true);
      set_brightness(bright_full);
    }
    peek_until = t + PC_OFF_PEEK_MS; // a held finger keeps it lit
  }
}

bool power_tick(uint32_t t)
{
  bool host = usb_host_alive(t);
  if (pc_off)
  {
    if (!host)
    {
      pc_off_step(t);
      delay(PC_OFF_POLL_MS); // the CPU idles in between
      return true;
    }
    leave_pc_off();
    return false;
  }
  if (!host)
  {
    enter_pc_off();
    return true;
  }
  return false;
}
