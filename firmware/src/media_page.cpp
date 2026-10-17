// "Now playing" page: progress arc, artwork, title, artist, time,
// previous/next; tap the artwork for play/pause, hold it for brightness.
// SPDX-License-Identifier: MIT
#include "app.h"
#if DESKDIAL_MEDIA
#include <esp_heap_caps.h>

#define ICON_MUSIC "\xEF\x80\x81"  // U+F001, same code points as LV_SYMBOL_*
#define ICON_PLAY LV_SYMBOL_PLAY   // U+F04B
#define ICON_PAUSE LV_SYMBOL_PAUSE // U+F04C

// ------------------------------------------------------------------ state --
struct NowPlaying
{
  bool session = false;
  bool playing = false;
  uint32_t pos_ms = 0, dur_ms = 0, pos_stamp = 0; // pos_ms valid at pos_stamp
  uint16_t rate_milli = 1000;                       // playback speed x1000
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

static uint8_t *art_buf = nullptr; // displayed artwork, PSRAM
static lv_img_dsc_t art_dsc;

static lv_obj_t *scr, *arc, *art_box, *art_img, *art_placeholder, *tap_disc, *tap_icon;
static lv_obj_t *lbl_title, *lbl_artist, *lbl_time, *lbl_idle, *btn_prev, *btn_next;

// Title/artist rendered by the PC (scripts the board's fonts lack). Each field
// has a clipping box; the title box holds two copies of the image for a
// seamless scroll, like the label's circular mode.
static const lv_coord_t TEXT_BOX_W = 240;
static const lv_coord_t MARQUEE_GAP = 40;
static const uint8_t TEXT_IMG_MAX_H = TEXT_IMG_TITLE_H;
static uint8_t *text_img_buf[2];
static lv_img_dsc_t text_img_dsc[2];
static bool text_img_on[2];
static lv_obj_t *text_box[2], *text_img[2], *title_img_copy;

static bool ui_idle = false, ui_first = true;
static uint32_t tap_until = 0;
static LongPress art_hold;
static int32_t shown_sec = -1, shown_dur_sec = -1;
static int16_t shown_arc = -1;

// ================================================================= frames ==
static void send_cmd(uint8_t cmd) { link_send(MSG_CMD, &cmd, 1); }

static uint32_t current_pos(uint32_t t)
{
  uint32_t pos = now.pos_ms;
  if (now.playing) // count at the player's speed between updates from the PC
    pos += (uint32_t)((uint64_t)(t - now.pos_stamp) * now.rate_milli / 1000);
  if (now.dur_ms && pos > now.dur_ms)
    pos = now.dur_ms;
  return pos;
}

#ifdef DESKDIAL_SIM
uint32_t media_debug_position() { return current_pos(millis()); }
#endif

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
  off += al;
  uint16_t rate = off + 2 <= len ? (uint16_t)(p[off] | (p[off + 1] << 8)) : 1000;
  if (rate != now.rate_milli && now.playing)
  {
    // re-anchor so the elapsed part keeps the old speed
    now.pos_ms = current_pos(t);
    now.pos_stamp = t;
  }
  now.rate_milli = rate ? rate : 1000;
}

static void handle_text_img(const uint8_t *p, uint32_t len)
{
  if (len < 5 || p[0] > TEXT_FIELD_ARTIST)
    return;
  uint8_t f = p[0];
  uint16_t w = p[1] | (p[2] << 8), h = p[3] | (p[4] << 8);
  uint32_t bytes = (uint32_t)((w + 1) / 2) * h;
  uint16_t max_w = f == TEXT_FIELD_TITLE ? TEXT_IMG_TITLE_MAX_W : TEXT_IMG_ARTIST_MAX_W;
  if (w == 0)
    text_img_on[f] = false;
  else if (w <= max_w && h <= TEXT_IMG_MAX_H && len == 5 + bytes)
  {
    memcpy(text_img_buf[f], p + 5, bytes); // safe: rendering happens later in this task
    text_img_dsc[f].header.w = w;
    text_img_dsc[f].header.h = h;
    text_img_dsc[f].data_size = bytes;
    text_img_on[f] = true;
  }
  now.text_dirty = true;
}

bool media_on_frame(uint8_t type, const uint8_t *p, uint32_t len)
{
  switch (type)
  {
  case MSG_STATE:
    handle_state(p, len);
    return true;
  case MSG_ART:
    if (len == 4 + ART_BYTES)
    {
      memcpy(art_buf, p + 4, ART_BYTES); // safe: rendering happens later in this task
      now.shown_art_id = rd32(p);
      now.has_art = true;
      now.art_dirty = true;
    }
    return true;
  case MSG_ART_NONE:
    if (len == 4)
    {
      now.shown_art_id = rd32(p);
      now.has_art = false;
      now.art_dirty = true;
    }
    return true;
  case MSG_TEXT_IMG:
    handle_text_img(p, len);
    return true;
  }
  return false;
}

