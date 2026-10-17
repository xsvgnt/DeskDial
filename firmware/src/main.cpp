// DeskDial firmware for the JC3636W518 round display (ESP32-S3, 360x360
// ST77916 QSPI panel, CST816S touch). Shows what Windows is playing and/or
// the PC's hardware stats, sent by the DeskDial companion over USB.
//
// Build features: include/config.h (DESKDIAL_MEDIA, DESKDIAL_HARDWARE).
// Pages offered = features of this firmware AND of the PC companion, as
// announced in the HELLO handshake. Swipe up/down to switch when both exist.
// SPDX-License-Identifier: MIT
#include "app.h"

// ----------------------------------------------------------------- pages --
static uint8_t page = PAGE_MEDIA;

static uint8_t page_feature(uint8_t p) { return p == PAGE_HW ? FEATURE_HARDWARE : FEATURE_MEDIA; }

// Pages both ends support. Before the PC has said, or if the two have nothing
// in common (mismatched builds), the board shows its own pages.
static uint8_t available_features()
{
  uint8_t both = board_features() & link_state.pc_features;
  return both ? both : board_features();
}

static bool page_available(uint8_t p) { return available_features() & page_feature(p); }

static lv_obj_t *page_screen(uint8_t p)
{
#if DESKDIAL_HARDWARE
  if (p == PAGE_HW)
    return hardware_screen();
#endif
#if DESKDIAL_MEDIA
  if (p == PAGE_MEDIA)
    return media_screen();
#endif
  return nullptr;
}

uint8_t page_current() { return page; }

static void set_page(uint8_t p, bool animate, bool remember)
{
  if (p == page || !page_available(p))
    return;
  page = p;
#if DESKDIAL_MEDIA
  if (p != PAGE_MEDIA)
    media_on_hidden();
#endif
  lv_scr_load_anim(page_screen(p),
                   p == PAGE_HW ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_BOTTOM,
                   animate ? PAGE_ANIM_MS : 0, 0, false);
  if (remember)
    settings_save_page(p);
  link_send(MSG_PAGE, &page, 1); // the PC collects hardware data only for PAGE_HW
}

void page_refresh_available()
{
  if (page_available(page))
    return;
  set_page(page == PAGE_HW ? PAGE_MEDIA : PAGE_HW, false, false); // not the user's choice
}

static void on_gesture(lv_event_t *e)
{
  if (display_dialog_open())
    return;
  lv_indev_t *indev = lv_indev_get_act();
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  lv_indev_wait_release(indev); // a swipe never also clicks what it started on
  if (dir == LV_DIR_TOP && page == PAGE_MEDIA)
    set_page(PAGE_HW, true, true); // ignored if the page isn't available
  else if (dir == LV_DIR_BOTTOM && page == PAGE_HW)
    set_page(PAGE_MEDIA, true, true);
}

lv_obj_t *ui_new_screen()
{
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, NULL);
  return s;
}

// ------------------------------------------------------------ app hooks --
void app_fatal(const char *msg)
{
  while (true)
  {
    Serial.print("DeskDial: ");
    Serial.println(msg);
    delay(2000);
  }
}

bool app_on_frame(uint8_t type, const uint8_t *p, uint32_t len)
{
#if DESKDIAL_MEDIA
  if (media_on_frame(type, p, len))
    return true;
#endif
#if DESKDIAL_HARDWARE
  if (hardware_on_frame(type, p, len))
    return true;
#endif
  return false;
}

void app_on_link_lost()
{
#if DESKDIAL_MEDIA
  media_reset();
#endif
#if DESKDIAL_HARDWARE
  hardware_reset();
#endif
}

void app_on_pc_off()
{
#if DESKDIAL_MEDIA
  media_on_hidden();
#endif
}

void app_on_pc_back()
{
  link_reset();
  app_on_link_lost();
  lv_scr_load(page_screen(page));
  link_send_hello();
}

// ------------------------------------------------------------------ tick --
static void ui_tick(lv_timer_t *)
{
  uint32_t t = millis();
  link_tick(t);
  bool idle = false; // dims the backlight
#if DESKDIAL_MEDIA
  media_tick(t, page == PAGE_MEDIA);
  idle = page == PAGE_MEDIA && media_idle();
#endif
#if DESKDIAL_HARDWARE
  hardware_tick(t, page == PAGE_HW);
#endif
  display_tick(t, idle);
}

// ================================================================= main ==
void setup()
{
  link_begin();
  display_begin();
#if DESKDIAL_MEDIA
  media_begin();
#endif
#if DESKDIAL_HARDWARE
  hardware_begin();
#endif

  uint8_t saved = settings_page();
  page = (saved == PAGE_HW || saved == PAGE_MEDIA) && page_available(saved) ? saved
         : page_available(PAGE_MEDIA)                                       ? PAGE_MEDIA
                                                                            : PAGE_HW;
  lv_obj_t *boot = lv_scr_act(); // LVGL's default screen, unused
  lv_scr_load(page_screen(page));
  lv_obj_del(boot);

  lv_timer_create(ui_tick, 100, NULL);
  ui_tick(NULL);
  link_send_hello();
  power_begin();
}

void loop()
{
  if (power_tick(millis()))
    return; // PC off: nothing else to do
  link_poll();
  lv_timer_handler();
  delay(2);
}
