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
#include <esp_heap_caps.h>
#include "scr_st77916.h" // vendor driver: panel, touch, backlight, LVGL glue
#include "protocol.h"

LV_FONT_DECLARE(font_latin_14);
LV_FONT_DECLARE(font_latin_16);
LV_FONT_DECLARE(font_latin_22);
LV_FONT_DECLARE(font_icons_48);

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
static const uint8_t BRIGHT_FULL = 100;
static const uint8_t BRIGHT_DIM = 10;

static const uint32_t COL_GREEN = 0x1DB954;
static const uint32_t COL_TRACK = 0x202020;
static const uint32_t COL_GREY = 0xA0A0A0;

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

static void send_hello()
{
  send_frame(MSG_HELLO, (const uint8_t *)"NPR1", 4);
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
static void on_art_clicked(lv_event_t *e)
{
  if (ui_idle)
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
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

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
}

static void show(lv_obj_t *o, bool visible)
{
  if (visible)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
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
        snprintf(buf, sizeof buf, "%s", a1); // player reports no duration
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
  bool dim = idle && t - idle_since >= DIM_AFTER_MS &&
             lv_disp_get_inactive_time(NULL) >= DIM_AFTER_MS;
  if (dim != dimmed)
  {
    set_brightness(dim ? BRIGHT_DIM : BRIGHT_FULL);
    dimmed = dim;
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

  scr_lvgl_init();
  build_ui();
  lv_timer_create(ui_tick, 100, NULL);
  ui_tick(NULL);
  send_hello();
}

void loop()
{
  serial_poll();
  lv_timer_handler();
  delay(2);
}
