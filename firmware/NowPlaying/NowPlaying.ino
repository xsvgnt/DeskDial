// NowPlaying — Spotify "now playing" remote for the JC3636W518 (ESP32-S3,
// 360x360 round ST77916 QSPI panel, CST816S touch).
//
// Data comes from the Windows bridge over the USB serial link (protocol.h).
// Requires: ESP32_Display_Panel 0.1.4, ESP32_IO_Expander 0.0.2, LVGL 8.4.0
// with the provided lv_conf.h, and PSRAM enabled in the board settings.
//
// Everything (serial parsing, LVGL, UI state) runs in the Arduino loop task,
// so the shared state below needs no locking.

#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include "soc/usb_serial_jtag_reg.h"
#include "scr_st77916.h" // vendor driver: panel, touch, backlight, LVGL glue
#include "protocol.h"

LV_FONT_DECLARE(font_latin_14);
LV_FONT_DECLARE(font_latin_16);
LV_FONT_DECLARE(font_latin_22);
LV_FONT_DECLARE(font_icons_48);
LV_FONT_DECLARE(font_num_34);

#define ICON_MUSIC "\xEF\x80\x81" // U+F001, same code points as LV_SYMBOL_*
#define ICON_PLAY LV_SYMBOL_PLAY  // U+F04B
#define ICON_PAUSE LV_SYMBOL_PAUSE // U+F04C

// ---------------------------------------------------------------- tuning --
static const uint32_t LINK_TIMEOUT_MS = 5000;     // PC sends state every 1 s
static const uint32_t HELLO_PERIOD_MS = 2000;
static const uint32_t PAUSED_IDLE_MS = 120000;    // paused -> "Nothing is playing"
static const uint32_t DIM_AFTER_MS = 10000;
static const uint32_t TAP_FEEDBACK_MS = 700;
static const uint32_t OPTIMISTIC_HOLD_MS = 1500;  // ignore stale "playing" from PC
static const uint32_t SETTINGS_HOLD_MS = 5000;    // long press on the artwork
static const uint32_t PC_OFF_AFTER_MS = 2000;     // no USB heartbeat -> PC off/asleep
static const uint32_t PC_OFF_PEEK_MS = 5000;      // "PC is off" shown after a touch
static const uint32_t PC_OFF_POLL_MS = 100;       // loop period while dark
#define PC_OFF_CPU_MHZ 80                         // CPU clock while dark (0 = leave as is)
static const uint8_t BRIGHT_DEFAULT = 100;
static const uint8_t BRIGHT_DIM = 10;

static const uint32_t COL_GREEN = 0x1DB954;
static const uint32_t COL_TRACK = 0x202020;
static const uint32_t COL_GREY = 0xA0A0A0;
static const uint32_t COL_RAM_BAR = 0x4FC3F7;  // hardware page bars
static const uint32_t COL_VRAM_BAR = 0x9C6BFF;
static const uint32_t COL_ARC_FROM = 0x3D5AFE; // load arc gradient
static const uint32_t COL_ARC_TO = 0xFF2E93;
// Value colour scale for text: cool blue -> violet -> red-pink
static const uint32_t COL_SCALE_LO = 0x4FA8FF;
static const uint32_t COL_SCALE_MID = 0x9C6BFF;
static const uint32_t COL_SCALE_HI = 0xFF3D6E;
// Ranges mapped onto that scale (below = blue, above = red-pink)
static const float SCALE_CPU_GPU_TEMP[2] = {30, 90};  // °C
static const float SCALE_OTHER_TEMP[2] = {25, 75};    // °C, RAM modules / SSD
static const float SCALE_CPU_CLOCK[2] = {1000, 5500}; // MHz
static const float SCALE_GPU_CLOCK[2] = {200, 3000};  // MHz
static const uint32_t HW_STALE_MS = 3000;
static const uint32_t PAGE_ANIM_MS = 250;

// ---------------------------------------------------------- shared state --
struct NowPlaying
{
  bool link = false;
  bool session = false;
  bool playing = false;
  uint32_t pos_ms = 0, dur_ms = 0, pos_stamp = 0; // pos_ms valid at pos_stamp
  uint32_t paused_since = 0;
  uint32_t optimistic_until = 0;
  uint32_t art_id = 0;       // art the PC says belongs to the current track
  uint32_t shown_art_id = 0; // art currently in art_buf
  bool has_art = false;
  bool text_dirty = true;
  bool art_dirty = true;
  char title[256] = "";
  char artist[256] = "";
};
static NowPlaying now;
static uint32_t last_rx_ms = 0;
static uint32_t last_hello_ms = 0;

// --------------------------------------------------------------- buffers --
static uint8_t *art_buf = nullptr; // displayed artwork, PSRAM
static uint8_t *rx_buf = nullptr;  // frame payload, PSRAM
static lv_img_dsc_t art_dsc;

// -------------------------------------------------------------------- UI --
static lv_obj_t *arc, *art_box, *art_img, *art_placeholder, *tap_disc, *tap_icon;
static lv_obj_t *lbl_title, *lbl_artist, *lbl_time, *lbl_idle, *btn_prev, *btn_next;
static bool ui_idle = false, ui_first = true, dimmed = false;
static uint32_t idle_since = 0, tap_until = 0;
static bool art_long_fired = false; // swallow the click that ends a long press

