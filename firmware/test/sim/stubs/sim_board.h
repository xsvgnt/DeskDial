// Simulator replacement for the vendor board driver (scr_st77916.h):
// LVGL renders into sim_fb, touch comes from the test, backlight is a number.
#pragma once
#include <lvgl.h>
#define SCREEN_RES_HOR 360
#define SCREEN_RES_VER 360
extern int sim_brightness;
extern bool sim_bl_on, sim_touch_down;
extern int sim_touch_reads;
void scr_lvgl_init();
static inline void set_brightness(uint8_t b) { sim_brightness = b; }
static inline void screen_switch(bool on) { sim_bl_on = on; }
struct ESP_PanelTouchPoint
{
  int x, y;
};
struct ESP_PanelTouch
{
  int readPoints(ESP_PanelTouchPoint *, int)
  {
    sim_touch_reads++;
    return sim_touch_down ? 1 : 0;
  }
};
extern ESP_PanelTouch *touch;
