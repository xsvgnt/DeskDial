// Hardware page: memory bars at the top, CPU and GPU load gauges side by side,
// labelled temperatures (RAM modules, SSD...) along the bottom. Hold anywhere
// for 5 s to open the brightness dialog.
// SPDX-License-Identifier: MIT
#include "app.h"
#if DESKDIAL_HARDWARE
#include <math.h>
#include <esp_heap_caps.h>

static const uint32_t COL_RAM_BAR = 0x4FC3F7;
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

static const lv_coord_t GAUGE = 140;                   // load arc diameter
static const lv_coord_t GAUGE_W = 10;                  // arc width
static const lv_coord_t GAUGE_CY = 192, GAUGE_DX = 98; // centres at 180 +/- 98

// ------------------------------------------------------------------ state --
static float hw_val[HW_COUNT];
static uint32_t hw_has = 0;       // bit per metric present in the last frame
static uint8_t hw_temp_count = 0; // labelled extra temperatures (bottom row)
static char hw_temp_label[HW_TEMPS_MAX][HW_TEMP_LABEL_MAX + 1];
static float hw_temp_val[HW_TEMPS_MAX];
static uint32_t hw_stamp = 0; // millis() of the last frame
static bool hw_dirty = true, hw_stale_shown = false, was_visible = false;
static LongPress screen_hold;

struct Gauge
{
  lv_obj_t *arc, *temp, *load, *clock, *power;
};
static lv_obj_t *scr;
static Gauge cpu, gpu;
static lv_obj_t *ram_val, *ram_bar, *vram_val, *vram_bar;
static lv_obj_t *extra_name[HW_TEMPS_MAX], *extra_val[HW_TEMPS_MAX];
static lv_img_dsc_t arc_grad_dsc;