// Brightness setting, persisted in NVS
static Preferences prefs;
static uint8_t bright_full = BRIGHT_DEFAULT;
static lv_obj_t *dlg_backdrop = nullptr, *dlg_slider, *dlg_value;

// Pages and hardware data
static uint8_t page = PAGE_MEDIA;
static lv_obj_t *scr_hw;
static float hw_val[HW_COUNT];
static uint32_t hw_has = 0;   // bit per metric present in the last frame
static uint8_t hw_temp_count = 0; // labelled extra temperatures (bottom row)
static char hw_temp_label[HW_TEMPS_MAX][HW_TEMP_LABEL_MAX + 1];
static float hw_temp_val[HW_TEMPS_MAX];
static uint32_t hw_stamp = 0; // millis() of the last frame
static bool hw_dirty = true, hw_stale_shown = false;
static void on_gesture(lv_event_t *e);

// PC presence
static lv_obj_t *scr_main, *scr_off;
static bool pc_off = false;
static uint32_t last_sof_ms = 0, last_frame = 0xFFFFFFFF, peek_until = 0;
static int32_t shown_sec = -1, shown_dur_sec = -1;
static int16_t shown_arc = -1;

// ================================================================ serial ==
static void send_frame(uint8_t type, const uint8_t *payload, uint32_t len)
{
  uint8_t hdr[7] = {NPR_SYNC0, NPR_SYNC1, type,
                    (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
  uint8_t sum = 0;
  for (int i = 2; i < 7; i++)
    sum += hdr[i];
  for (uint32_t i = 0; i < len; i++)
    sum += payload[i];
  Serial.write(hdr, sizeof hdr);
  if (len)
    Serial.write(payload, len);
  Serial.write(&sum, 1);
}

static void send_page() { send_frame(MSG_PAGE, &page, 1); }

static void send_hello()
{
  send_frame(MSG_HELLO, (const uint8_t *)"NPR1", 4);
  send_page(); // tells the PC whether to collect hardware data
  last_hello_ms = millis();
}

static void send_cmd(uint8_t cmd) { send_frame(MSG_CMD, &cmd, 1); }

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t current_pos(uint32_t t)
{
  uint32_t pos = now.pos_ms;
  if (now.playing)
    pos += t - now.pos_stamp;
  if (now.dur_ms && pos > now.dur_ms)
    pos = now.dur_ms;
  return pos;
}

static void set_playing(bool playing, uint32_t t)
{
  if (now.playing && !playing)
    now.paused_since = t;
  now.playing = playing;
}

static void copy_text(char *dst, const uint8_t *src, uint8_t n)
{
  char tmp[256];
  memcpy(tmp, src, n);
  tmp[n] = 0;
  if (strcmp(dst, tmp) != 0) // only on change: set_text restarts the scroll
  {
    strcpy(dst, tmp);
    now.text_dirty = true;
  }
}

static void handle_state(const uint8_t *p, uint32_t len)
{
  if (len < 14)
    return;
  uint32_t t = millis();
  uint8_t flags = p[0];
  bool session = flags & STATE_FLAG_SESSION;
  if (session && !now.session)
    now.paused_since = t; // a paused track that just appeared gets its 120 s
  now.session = session;

  if ((int32_t)(t - now.optimistic_until) >= 0) // not inside the tap hold-off
  {
    set_playing(flags & STATE_FLAG_PLAYING, t);
    now.pos_ms = rd32(p + 1);
    now.pos_stamp = t;
  }
  now.dur_ms = rd32(p + 5);
  now.art_id = rd32(p + 9);

  uint32_t off = 13;
  uint8_t tl = p[off++];
  if (off + tl + 1 > len)
    return;
  copy_text(now.title, p + off, tl);
  off += tl;
  uint8_t al = p[off++];
  if (off + al > len)
    return;
  copy_text(now.artist, p + off, al);
}

static void handle_frame(uint8_t type, const uint8_t *p, uint32_t len)
{
  last_rx_ms = millis();
  now.link = true;
  switch (type)
  {
  case MSG_HELLO:
    send_hello();
    break;
  case MSG_STATE:
    handle_state(p, len);
    break;
  case MSG_ART:
    if (len == 4 + ART_BYTES)
    {
      memcpy(art_buf, p + 4, ART_BYTES); // safe: rendering happens later in this task
      now.shown_art_id = rd32(p);
      now.has_art = true;
      now.art_dirty = true;
    }
    break;
  case MSG_HW:
    if (len >= 1 && len >= 1u + p[0] * 5u)
    {
      uint32_t has = 0;
      for (int i = 0; i < p[0]; i++)
      {
        const uint8_t *e = p + 1 + i * 5;
        if (e[0] >= HW_COUNT)
          continue; // newer PC software: ignore unknown metrics
        float v;
        memcpy(&v, e + 1, 4);
        hw_val[e[0]] = v;
        has |= 1u << e[0];
      }
      hw_has = has;
      hw_stamp = millis();
      hw_dirty = true;
    }
    break;
  case MSG_HW_TEMPS:
    if (len >= 1)
    {
      uint32_t off = 1;
      uint8_t n = 0;
      for (int i = 0; i < p[0] && n < HW_TEMPS_MAX; i++)
      {
        if (off + 1 > len)
          break;
        uint8_t l = p[off++];
        if (off + l + 4 > len)
          break;
        uint8_t keep = l > HW_TEMP_LABEL_MAX ? HW_TEMP_LABEL_MAX : l;
        memcpy(hw_temp_label[n], p + off, keep);
        hw_temp_label[n][keep] = 0;
        off += l;
        memcpy(&hw_temp_val[n], p + off, 4);
        off += 4;
        n++;
      }
      hw_temp_count = n;
      hw_dirty = true;
    }
    break;
  case MSG_ART_NONE:
    if (len == 4)
    {
      now.shown_art_id = rd32(p);
      now.has_art = false;
      now.art_dirty = true;
    }
    break;
  }
}

static void serial_poll()
{
  enum
  {
    S_SYNC0,
    S_SYNC1,
    S_HDR,
    S_PAYLOAD,
    S_SUM
  };
  static int st = S_SYNC0;
  static uint8_t hdr[5];
  static uint32_t hn, plen, pn;
  static uint8_t sum;
  static uint8_t chunk[512];

  int avail;
  while ((avail = Serial.available()) > 0)
  {
    size_t n = Serial.read(chunk, min(avail, (int)sizeof chunk));
    for (size_t i = 0; i < n; i++)
    {
      uint8_t b = chunk[i];
      switch (st)
      {
      case S_SYNC0:
        if (b == NPR_SYNC0)
          st = S_SYNC1;
        break;
      case S_SYNC1:
        st = (b == NPR_SYNC1) ? S_HDR : (b == NPR_SYNC0 ? S_SYNC1 : S_SYNC0);
        hn = 0;
        break;
      case S_HDR:
        hdr[hn++] = b;
        if (hn == 5)
        {
          plen = rd32(hdr + 1);
          pn = 0;
          sum = hdr[0] + hdr[1] + hdr[2] + hdr[3] + hdr[4];
          st = plen > NPR_MAX_PAYLOAD ? S_SYNC0 : (plen ? S_PAYLOAD : S_SUM);
        }
        break;
      case S_PAYLOAD:
      {
        // copy as much of this chunk as belongs to the payload
        size_t take = min((size_t)(plen - pn), n - i);
        memcpy(rx_buf + pn, chunk + i, take);
        for (size_t k = 0; k < take; k++)
          sum += chunk[i + k];
        pn += take;
        i += take - 1;
        if (pn == plen)
          st = S_SUM;
        break;
      }
      case S_SUM:
        if (b == sum)
          handle_frame(hdr[0], rx_buf, plen);
        st = S_SYNC0;
        break;
      }
    }
  }
}

// ==================================================================== UI ==
static void show(lv_obj_t *o, bool visible)
{
  if (visible)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void on_art_clicked(lv_event_t *e)
{
  if (art_long_fired)
  {
    art_long_fired = false;
    return;
  }
  if (ui_idle || dlg_backdrop)
    return;
  uint32_t t = millis();
  send_cmd(CMD_PLAY_PAUSE);
  now.pos_ms = current_pos(t); // freeze/resume the clock from here
  now.pos_stamp = t;
  set_playing(!now.playing, t); // optimistic; the next state frame corrects it
  now.optimistic_until = t + OPTIMISTIC_HOLD_MS;

  lv_label_set_text(tap_icon, now.playing ? ICON_PLAY : ICON_PAUSE); // state moved *to*
  lv_obj_clear_flag(tap_disc, LV_OBJ_FLAG_HIDDEN);
  tap_until = t + TAP_FEEDBACK_MS; // expiry handled by ui_tick
}

static void on_art_pressed(lv_event_t *e)
{
  art_long_fired = false; // a new touch starts clean, even if a release was lost
}

static void on_nav_clicked(lv_event_t *e)
{
  send_cmd((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *make_nav(lv_obj_t *parent, const char *sym, lv_coord_t x, uint8_t cmd)
{
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, 64, 64);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED); // bg_opa only, no inherited opa
  lv_obj_align(b, LV_ALIGN_CENTER, x, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b, on_nav_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)cmd);

  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, sym);
  lv_obj_center(l);
  return b;
}

// ------------------------------------------------------ brightness dialog --
static uint8_t clamp_brightness(int v)
{
  v = (v + 5) / 10 * 10; // snap to the 10 % grid
  return (uint8_t)(v < 10 ? 10 : (v > 100 ? 100 : v));
}

static void load_brightness()
{
  prefs.begin("npr", true); // read-only
  bright_full = clamp_brightness(prefs.getUChar("bright", BRIGHT_DEFAULT));
  page = prefs.getUChar("page", PAGE_MEDIA) == PAGE_HW ? PAGE_HW : PAGE_MEDIA;
  prefs.end();
}

static void save_brightness(uint8_t v)
{
  prefs.begin("npr", false);
  prefs.putUChar("bright", v);
  prefs.end();
}

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
    save_brightness(bright_full);
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
  lv_obj_set_style_bg_color(b, lv_color_hex(primary ? COL_GREEN : 0x3A3A3A), 0);
  lv_obj_add_event_cb(b, on_dlg_button, LV_EVENT_CLICKED, (void *)(uintptr_t)primary);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &font_latin_16, 0);
  lv_obj_set_style_text_color(l, primary ? lv_color_black() : lv_color_white(), 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

static void dlg_open()
{
  if (dlg_backdrop)
    return;
  show(tap_disc, false);
  tap_until = 0;

  // Full-screen backdrop on the top layer: dims the UI and swallows touches.
  // Translucency through bg_opa only (never animated opa on parents).
  dlg_backdrop = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(dlg_backdrop);
  lv_obj_set_size(dlg_backdrop, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(dlg_backdrop, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(dlg_backdrop, LV_OPA_70, 0);
  lv_obj_add_flag(dlg_backdrop, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(dlg_backdrop, LV_OBJ_FLAG_SCROLLABLE);

  // Panel fits inside the 254 px safe square
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

  lv_obj_t *title = lv_label_create(panel);
  lv_obj_set_style_text_font(title, &font_latin_22, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "Brightness");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  dlg_value = lv_label_create(panel);
  lv_obj_set_style_text_font(dlg_value, &font_latin_16, 0);
  lv_obj_set_style_text_color(dlg_value, lv_color_hex(COL_GREY), 0);
  lv_obj_align(dlg_value, LV_ALIGN_TOP_MID, 0, 48);

  dlg_slider = lv_slider_create(panel);
  lv_obj_set_size(dlg_slider, 190, 10);
  lv_obj_align(dlg_slider, LV_ALIGN_TOP_MID, 0, 92);
  lv_slider_set_range(dlg_slider, 1, 10); // x10 = 10..100 %, step 10
  lv_slider_set_value(dlg_slider, bright_full / 10, LV_ANIM_OFF);
  lv_obj_set_ext_click_area(dlg_slider, 20); // easier to grab on a small screen
  lv_obj_set_style_bg_color(dlg_slider, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dlg_slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dlg_slider, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(dlg_slider, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_pad_all(dlg_slider, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(dlg_slider, on_dlg_slider, LV_EVENT_VALUE_CHANGED, NULL);
  dlg_show_value(bright_full);

  make_dlg_button(panel, "Cancel", false, -58);
  make_dlg_button(panel, "Save", true, 58);

  set_brightness(bright_full); // in case the screen was dimmed
  dimmed = false;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            lv_coord_t w, lv_coord_t y, lv_label_long_mode_t mode)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, mode);
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

static void build_ui()
{
  lv_obj_t *scr = scr_main = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, NULL);

  // Progress arc on the rim
  arc = lv_arc_create(scr);
  lv_obj_set_size(arc, 336, 336);
  lv_obj_center(arc);
  lv_arc_set_rotation(arc, 270); // 0 at 12 o'clock
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, 1000);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(arc, 0, 0);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
  lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);

  // Album art: a clickable box holding the image, a placeholder and the tap disc
  art_box = lv_obj_create(scr);
  lv_obj_remove_style_all(art_box);
  lv_obj_set_size(art_box, ART_W, ART_H);
  lv_obj_align(art_box, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_style_bg_color(art_box, lv_color_hex(COL_TRACK), 0);
  lv_obj_set_style_bg_opa(art_box, LV_OPA_COVER, 0);
  lv_obj_clear_flag(art_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(art_box, on_art_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(art_box, on_art_pressed, LV_EVENT_PRESSED, NULL);

  memset(&art_dsc, 0, sizeof art_dsc);
  art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  art_dsc.header.always_zero = 0;
  art_dsc.header.w = ART_W;
  art_dsc.header.h = ART_H;
  art_dsc.data_size = ART_BYTES;
  art_dsc.data = art_buf;

  art_img = lv_img_create(art_box);
  lv_img_set_src(art_img, &art_dsc);
  lv_obj_set_pos(art_img, 0, 0);
  lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);

  art_placeholder = lv_label_create(art_box);
  lv_obj_set_style_text_font(art_placeholder, &font_icons_48, 0);
  lv_obj_set_style_text_color(art_placeholder, lv_color_hex(0x505050), 0);
  lv_label_set_text(art_placeholder, ICON_MUSIC);
  lv_obj_center(art_placeholder);

  tap_disc = lv_obj_create(art_box);
  lv_obj_remove_style_all(tap_disc);
  lv_obj_set_size(tap_disc, 88, 88);
  lv_obj_set_style_radius(tap_disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(tap_disc, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(tap_disc, LV_OPA_60, 0); // translucency via bg_opa only
  lv_obj_clear_flag(tap_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(tap_disc);
  lv_obj_add_flag(tap_disc, LV_OBJ_FLAG_HIDDEN);

  tap_icon = lv_label_create(tap_disc);
  lv_obj_set_style_text_font(tap_icon, &font_icons_48, 0);
  lv_obj_set_style_text_color(tap_icon, lv_color_white(), 0);
  lv_label_set_text(tap_icon, ICON_PLAY);
  lv_obj_center(tap_icon);

  // Text
  lbl_title = make_label(scr, &font_latin_22, 0xFFFFFF, 240, 230, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lbl_artist = make_label(scr, &font_latin_16, COL_GREY, 240, 262, LV_LABEL_LONG_DOT);
  lbl_time = make_label(scr, &font_latin_14, COL_GREY, 170, 290, LV_LABEL_LONG_CLIP);
  lbl_idle = make_label(scr, &font_latin_22, COL_GREY, 240, 230, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lbl_idle, "Nothing is playing");

  // Navigation
  btn_prev = make_nav(scr, LV_SYMBOL_PREV, -118, CMD_PREV);
  btn_next = make_nav(scr, LV_SYMBOL_NEXT, 118, CMD_NEXT);

  // Separate screen shown (briefly) while the PC is off
  scr_off = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_off, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr_off, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr_off, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *off = lv_label_create(scr_off);
  lv_obj_set_style_text_font(off, &font_latin_22, 0);
  lv_obj_set_style_text_color(off, lv_color_hex(COL_GREY), 0);
  lv_label_set_text(off, "PC is off");
  lv_obj_center(off);
}

// ========================================================= hardware page ==
// Layout v2: memory bars at the top, CPU and GPU load gauges side by side,
// labelled temperatures (RAM modules, SSD...) along the bottom.
static lv_obj_t *hw_cpu_arc, *hw_cpu_temp, *hw_cpu_load, *hw_cpu_clock, *hw_cpu_power;
static lv_obj_t *hw_gpu_arc, *hw_gpu_temp, *hw_gpu_load, *hw_gpu_clock, *hw_gpu_power;
static lv_obj_t *hw_ram_val, *hw_ram_bar, *hw_vram_val, *hw_vram_bar;
static lv_obj_t *hw_extra_name[HW_TEMPS_MAX], *hw_extra_val[HW_TEMPS_MAX];
static lv_img_dsc_t arc_grad_dsc;

static const lv_coord_t GAUGE = 140;  // load arc diameter
static const lv_coord_t GAUGE_W = 10; // arc width
static const lv_coord_t GAUGE_CY = 192, GAUGE_DX = 98; // centres at 180 +/- 98

static void set_page(uint8_t p, bool animate);

static void on_gesture(lv_event_t *e)
{
  if (dlg_backdrop)
    return;
  lv_indev_t *indev = lv_indev_get_act();
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_TOP && page == PAGE_MEDIA)
    set_page(PAGE_HW, true);
  else if (dir == LV_DIR_BOTTOM && page == PAGE_HW)
    set_page(PAGE_MEDIA, true);
  else
    return;
  lv_indev_wait_release(indev); // the swipe must not also click what it started on
}

static void set_page(uint8_t p, bool animate)
{
  if (p == page)
    return;
  page = p;
  show(tap_disc, false);
  tap_until = 0;
  hw_dirty = true;
  lv_scr_load_anim(p == PAGE_HW ? scr_hw : scr_main,
                   p == PAGE_HW ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_BOTTOM,
                   animate ? PAGE_ANIM_MS : 0, 0, false);
  prefs.begin("npr", false);
  prefs.putUChar("page", p);
  prefs.end();
  send_page();
  if (dimmed) // the hardware page is never dimmed
  {
    set_brightness(bright_full);
    dimmed = false;
  }
}

// Conic gradient (blue -> pink along the 270 degree sweep), used as the
// texture of the temperature arcs' indicator. Shared by both gauges.
static void make_arc_gradient()
{
  uint16_t *px = (uint16_t *)heap_caps_malloc(GAUGE * GAUGE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!px)
    return;
  lv_color_t a = lv_color_hex(COL_ARC_FROM), b = lv_color_hex(COL_ARC_TO);
  float c = (GAUGE - 1) / 2.0f;
  for (int y = 0; y < GAUGE; y++)
    for (int x = 0; x < GAUGE; x++)
    {
      float deg = atan2f(y - c, x - c) * 57.29578f; // 0 = 3 o'clock, clockwise
      float along = fmodf(deg - 135.0f + 720.0f, 360.0f) / 270.0f;
      if (along > 1) // in the bottom gap (rounded end caps): nearest end's colour
        along = along < 1.1667f ? 1 : 0;
      lv_color_t col = lv_color_mix(b, a, (uint8_t)(along * 255));
      px[y * GAUGE + x] = col.full;
    }
  memset(&arc_grad_dsc, 0, sizeof arc_grad_dsc);
  arc_grad_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  arc_grad_dsc.header.w = GAUGE;
  arc_grad_dsc.header.h = GAUGE;
  arc_grad_dsc.data_size = GAUGE * GAUGE * 2;
  arc_grad_dsc.data = (const uint8_t *)px;
}

static lv_obj_t *hw_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text,
                          lv_align_t align, lv_coord_t x, lv_coord_t y)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  lv_obj_align(l, align, x, y);
  return l;
}

/// Value -> cool blue / violet / red-pink, linear between lo and hi.
static lv_color_t scale_color(float v, float lo, float hi)
{
  float t = hi > lo ? (v - lo) / (hi - lo) : 0;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  if (t < 0.5f)
    return lv_color_mix(lv_color_hex(COL_SCALE_MID), lv_color_hex(COL_SCALE_LO), (uint8_t)(t * 2 * 255));
  return lv_color_mix(lv_color_hex(COL_SCALE_HI), lv_color_hex(COL_SCALE_MID), (uint8_t)((t - 0.5f) * 2 * 255));
}

static void set_color(lv_obj_t *l, lv_color_t c) { lv_obj_set_style_text_color(l, c, 0); }

// One load gauge: arc (0-100 %), caption, temperature, load; clock and
// power just below the arc's open ends. cx = horizontal offset from centre.
static void hw_gauge(lv_obj_t *scr, lv_coord_t cx, const char *name, lv_obj_t **arc, lv_obj_t **temp,
                     lv_obj_t **load, lv_obj_t **clock, lv_obj_t **power)
{
  lv_obj_t *a = lv_arc_create(scr);
  lv_obj_set_size(a, GAUGE, GAUGE);
  lv_obj_align(a, LV_ALIGN_TOP_MID, cx, GAUGE_CY - GAUGE / 2);
  lv_arc_set_rotation(a, 0);
  lv_arc_set_bg_angles(a, 135, 45); // 270 degree sweep, open at the bottom
  lv_arc_set_range(a, 0, 100);
  lv_arc_set_value(a, 0);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_obj_set_style_arc_width(a, GAUGE_W, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, GAUGE_W, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, lv_color_hex(COL_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, lv_color_hex(COL_ARC_FROM), LV_PART_INDICATOR); // fallback
  if (arc_grad_dsc.data)
    lv_obj_set_style_arc_img_src(a, &arc_grad_dsc, LV_PART_INDICATOR);
  *arc = a;

  lv_coord_t top = GAUGE_CY - GAUGE / 2;
  char caption[24];
  snprintf(caption, sizeof caption, "%s TEMP", name);
  hw_label(scr, &font_latin_14, COL_GREY, caption, LV_ALIGN_TOP_MID, cx, top + 26);
  *temp = hw_label(scr, &font_num_34, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, cx, top + 44);
  *load = hw_label(scr, &font_latin_14, 0xFFFFFF, "LOAD --", LV_ALIGN_TOP_MID, cx, top + 88);

  // shifted towards the screen centre so the round edge never clips them
  lv_coord_t below = top + GAUGE - 10;
  lv_coord_t shift = cx < 0 ? 8 : -6;
  *clock = hw_label(scr, &font_latin_14, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, cx - 32 + shift, below);
  *power = hw_label(scr, &font_latin_14, 0xFFFFFF, "--", LV_ALIGN_TOP_MID, cx + 32 + shift, below);
}

static lv_obj_t *hw_bar(lv_obj_t *scr, lv_coord_t y, uint32_t color)
{
  lv_obj_t *b = lv_bar_create(scr);
  lv_obj_set_size(b, 124, 5);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
  return b;
}

static void build_hw_ui()
{
  make_arc_gradient();
  scr_hw = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_hw, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr_hw, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr_hw, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr_hw, on_gesture, LV_EVENT_GESTURE, NULL);

  // Memory at the top
  hw_label(scr_hw, &font_latin_14, COL_GREY, "GPU MEMORY", LV_ALIGN_TOP_MID, 0, 14);
  hw_vram_val = hw_label(scr_hw, &font_latin_22, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, 0, 30);
  hw_vram_bar = hw_bar(scr_hw, 58, COL_VRAM_BAR);
  hw_label(scr_hw, &font_latin_14, COL_GREY, "RAM USED", LV_ALIGN_TOP_MID, 0, 70);
  hw_ram_val = hw_label(scr_hw, &font_latin_22, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, 0, 86);
  hw_ram_bar = hw_bar(scr_hw, 114, COL_RAM_BAR);

  // Load gauges side by side
  hw_gauge(scr_hw, -GAUGE_DX, "CPU", &hw_cpu_arc, &hw_cpu_temp, &hw_cpu_load, &hw_cpu_clock, &hw_cpu_power);
  hw_gauge(scr_hw, GAUGE_DX, "GPU", &hw_gpu_arc, &hw_gpu_temp, &hw_gpu_load, &hw_gpu_clock, &hw_gpu_power);

  // Labelled temperatures along the bottom (positions set when data arrives)
  for (int i = 0; i < HW_TEMPS_MAX; i++)
  {
    hw_extra_name[i] = hw_label(scr_hw, &font_latin_14, 0x707070, "", LV_ALIGN_TOP_MID, 0, 282);
    hw_extra_val[i] = hw_label(scr_hw, &font_latin_16, COL_SCALE_LO, "", LV_ALIGN_TOP_MID, 0, 299);
  }
}

static bool hw_get(uint8_t id, float *v)
{
  if (!(hw_has & (1u << id)))
    return false;
  *v = hw_val[id];
  return true;
}

static void hw_text(lv_obj_t *l, uint8_t id, const char *fmt, float scale = 1.0f)
{
  float v;
  char buf[32];
  if (hw_get(id, &v))
    snprintf(buf, sizeof buf, fmt, v * scale);
  else
    strcpy(buf, "--");
  lv_label_set_text(l, buf);
}

static void fmt_temp(char *buf, size_t n, float v)
{
  snprintf(buf, n, "%d\xC2\xB0" "C", (int)lroundf(v));
}

static void hw_gauge_update(lv_obj_t *arc, lv_obj_t *temp, lv_obj_t *load, lv_obj_t *clock, lv_obj_t *power,
                            uint8_t id_temp, uint8_t id_load, uint8_t id_clock, uint8_t id_power,
                            const float clock_scale[2])
{
  float v;
  char buf[24];
  if (hw_get(id_temp, &v))
  {
    fmt_temp(buf, sizeof buf, v);
    lv_label_set_text(temp, buf);
    set_color(temp, scale_color(v, SCALE_CPU_GPU_TEMP[0], SCALE_CPU_GPU_TEMP[1]));
  }
  else
    lv_label_set_text(temp, "--");

  if (hw_get(id_load, &v))
  {
    int pct = (int)lroundf(v);
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    snprintf(buf, sizeof buf, "LOAD %d%%", pct);
    lv_label_set_text(load, buf);
    lv_arc_set_value(arc, pct);
  }
  else
  {
    lv_label_set_text(load, "LOAD --");
    lv_arc_set_value(arc, 0);
  }

  if (hw_get(id_clock, &v))
  {
    snprintf(buf, sizeof buf, "%.2f GHz", v / 1000);
    lv_label_set_text(clock, buf);
    set_color(clock, scale_color(v, clock_scale[0], clock_scale[1]));
  }
  else
    lv_label_set_text(clock, "--");

  hw_text(power, id_power, "%.1f W");
}

static void hw_memory(lv_obj_t *val, lv_obj_t *bar, uint8_t used_id, uint8_t total_id)
{
  float used, total;
  char buf[32];
  if (!hw_get(used_id, &used))
  {
    lv_label_set_text(val, "--");
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return;
  }
  bool has_total = hw_get(total_id, &total) && total > 0;
  if (has_total)
    snprintf(buf, sizeof buf, "%.1f / %.0f GB", used, total);
  else
    snprintf(buf, sizeof buf, "%.1f GB", used);
  lv_label_set_text(val, buf);
  set_color(val, scale_color(has_total ? used / total : 0, 0, 1));
  lv_bar_set_value(bar, has_total ? (int32_t)lroundf(used * 100 / total) : 0, LV_ANIM_OFF);
}

static void hw_extras_update()
{
  static const lv_coord_t pos[HW_TEMPS_MAX + 1][HW_TEMPS_MAX] = {
      {0, 0, 0}, {0, 0, 0}, {-40, 40, 0}, {-76, 0, 76}};
  uint8_t n = hw_temp_count;
  for (int i = 0; i < HW_TEMPS_MAX; i++)
  {
    bool on = i < n;
    show(hw_extra_name[i], on);
    show(hw_extra_val[i], on);
    if (!on)
      continue;
    char buf[16];
    fmt_temp(buf, sizeof buf, hw_temp_val[i]);
    lv_label_set_text(hw_extra_name[i], hw_temp_label[i]);
    lv_label_set_text(hw_extra_val[i], buf);
    set_color(hw_extra_val[i], scale_color(hw_temp_val[i], SCALE_OTHER_TEMP[0], SCALE_OTHER_TEMP[1]));
    lv_obj_align(hw_extra_name[i], LV_ALIGN_TOP_MID, pos[n][i], 282);
    lv_obj_align(hw_extra_val[i], LV_ALIGN_TOP_MID, pos[n][i], 299);
  }
}

static void hw_refresh(uint32_t t)
{
  bool stale = !now.link || t - hw_stamp > HW_STALE_MS;
  if (stale && !hw_stale_shown)
  {
    hw_has = 0; // show "--" everywhere until fresh data arrives
    hw_temp_count = 0;
    hw_dirty = true;
  }
  hw_stale_shown = stale;
  if (!hw_dirty)
    return;
  hw_dirty = false;

  hw_memory(hw_vram_val, hw_vram_bar, HW_VRAM_USED, HW_VRAM_TOTAL);
  hw_memory(hw_ram_val, hw_ram_bar, HW_RAM_USED, HW_RAM_TOTAL);
  hw_gauge_update(hw_cpu_arc, hw_cpu_temp, hw_cpu_load, hw_cpu_clock, hw_cpu_power,
                  HW_CPU_TEMP, HW_CPU_LOAD, HW_CPU_CLOCK, HW_CPU_POWER, SCALE_CPU_CLOCK);
  hw_gauge_update(hw_gpu_arc, hw_gpu_temp, hw_gpu_load, hw_gpu_clock, hw_gpu_power,
                  HW_GPU_TEMP, HW_GPU_LOAD, HW_GPU_CLOCK, HW_GPU_POWER, SCALE_GPU_CLOCK);
  hw_extras_update();
}

static void apply_idle(bool idle)
{
  ui_idle = idle;
  show(lbl_idle, idle);
  show(lbl_title, !idle);
  show(lbl_artist, !idle);
  show(lbl_time, !idle);
  show(btn_prev, !idle);
  show(btn_next, !idle);
  show(tap_disc, false);
  tap_until = 0;
  if (idle)
    lv_obj_clear_flag(art_box, LV_OBJ_FLAG_CLICKABLE);
  else
    lv_obj_add_flag(art_box, LV_OBJ_FLAG_CLICKABLE);
  now.text_dirty = now.art_dirty = true;
  shown_sec = shown_dur_sec = -1;
  shown_arc = -1;
}

static void fmt_time(char *out, uint32_t sec)
{
  unsigned h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
  if (h)
    sprintf(out, "%u:%02u:%02u", h, m, s);
  else
    sprintf(out, "%u:%02u", (unsigned)(sec / 60), s);
}

static void ui_tick(lv_timer_t *)
{
  uint32_t t = millis();

  if (now.link && t - last_rx_ms > LINK_TIMEOUT_MS)
  {
    now.link = false;
    now.session = false;
  }
  if (!now.link && t - last_hello_ms > HELLO_PERIOD_MS)
    send_hello();

  bool idle = !now.link || !now.session ||
              (!now.playing && t - now.paused_since >= PAUSED_IDLE_MS);
  if (idle != ui_idle || ui_first)
  {
    apply_idle(idle);
    ui_first = false;
  }

  // Artwork (also hidden while idle; the placeholder takes its place)
  if (now.art_dirty)
  {
    bool art = !idle && now.has_art;
    show(art_img, art);
    show(art_placeholder, !art);
    if (art)
    {
      lv_img_cache_invalidate_src(&art_dsc);
      lv_obj_invalidate(art_img);
    }
    now.art_dirty = false;
  }

  if (!idle)
  {
    if (now.text_dirty)
    {
      lv_label_set_text(lbl_title, now.title);
      lv_label_set_text(lbl_artist, now.artist);
      now.text_dirty = false;
    }
    uint32_t pos = current_pos(t);
    int16_t a = now.dur_ms ? (int16_t)((uint64_t)pos * 1000 / now.dur_ms) : 0;
    if (a != shown_arc)
    {
      lv_arc_set_value(arc, a);
      shown_arc = a;
    }
    int32_t s = pos / 1000, ds = now.dur_ms / 1000;
    if (s != shown_sec || ds != shown_dur_sec)
    {
      char a1[16], a2[16], buf[40];
      fmt_time(a1, s);
      fmt_time(a2, ds);
      if (ds > 0)
        snprintf(buf, sizeof buf, "%s / %s", a1, a2);
      else
        buf[0] = 0; // position unknown (e.g. a browser that hasn't sent its timeline yet)
      lv_label_set_text(lbl_time, buf);
      shown_sec = s;
      shown_dur_sec = ds;
    }
  }
  else if (shown_arc != 0)
  {
    lv_arc_set_value(arc, 0);
    shown_arc = 0;
  }

  if (page == PAGE_HW)
    hw_refresh(t);

  // Long press on the artwork opens the brightness dialog
  static uint32_t art_press_start = 0;
  if (page == PAGE_MEDIA && !ui_idle && !dlg_backdrop && lv_obj_has_state(art_box, LV_STATE_PRESSED))
  {
    if (!art_press_start)
      art_press_start = t;
    else if (t - art_press_start >= SETTINGS_HOLD_MS)
    {
      art_long_fired = true; // the release must not toggle play/pause
      art_press_start = 0;
      dlg_open();
    }
  }
  else
    art_press_start = 0;

  // Tap feedback expiry
  if (tap_until && (int32_t)(t - tap_until) >= 0)
  {
    show(tap_disc, false);
    tap_until = 0;
  }

  // Backlight: dim 10 s into idle; any touch or a track restores it
  if (!idle)
    idle_since = 0;
  else if (!idle_since)
    idle_since = t;
  bool dim = idle && page == PAGE_MEDIA && !dlg_backdrop && t - idle_since >= DIM_AFTER_MS &&
             lv_disp_get_inactive_time(NULL) >= DIM_AFTER_MS;
  if (dim != dimmed)
  {
    set_brightness(dim ? BRIGHT_DIM : bright_full);
    dimmed = dim;
  }
}

// ============================================================ PC presence ==
// A powered, awake USB host sends a start-of-frame packet every 1 ms, even when
// no program has the port open. The USB Serial/JTAG peripheral latches the
// 11-bit frame number: if it stops changing, the PC is off, asleep or gone.
// While it is, the board does the minimum: backlight off, no LVGL rendering
// (the panel keeps the last image in its own RAM), slower CPU, and one cheap
// touch read every 100 ms.

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

static void enter_pc_off()
{
  pc_off = true;
  if (dlg_backdrop) // drop an open brightness dialog without saving
  {
    lv_obj_del(dlg_backdrop);
    dlg_backdrop = nullptr;
  }
  show(tap_disc, false);
  tap_until = 0;

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
  now.link = false; // anything we knew is stale; the bridge resends on HELLO
  now.session = false;
  lv_scr_load(page == PAGE_HW ? scr_hw : scr_main);
  hw_dirty = true;
  ui_first = true; // re-apply the idle/playing layout on the next tick
  dimmed = false;
  screen_switch(true);
  set_brightness(bright_full);
  lv_disp_trig_activity(NULL);
  send_hello();
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

// ================================================================= main ==
void setup()
{
  Serial.setRxBufferSize(16384);
  Serial.begin(NPR_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0); // never block when the PC isn't listening
#endif

  art_buf = (uint8_t *)heap_caps_calloc(1, ART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rx_buf = (uint8_t *)heap_caps_malloc(NPR_MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!art_buf || !rx_buf)
  {
    // PSRAM missing: enable "OPI PSRAM" in the board settings
    while (true)
    {
      Serial.println("NowPlaying: PSRAM allocation failed");
      delay(2000);
    }
  }

  load_brightness(); // saved setting, or 100 % on first boot
  scr_lvgl_init();   // vendor init switches the backlight on at 100 %
  set_brightness(bright_full);
  build_ui();
  build_hw_ui();
  if (page == PAGE_HW)
    lv_scr_load(scr_hw);
  lv_timer_create(ui_tick, 100, NULL);
  ui_tick(NULL);
  send_hello();
  last_sof_ms = millis(); // grace period while USB enumerates
}

void loop()
{
  uint32_t t = millis();
  bool host = usb_host_alive(t);
  if (pc_off)
  {
    if (!host)
    {
      pc_off_step(t);
      delay(PC_OFF_POLL_MS); // the CPU idles in between
      return;
    }
    leave_pc_off();
  }
  else if (!host)
  {
    enter_pc_off();
    return;
  }

  serial_poll();
  lv_timer_handler();
  delay(2);
}