void media_reset()
{
  now.session = false; // anything we knew is stale; the PC resends on HELLO
  ui_first = true;     // re-apply the idle/playing layout on the next tick
}

bool media_idle()
{
  return !link_state.up || !now.session ||
         (!now.playing && millis() - now.paused_since >= PAUSED_IDLE_MS);
}

// ================================================================== touch ==
static void hide_tap_feedback()
{
  ui_show(tap_disc, false);
  tap_until = 0;
}

void media_on_hidden() { hide_tap_feedback(); }

static void on_art_clicked(lv_event_t *e)
{
  if (art_hold.fired) // end of a long press: not a tap
  {
    art_hold.fired = false;
    return;
  }
  if (ui_idle || display_dialog_open())
    return;
  uint32_t t = millis();
  send_cmd(CMD_PLAY_PAUSE);
  now.pos_ms = current_pos(t); // freeze/resume the clock from here
  now.pos_stamp = t;
  set_playing(!now.playing, t); // optimistic; the next state frame corrects it
  now.optimistic_until = t + OPTIMISTIC_HOLD_MS;

  lv_label_set_text(tap_icon, now.playing ? ICON_PLAY : ICON_PAUSE); // state moved *to*
  ui_show(tap_disc, true);
  tap_until = t + TAP_FEEDBACK_MS; // expiry handled by media_tick
}

static void on_art_pressed(lv_event_t *e)
{
  art_hold.fired = false; // a new touch starts clean, even if a release was lost
}

static void on_nav_clicked(lv_event_t *e)
{
  send_cmd((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

// ================================================================= layout ==
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
  lv_obj_t *l = ui_label(parent, font, color, "", LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, mode);
  return l;
}

// ------------------------------------------------------------ text images --
// Box placed so the image's baseline row lands on the label's baseline.
static void make_text_box(uint8_t f, lv_coord_t label_y, const lv_font_t *font,
                          lv_coord_t h, lv_coord_t baseline, uint32_t color)
{
  lv_coord_t label_baseline = label_y + font->line_height - font->base_line;
  lv_obj_t *box = lv_obj_create(scr);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, TEXT_BOX_W, h);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, label_baseline - baseline);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
  text_box[f] = box;

  memset(&text_img_dsc[f], 0, sizeof(lv_img_dsc_t));
  text_img_dsc[f].header.cf = LV_IMG_CF_ALPHA_4BIT; // colour comes from img_recolor
  text_img_dsc[f].data = text_img_buf[f];

  int copies = f == TEXT_FIELD_TITLE ? 2 : 1;
  for (int i = 0; i < copies; i++)
  {
    lv_obj_t *img = lv_img_create(box);
    lv_obj_set_style_img_recolor(img, lv_color_hex(color), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0); // LVGL ignores the colour at opa 0
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    if (i == 0)
      text_img[f] = img;
    else
      title_img_copy = img;
  }
}

static void marquee_cb(void *var, int32_t x)
{
  lv_obj_set_x(text_img[TEXT_FIELD_TITLE], x);
  lv_obj_set_x(title_img_copy, x + text_img_dsc[TEXT_FIELD_TITLE].header.w + MARQUEE_GAP);
}

static void apply_text_img(uint8_t f)
{
  lv_obj_t *img = text_img[f];
  if (f == TEXT_FIELD_TITLE)
  {
    lv_anim_del(img, marquee_cb);
    ui_show(title_img_copy, false);
  }
  if (!text_img_on[f])
  {
    ui_show(img, false);
    return;
  }
  lv_img_set_src(img, &text_img_dsc[f]);
  ui_show(img, true);
  lv_coord_t w = text_img_dsc[f].header.w;
  if (w <= TEXT_BOX_W || f != TEXT_FIELD_TITLE)
  {
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0); // fits: centred, like the label
    return;
  }
  // Too wide: scroll left forever; the copy follows one gap behind, so the
  // jump from -span back to 0 is invisible.
  lv_img_set_src(title_img_copy, &text_img_dsc[f]);
  ui_show(title_img_copy, true);
  lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_align(title_img_copy, LV_ALIGN_TOP_LEFT, 0, 0);
  int32_t span = w + MARQUEE_GAP;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, img);
  lv_anim_set_exec_cb(&a, marquee_cb);
  lv_anim_set_values(&a, 0, -span);
  lv_anim_set_time(&a, span * 1000 / (lv_disp_get_dpi(NULL) / 3)); // same speed as a scrolling label
  lv_anim_set_delay(&a, 1000);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
  marquee_cb(img, 0);
}