// ================================================================= frames ==
static void handle_hw(const uint8_t *p, uint32_t len)
{
  if (len < 1 || len < 1u + p[0] * 5u)
    return;
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

static void handle_temps(const uint8_t *p, uint32_t len)
{
  if (len < 1)
    return;
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

bool hardware_on_frame(uint8_t type, const uint8_t *p, uint32_t len)
{
  switch (type)
  {
  case MSG_HW:
    handle_hw(p, len);
    return true;
  case MSG_HW_TEMPS:
    handle_temps(p, len);
    return true;
  }
  return false;
}

void hardware_reset() { hw_dirty = true; }

// ================================================================= layout ==
// Conic gradient (blue -> pink along the 270 degree sweep), used as the
// texture of the load arcs' indicator. Shared by both gauges.
static void make_arc_gradient()
{
  uint16_t *px = (uint16_t *)heap_caps_malloc(GAUGE * GAUGE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!px)
    return; // plain colour instead
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
static Gauge make_gauge(lv_coord_t cx, const char *name)
{
  Gauge g;
  lv_obj_t *a = g.arc = lv_arc_create(scr);
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

  lv_coord_t top = GAUGE_CY - GAUGE / 2;
  char caption[24];
  snprintf(caption, sizeof caption, "%s TEMP", name);
  ui_label(scr, &font_latin_14, COL_GREY, caption, LV_ALIGN_TOP_MID, cx, top + 26);
  g.temp = ui_label(scr, &font_num_34, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, cx, top + 44);
  g.load = ui_label(scr, &font_latin_14, 0xFFFFFF, "LOAD --", LV_ALIGN_TOP_MID, cx, top + 88);

  // shifted towards the screen centre so the round edge never clips them
  lv_coord_t below = top + GAUGE - 10;
  lv_coord_t shift = cx < 0 ? 8 : -6;
  g.clock = ui_label(scr, &font_latin_14, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, cx - 32 + shift, below);
  g.power = ui_label(scr, &font_latin_14, 0xFFFFFF, "--", LV_ALIGN_TOP_MID, cx + 32 + shift, below);
  return g;
}

static lv_obj_t *make_bar(lv_coord_t y, uint32_t color)
{
  lv_obj_t *b = lv_bar_create(scr);
  lv_obj_set_size(b, 124, 5);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
  lv_bar_set_range(b, 0, 100);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE); // touches go to the screen (long press)
  lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
  return b;
}

void hardware_begin()
{
  make_arc_gradient();
  scr = ui_new_screen();

  // Memory at the top
  ui_label(scr, &font_latin_14, COL_GREY, "GPU MEMORY", LV_ALIGN_TOP_MID, 0, 14);
  vram_val = ui_label(scr, &font_latin_22, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, 0, 30);
  vram_bar = make_bar(58, COL_VRAM_BAR);
  ui_label(scr, &font_latin_14, COL_GREY, "RAM USED", LV_ALIGN_TOP_MID, 0, 70);
  ram_val = ui_label(scr, &font_latin_22, COL_SCALE_LO, "--", LV_ALIGN_TOP_MID, 0, 86);
  ram_bar = make_bar(114, COL_RAM_BAR);

  // Load gauges side by side
  cpu = make_gauge(-GAUGE_DX, "CPU");
  gpu = make_gauge(GAUGE_DX, "GPU");

  // Labelled temperatures along the bottom (positions set when data arrives)
  for (int i = 0; i < HW_TEMPS_MAX; i++)
  {
    extra_name[i] = ui_label(scr, &font_latin_14, 0x707070, "", LV_ALIGN_TOP_MID, 0, 282);
    extra_val[i] = ui_label(scr, &font_latin_16, COL_SCALE_LO, "", LV_ALIGN_TOP_MID, 0, 299);
  }
}

lv_obj_t *hardware_screen() { return scr; }

// ================================================================= update ==
static bool hw_get(uint8_t id, float *v)
{
  if (!(hw_has & (1u << id)))
    return false;
  *v = hw_val[id];
  return true;
}

static void fmt_temp(char *buf, size_t n, float v)
{
  snprintf(buf, n, "%d\xC2\xB0" "C", (int)lroundf(v)); // degree sign in UTF-8
}

static void update_gauge(const Gauge &g, uint8_t id_temp, uint8_t id_load, uint8_t id_clock, uint8_t id_power,
                         const float clock_scale[2])
{
  float v;
  char buf[24];
  if (hw_get(id_temp, &v))
  {
    fmt_temp(buf, sizeof buf, v);
    lv_label_set_text(g.temp, buf);
    set_color(g.temp, scale_color(v, SCALE_CPU_GPU_TEMP[0], SCALE_CPU_GPU_TEMP[1]));
  }
  else
    lv_label_set_text(g.temp, "--");

  if (hw_get(id_load, &v))
  {
    int pct = (int)lroundf(v);
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    snprintf(buf, sizeof buf, "LOAD %d%%", pct);
    lv_label_set_text(g.load, buf);
    lv_arc_set_value(g.arc, pct);
  }
  else
  {
    lv_label_set_text(g.load, "LOAD --");
    lv_arc_set_value(g.arc, 0);
  }

  if (hw_get(id_clock, &v))
  {
    snprintf(buf, sizeof buf, "%.2f GHz", v / 1000);
    lv_label_set_text(g.clock, buf);
    set_color(g.clock, scale_color(v, clock_scale[0], clock_scale[1]));
  }
  else
    lv_label_set_text(g.clock, "--");

  if (hw_get(id_power, &v))
  {
    snprintf(buf, sizeof buf, "%.1f W", v); // lv_label_set_text_fmt has no %f
    lv_label_set_text(g.power, buf);
  }
  else
    lv_label_set_text(g.power, "--");
}

static void update_memory(lv_obj_t *val, lv_obj_t *bar, uint8_t used_id, uint8_t total_id)
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

static void update_extras()
{
  static const lv_coord_t pos[HW_TEMPS_MAX + 1][HW_TEMPS_MAX] = {
      {0, 0, 0}, {0, 0, 0}, {-40, 40, 0}, {-76, 0, 76}};
  uint8_t n = hw_temp_count;
  for (int i = 0; i < HW_TEMPS_MAX; i++)
  {
    bool on = i < n;
    ui_show(extra_name[i], on);
    ui_show(extra_val[i], on);
    if (!on)
      continue;
    char buf[16];
    fmt_temp(buf, sizeof buf, hw_temp_val[i]);
    lv_label_set_text(extra_name[i], hw_temp_label[i]);
    lv_label_set_text(extra_val[i], buf);
    set_color(extra_val[i], scale_color(hw_temp_val[i], SCALE_OTHER_TEMP[0], SCALE_OTHER_TEMP[1]));
    lv_obj_align(extra_name[i], LV_ALIGN_TOP_MID, pos[n][i], 282);
    lv_obj_align(extra_val[i], LV_ALIGN_TOP_MID, pos[n][i], 299);
  }
}

// Every 100 ms; the labels are only touched while the page is visible (the
// PC only sends data then anyway).
void hardware_tick(uint32_t t, bool visible)
{
  if (long_press_tick(screen_hold, scr, t, visible && !display_dialog_open()))
    display_open_brightness_dialog();

  if (!visible)
  {
    was_visible = false;
    return;
  }
  if (!was_visible)
    hw_dirty = true;
  was_visible = true;

  bool stale = !link_state.up || t - hw_stamp > HW_STALE_MS;
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

  update_memory(vram_val, vram_bar, HW_VRAM_USED, HW_VRAM_TOTAL);
  update_memory(ram_val, ram_bar, HW_RAM_USED, HW_RAM_TOTAL);
  update_gauge(cpu, HW_CPU_TEMP, HW_CPU_LOAD, HW_CPU_CLOCK, HW_CPU_POWER, SCALE_CPU_CLOCK);
  update_gauge(gpu, HW_GPU_TEMP, HW_GPU_LOAD, HW_GPU_CLOCK, HW_GPU_POWER, SCALE_GPU_CLOCK);
  update_extras();
}

#endif // DESKDIAL_HARDWARE