// Shows each field as either its label or its PC-rendered image.
static void show_track_text(bool visible)
{
  lv_obj_t *labels[2] = {lbl_title, lbl_artist};
  for (int f = 0; f < 2; f++)
  {
    ui_show(labels[f], visible && !text_img_on[f]);
    ui_show(text_box[f], visible && text_img_on[f]);
  }
}

static void build_ui()
{
  scr = ui_new_screen();

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
  lv_obj_set_style_arc_color(arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

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

  art_placeholder = ui_label(art_box, &font_icons_48, 0x505050, ICON_MUSIC, LV_ALIGN_CENTER, 0, 0);

  tap_disc = lv_obj_create(art_box);
  lv_obj_remove_style_all(tap_disc);
  lv_obj_set_size(tap_disc, 88, 88);
  lv_obj_set_style_radius(tap_disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(tap_disc, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(tap_disc, LV_OPA_60, 0); // translucency via bg_opa only
  lv_obj_clear_flag(tap_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(tap_disc);
  lv_obj_add_flag(tap_disc, LV_OBJ_FLAG_HIDDEN);
  tap_icon = ui_label(tap_disc, &font_icons_48, 0xFFFFFF, ICON_PLAY, LV_ALIGN_CENTER, 0, 0);

  // Text
  lbl_title = make_label(scr, &font_latin_22, 0xFFFFFF, 240, 230, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lbl_artist = make_label(scr, &font_latin_16, COL_GREY, 240, 262, LV_LABEL_LONG_DOT);
  make_text_box(TEXT_FIELD_TITLE, 230, &font_latin_22, TEXT_IMG_TITLE_H, TEXT_IMG_TITLE_BASELINE, 0xFFFFFF);
  make_text_box(TEXT_FIELD_ARTIST, 262, &font_latin_16, TEXT_IMG_ARTIST_H, TEXT_IMG_ARTIST_BASELINE, COL_GREY);
  lbl_time = make_label(scr, &font_latin_14, COL_GREY, 170, 290, LV_LABEL_LONG_CLIP);
  lbl_idle = make_label(scr, &font_latin_22, COL_GREY, 240, 230, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lbl_idle, "Nothing is playing");

  // Navigation
  btn_prev = make_nav(scr, LV_SYMBOL_PREV, -118, CMD_PREV);
  btn_next = make_nav(scr, LV_SYMBOL_NEXT, 118, CMD_NEXT);
}

void media_begin()
{
  art_buf = (uint8_t *)heap_caps_calloc(1, ART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  for (int f = 0; f < 2; f++)
    text_img_buf[f] = (uint8_t *)heap_caps_malloc((TEXT_IMG_TITLE_MAX_W + 1) / 2 * TEXT_IMG_MAX_H,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!art_buf || !text_img_buf[0] || !text_img_buf[1])
    app_fatal("PSRAM allocation failed: enable OPI PSRAM");
  build_ui();
}

lv_obj_t *media_screen() { return scr; }

// ================================================================= update ==
static void apply_idle(bool idle)
{
  ui_idle = idle;
  ui_show(lbl_idle, idle);
  show_track_text(!idle);
  ui_show(lbl_time, !idle);
  ui_show(btn_prev, !idle);
  ui_show(btn_next, !idle);
  hide_tap_feedback();
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

static void update_progress(uint32_t t)
{
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

// Every 100 ms, visible or not, so the page is current when swiped to.
void media_tick(uint32_t t, bool visible)
{
  bool idle = media_idle();
  if (idle != ui_idle || ui_first)
  {
    apply_idle(idle);
    ui_first = false;
  }

  // Artwork (also hidden while idle; the placeholder takes its place)
  if (now.art_dirty)
  {
    bool art = !idle && now.has_art;
    ui_show(art_img, art);
    ui_show(art_placeholder, !art);
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
      apply_text_img(TEXT_FIELD_TITLE);
      apply_text_img(TEXT_FIELD_ARTIST);
      show_track_text(true);
      now.text_dirty = false;
    }
    update_progress(t);
  }
  else if (shown_arc != 0)
  {
    lv_arc_set_value(arc, 0);
    shown_arc = 0;
  }

  // Long press on the artwork opens the brightness dialog
  if (long_press_tick(art_hold, art_box, t, visible && !idle && !display_dialog_open()))
  {
    hide_tap_feedback();
    display_open_brightness_dialog();
  }

  if (tap_until && (int32_t)(t - tap_until) >= 0)
    hide_tap_feedback();
}

#endif // DESKDIAL_MEDIA
