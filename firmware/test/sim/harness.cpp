// DeskDial firmware simulator: runs the real firmware sources on a PC against
// fake hardware (stubs/), plays the companion's side of the serial protocol
// and checks how the board reacts. Screens are dumped as raw RGB565 files
// (render.py turns them into PNG).
//
// The same tests are built once per feature set (see the Makefile):
//   full      DESKDIAL_MEDIA=1 DESKDIAL_HARDWARE=1
//   media     DESKDIAL_MEDIA=1 DESKDIAL_HARDWARE=0
//   hardware  DESKDIAL_MEDIA=0 DESKDIAL_HARDWARE=1
// SPDX-License-Identifier: MIT
#include "Arduino.h"
#include "sim_board.h"
#include "app.h"
#include <assert.h>
#include <map>
#include <string>
#include <vector>

#define CHECK(cond)                                                          \
  do                                                                         \
  {                                                                          \
    if (!(cond))                                                             \
    {                                                                        \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      exit(1);                                                               \
    }                                                                        \
  } while (0)

// ============================================================ fake board ==
FakeSerial Serial;
std::map<std::string, uint8_t> sim_nvs;
static uint32_t clk = 0;
extern "C" uint32_t millis(void) { return clk; }
void delay(uint32_t ms) { clk += ms; }

int sim_cpu_mhz = 240, sim_touch_reads = 0, sim_brightness = 100;
bool sim_bl_on = true, sim_touch_down = false;
static int sim_touch_x, sim_touch_y, sim_flushes = 0;
static ESP_PanelTouch sim_tp;
ESP_PanelTouch *touch = &sim_tp;
static uint16_t sim_fb[360 * 360];

static bool sim_sof = true; // the PC sends USB start-of-frame packets
static uint32_t sim_frozen = 0;
uint32_t sim_usb_frame()
{
  if (sim_sof)
    sim_frozen = clk & 0x7FF;
  return sim_frozen;
}
static int lit() { return sim_bl_on ? sim_brightness : 0; } // what the eye sees

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++)
      sim_fb[y * 360 + x] = (c++)->full;
  sim_flushes++;
  lv_disp_flush_ready(d);
}

static void tp_read(lv_indev_drv_t *, lv_indev_data_t *d)
{
  d->point.x = sim_touch_x;
  d->point.y = sim_touch_y;
  d->state = sim_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void scr_lvgl_init()
{
  static lv_disp_draw_buf_t db;
  static lv_color_t buf[360 * 72];
  static lv_disp_drv_t dd;
  static lv_indev_drv_t id;
  lv_init();
  lv_disp_draw_buf_init(&db, buf, NULL, 360 * 72);
  lv_disp_drv_init(&dd);
  dd.hor_res = 360;
  dd.ver_res = 360;
  dd.flush_cb = flush;
  dd.draw_buf = &db;
  lv_disp_drv_register(&dd);
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = tp_read;
  lv_indev_drv_register(&id);
}

// ================================================================ the PC ==
void setup();
void loop();
static const char *out_dir = ".";

static void run(uint32_t ms)
{
  uint32_t end = clk + ms;
  while (clk < end)
    loop();
}

static void frame(uint8_t type, const std::string &p)
{
  uint32_t n = p.size();
  uint8_t h[7] = {0xA5, 0x5A, type, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
  uint8_t s = 0;
  for (int i = 2; i < 7; i++)
    s += h[i];
  for (unsigned char c : p)
    s += c;
  for (auto b : h)
    Serial.rx.push_back(b);
  for (unsigned char c : p)
    Serial.rx.push_back(c);
  Serial.rx.push_back(s);
}

static std::string u32(uint32_t v) { return std::string{(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)}; }

static void state(bool playing, uint32_t pos, uint32_t dur, uint32_t art, std::string t, std::string a)
{
  std::string p;
  p += (char)(STATE_FLAG_SESSION | (playing ? STATE_FLAG_PLAYING : 0));
  p += u32(pos) + u32(dur) + u32(art);
  p += (char)t.size();
  p += t;
  p += (char)a.size();
  p += a;
  frame(MSG_STATE, p);
}

static void no_session()
{
  std::string p;
  p += (char)0;
  p += u32(0) + u32(0) + u32(0);
  p += (char)0;
  p += (char)0;
  frame(MSG_STATE, p);
}

static void art(uint32_t id)
{
  std::string p = u32(id);
  for (int y = 0; y < 150; y++)
    for (int x = 0; x < 150; x++)
    {
      int r = 40 + x, g = 20 + y / 2, b = 200 - y;
      bool ring = ((x - 75) * (x - 75) + (y - 75) * (y - 75)) / 300 % 2;
      if (ring)
      {
        r /= 2;
        g /= 2;
        b /= 2;
      }
      uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      p += (char)(v >> 8);
      p += (char)v;
    }
  frame(MSG_ART, p);
}

static void pc_hello(uint8_t features, bool reply)
{
  std::string p = "DD";
  p += (char)DD_PROTOCOL_VERSION;
  p += (char)features;
  p += (char)(reply ? HELLO_FLAG_REPLY : 0);
  frame(MSG_HELLO, p);
}

static void hwframe(std::vector<std::pair<uint8_t, float>> m)
{
  std::string p;
  p += (char)m.size();
  for (auto &kv : m)
  {
    p += (char)kv.first;
    char b[4];
    memcpy(b, &kv.second, 4);
    p.append(b, 4);
  }
  frame(MSG_HW, p);
}

static void temps(std::vector<std::pair<std::string, float>> m)
{
  std::string p;
  p += (char)m.size();
  for (auto &kv : m)
  {
    p += (char)kv.first.size();
    p += kv.first;
    char b[4];
    memcpy(b, &kv.second, 4);
    p.append(b, 4);
  }
  frame(MSG_HW_TEMPS, p);
}

static std::string fixtures_dir = "fixtures";
static void textimg_file(const char *name)
{
  std::string path = fixtures_dir + "/" + name;
  FILE *f = fopen(path.c_str(), "rb");
  CHECK(f);
  std::string p;
  int c;
  while ((c = fgetc(f)) != EOF)
    p += (char)c;
  fclose(f);
  frame(MSG_TEXT_IMG, p);
}

static void textimg_clear(uint8_t field)
{
  std::string p(5, '\0');
  p[0] = (char)field;
  frame(MSG_TEXT_IMG, p);
}

// Frames the board sent, decoded from Serial.tx.
struct Sent
{
  uint8_t type;
  std::vector<uint8_t> payload;
};
static std::vector<Sent> sent()
{
  std::vector<Sent> out;
  auto &t = Serial.tx;
  size_t i = 0;
  while (i + 8 <= t.size())
  {
    if (t[i] != 0xA5 || t[i + 1] != 0x5A)
    {
      i++;
      continue;
    }
    uint32_t n = t[i + 3] | (t[i + 4] << 8) | (t[i + 5] << 16) | ((uint32_t)t[i + 6] << 24);
    if (i + 8 + n > t.size())
      break;
    uint8_t sum = 0;
    for (size_t k = i + 2; k < i + 7 + n; k++)
      sum += t[k];
    CHECK(sum == t[i + 7 + n]); // the board's frames are well formed
    out.push_back({t[i + 2], std::vector<uint8_t>(t.begin() + i + 7, t.begin() + i + 7 + n)});
    i += 8 + n;
  }
  return out;
}
static int count_sent(uint8_t type)
{
  int n = 0;
  for (auto &f : sent())
    n += f.type == type;
  return n;
}
static bool sent_cmd(uint8_t c)
{
  for (auto &f : sent())
    if (f.type == MSG_CMD && f.payload.size() == 1 && f.payload[0] == c)
      return true;
  return false;
}
static int last_page_sent()
{
  int pg = -1;
  for (auto &f : sent())
    if (f.type == MSG_PAGE)
      pg = f.payload[0];
  return pg;
}
static const Sent *last_hello()
{
  static Sent h;
  bool found = false;
  for (auto &f : sent())
    if (f.type == MSG_HELLO)
    {
      h = f;
      found = true;
    }
  return found ? &h : nullptr;
}

static void dump(const char *name)
{
  std::string path = std::string(out_dir) + "/" + name + ".raw";
  FILE *f = fopen(path.c_str(), "wb");
  CHECK(f);
  fwrite(sim_fb, 2, 360 * 360, f);
  fclose(f);
  printf("  screen %s (t=%u ms, backlight %d)\n", name, clk, lit());
}

// ----------------------------------------------------------------- touch --
static void tap(int x, int y)
{
  sim_touch_x = x;
  sim_touch_y = y;
  sim_touch_down = true;
  run(120);
  sim_touch_down = false;
  run(60);
}
static void touch_briefly()
{
  sim_touch_down = true;
  run(150);
  sim_touch_down = false;
}
static void swipe(int x, int y0, int y1)
{
  sim_touch_x = x;
  sim_touch_y = y0;
  sim_touch_down = true;
  run(40);
  for (int i = 1; i <= 8; i++)
  {
    sim_touch_y = y0 + (y1 - y0) * i / 8;
    run(20);
  }
  sim_touch_down = false;
  run(400);
}

// Keeps the link alive while time passes (the PC sends once a second).
static std::string g_title = "Keep-alive";
static void live(uint32_t ms)
{
  while (ms)
  {
    uint32_t k = ms > 1000 ? 1000 : ms;
#if DESKDIAL_MEDIA
    state(true, 10000, 236000, 42, g_title, "Beyonc\xC3\xA9");
#else
    no_session(); // ignored by a hardware-only board, but it is traffic
#endif
    run(k);
    ms -= k;
  }
}

// Holds a finger for 5.3 s: opens the brightness dialog where it applies.
static void hold(int x, int y)
{
  sim_touch_x = x;
  sim_touch_y = y;
  sim_touch_down = true;
  live(5300);
  sim_touch_down = false;
  run(200);
}

// Dialog geometry: slider 10..100 % across x 85..275 at y 177.
static const int SLIDER_40 = 148, SLIDER_60 = 191, SLIDER_Y = 177;
static const int BTN_CANCEL_X = 122, BTN_SAVE_X = 238, BTN_Y = 244;

static void section(const char *name) { printf("%s\n", name); }

// ================================================================= tests ==
static int bright = 100; // the saved brightness the tests expect

static void test_boot()
{
  section("boot and handshake");
  setup();
  run(500);
  const Sent *h = last_hello();
  CHECK(h && h->payload.size() == HELLO_LEN);
  CHECK(h->payload[0] == 'D' && h->payload[1] == 'D' && h->payload[2] == DD_PROTOCOL_VERSION);
  CHECK(h->payload[3] == board_features() && !(h->payload[4] & HELLO_FLAG_REPLY));
  CHECK(last_page_sent() == (DESKDIAL_MEDIA ? PAGE_MEDIA : PAGE_HW));
  dump("01_boot");

  // A HELLO request from the PC is answered (with the reply flag); a reply is not.
  Serial.tx.clear();
  pc_hello(FEATURE_MEDIA | FEATURE_HARDWARE, false);
  run(100);
  h = last_hello();
  CHECK(h && (h->payload[4] & HELLO_FLAG_REPLY) && count_sent(MSG_PAGE) == 1);
  Serial.tx.clear();
  pc_hello(FEATURE_MEDIA | FEATURE_HARDWARE, true);
  run(100);
  CHECK(count_sent(MSG_HELLO) == 0);
  // Not DeskDial: ignored, no answer
  Serial.tx.clear();
  frame(MSG_HELLO, "NPR1");
  run(100);
  CHECK(count_sent(MSG_HELLO) == 0);
  puts("  handshake ok");
}

#if DESKDIAL_MEDIA
static const char *TITLE = "Caf\xC3\xA9 del Mar \xE2\x80\x93 \xC3\x89nergie Nocturne (Extended Remix)";

static void test_media()
{
  section("media page");
  run(10500);
  printf("  idle after boot, backlight %d\n", lit());
  CHECK(sim_brightness == BRIGHT_DIM);

  art(42);
  state(true, 83000, 236000, 42, TITLE, "Beyonc\xC3\xA9 & Ma\xC3\xAEtre Gims");
  run(300);
  CHECK(sim_brightness == 100); // a track wakes the backlight
  run(1700);
  dump("02_playing");

  Serial.tx.clear();
  tap(180, 133);
  CHECK(sent_cmd(CMD_PLAY_PAUSE));
  dump("03_tap_pause");
  state(true, 85000, 236000, 42, TITLE, "x"); // stale frame inside the hold-off must not flip back
  run(800);
  dump("04_after_tap");
  Serial.tx.clear();
  tap(180 + 118, 180);
  CHECK(sent_cmd(CMD_NEXT));
  Serial.tx.clear();
  tap(180 - 118, 180);
  CHECK(sent_cmd(CMD_PREV));
  puts("  commands ok");

  // paused for 125 s (with the PC's keep-alive frames) -> "Nothing is playing"
  for (int i = 0; i < 125; i++)
  {
    state(false, 85000, 236000, 42, TITLE, "Beyonc\xC3\xA9");
    run(1000);
  }
  dump("05_paused_idle");
  CHECK(sim_brightness == 100);
  for (int i = 0; i < 11; i++)
  {
    state(false, 85000, 236000, 42, TITLE, "Beyonc\xC3\xA9");
    run(1000);
  }
  CHECK(sim_brightness == BRIGHT_DIM);
  sim_touch_x = 180;
  sim_touch_y = 133;
  touch_briefly();
  CHECK(sim_brightness == 100);
  Serial.tx.clear();
  run(200);
  CHECK(!sent_cmd(CMD_PLAY_PAUSE)); // the artwork isn't a button while idle
  puts("  idle and dimming ok");
}

static void test_media_dialog()
{
  section("brightness dialog (media page)");
  g_title = TITLE;
  state(true, 10000, 236000, 42, TITLE, "Beyonc\xC3\xA9");
  run(1500);
  Serial.tx.clear();
  sim_touch_x = 180;
  sim_touch_y = 133;
  sim_touch_down = true;
  live(5300);
  dump("06_dialog_open");
  sim_touch_down = false;
  run(200);
  CHECK(!sent_cmd(CMD_PLAY_PAUSE)); // the long press is not a tap
  tap(SLIDER_40, SLIDER_Y);
  dump("07_dialog_40");
  CHECK(sim_brightness == 40); // live preview
  tap(BTN_CANCEL_X, BTN_Y);
  run(100);
  CHECK(sim_brightness == 100 && sim_nvs.count("bright") == 0);
  tap(180, 133);
  CHECK(sent_cmd(CMD_PLAY_PAUSE)); // taps work again after the dialog
  run(1600);
  state(true, 10000, 236000, 42, TITLE, "Beyonc\xC3\xA9");
  run(200);
  hold(180, 133);
  tap(SLIDER_60, SLIDER_Y);
  CHECK(sim_brightness == 60);
  tap(BTN_SAVE_X, BTN_Y);
  run(100);
  CHECK(sim_brightness == 60 && sim_nvs["bright"] == 60);
  bright = 60;
  dump("08_after_save");

  // dims when idle, wakes to the saved 60 %, not 100 %
  for (int i = 0; i < 12; i++)
  {
    no_session();
    run(1000);
  }
  CHECK(sim_brightness == BRIGHT_DIM);
  touch_briefly();
  CHECK(sim_brightness == 60);
  puts("  cancel, save and wake ok");
}

static void test_text_images()
{
  section("PC-rendered text (CJK, Arabic)");
  live(500);
  auto track = [&](const char *t, const char *a) {
    for (int i = 0; i < 3; i++)
    {
      state(true, 30000, 200000, 42, t, a);
      run(300);
    }
  };
  textimg_file("ja_title.bin");
  textimg_clear(TEXT_FIELD_ARTIST);
  track("?? (Racing Into The Night)", "YOASOBI");
  dump("14_ja_start");
  for (int i = 0; i < 4; i++)
  {
    state(true, 31000 + i * 1000, 200000, 42, "?? (Racing Into The Night)", "YOASOBI");
    run(1000);
  }
  dump("15_ja_scrolled");
  textimg_file("ko_title.bin");
  textimg_file("ko_artist.bin");
  track("??", "?????");
  dump("16_ko");
  textimg_file("ar_title.bin");
  textimg_file("ar_artist.bin");
  track("??? ?? ??? ?????", "???? ????");
  dump("17_ar");
  textimg_clear(TEXT_FIELD_TITLE);
  textimg_clear(TEXT_FIELD_ARTIST);
  track("Back to Latin", "Montserrat");
  dump("18_latin_again");
}

static void test_speed()
{
  section("playback speed");
  auto st = [&](uint32_t pos, int rate) {
    std::string p;
    p += (char)3;
    p += u32(pos) + u32(300000) + u32(42);
    p += (char)5;
    p += "Speed";
    p += (char)1;
    p += "x";
    if (rate)
    {
      p += (char)(rate & 0xFF);
      p += (char)(rate >> 8);
    }
    frame(MSG_STATE, p);
  };
  st(60000, 0);
  run(200);
  uint32_t a = media_debug_position();
  run(3000);
  uint32_t b = media_debug_position();
  printf("  no rate field: %u ms of media in 3 s\n", b - a);
  CHECK(b - a > 2900 && b - a < 3100);
  st(60000, 2000);
  run(200);
  a = media_debug_position();
  run(3000);
  b = media_debug_position();
  printf("  rate 2.0: %u ms of media in 3 s\n", b - a);
  CHECK(b - a > 5900 && b - a < 6100);
  st(60000, 1500);
  run(200);
  a = media_debug_position();
  run(2000);
  b = media_debug_position();
  printf("  rate 1.5: %u ms of media in 2 s\n", b - a);
  CHECK(b - a > 2900 && b - a < 3100);
}
#endif // DESKDIAL_MEDIA

static void test_pc_off()
{
  section("PC off");
  live(1500);
  CHECK(lit() == bright);
  sim_sof = false;
  Serial.tx.clear();
  for (int i = 0; i < 25; i++)
    run(100); // 2.5 s without USB frames
  printf("  dark: backlight %d, cpu %d MHz\n", lit(), sim_cpu_mhz);
  CHECK(lit() == 0 && sim_cpu_mhz == PC_OFF_CPU_MHZ);
  dump("09_pc_off_panel");
  int fl = sim_flushes, tr = sim_touch_reads;
  run(60000);
  printf("  60 s dark: %d redraws, %d touch reads, %zu bytes sent\n", sim_flushes - fl, sim_touch_reads - tr,
         Serial.tx.size());
  CHECK(sim_flushes == fl && lit() == 0 && Serial.tx.empty());
  touch_briefly();
  CHECK(lit() == bright); // a touch shows "PC is off" for 5 s
  run(4500);
  CHECK(lit() == bright);
  run(800);
  CHECK(lit() == 0);
  CHECK(sim_flushes == fl);
  sim_sof = true;
  run(300);
  const Sent *h = last_hello();
  printf("  PC back: backlight %d, cpu %d MHz, hello %d\n", lit(), sim_cpu_mhz, h != nullptr);
  CHECK(lit() == bright && sim_cpu_mhz == 240 && h && !(h->payload[4] & HELLO_FLAG_REPLY));
  live(1500);
  dump("10_back");
}

#if DESKDIAL_HARDWARE
static void hw_screens()
{
  std::vector<std::pair<uint8_t, float>> full = {
      {HW_CPU_TEMP, 54.4f}, {HW_CPU_LOAD, 26}, {HW_CPU_CLOCK, 4590}, {HW_CPU_POWER, 69.67f}, {5, 917},
      {HW_RAM_USED, 8.2f}, {HW_RAM_TOTAL, 12}, {HW_GPU_TEMP, 37}, {HW_GPU_LOAD, 22}, {HW_GPU_CLOCK, 1200},
      {HW_GPU_POWER, 47.85f}, {12, 0}, {HW_VRAM_USED, 8.2f}, {HW_VRAM_TOTAL, 12}, {HW_DISK_USED, 306},
      {HW_DISK_TOTAL, 931}, {99, 1}}; // 5 and 12 retired, 99 unknown: all ignored
  hwframe(full);
  temps({{"RAM", 51}, {"SSD", 60}});
  live(300);
  dump("11_hw");
  // colour scale: RAM 12/16 (reddish), hot CPU, idle GPU
  hwframe({{HW_CPU_TEMP, 88}, {HW_CPU_LOAD, 97}, {HW_CPU_CLOCK, 5400}, {HW_CPU_POWER, 142}, {HW_RAM_USED, 12},
           {HW_RAM_TOTAL, 16}, {HW_GPU_TEMP, 31}, {HW_GPU_LOAD, 2}, {HW_GPU_CLOCK, 210}, {HW_GPU_POWER, 9.5f},
           {HW_VRAM_USED, 1}, {HW_VRAM_TOTAL, 16}});
  temps({{"SSD", 72}, {"RAM #1", 30}});
  run(300);
  dump("11b_hw_scale");
  // partial data: a PC without power sensors and no dedicated GPU
  hwframe({{HW_CPU_TEMP, 101}, {HW_CPU_LOAD, 3}, {HW_RAM_USED, 7.9f}, {HW_RAM_TOTAL, 16}});
  run(300);
  dump("12_hw_partial");
  // stale: no hardware frames for more than 3 s -> dashes
  live(3500);
  dump("13_hw_stale");
  // the hardware page never dims, even with nothing playing
  for (int i = 0; i < 15; i++)
  {
    no_session();
    run(1000);
  }
  printf("  15 s with nothing playing: backlight %d\n", lit());
  CHECK(lit() == bright);
}

static void test_hw_dialog()
{
  section("brightness dialog (hardware page)");
  hold(180, 250); // between the gauges, on the screen itself
  dump("19_hw_dialog");
  tap(SLIDER_40, SLIDER_Y);
  CHECK(sim_brightness == 40);
  tap(BTN_CANCEL_X, BTN_Y);
  run(100);
  CHECK(sim_brightness == bright);
  puts("  long press opens the dialog");
}
#endif

#if DESKDIAL_MEDIA && DESKDIAL_HARDWARE
static void test_pages()
{
  section("pages");
  live(500);
  Serial.tx.clear();
  swipe(180, 150, 40); // starts on the artwork: must not toggle play/pause
  printf("  swipe up: page %d, saved %d\n", last_page_sent(), sim_nvs["page"]);
  CHECK(last_page_sent() == PAGE_HW && sim_nvs["page"] == PAGE_HW && !sent_cmd(CMD_PLAY_PAUSE));
  hw_screens();
  test_hw_dialog();
  Serial.tx.clear();
  swipe(180, 60, 250);
  CHECK(last_page_sent() == PAGE_MEDIA && sim_nvs["page"] == PAGE_MEDIA);
  puts("  swipe down ok");

  section("feature negotiation");
  swipe(180, 150, 40);
  CHECK(last_page_sent() == PAGE_HW);
  Serial.tx.clear();
  pc_hello(FEATURE_MEDIA, false); // a media-only companion
  run(200);
  printf("  media-only PC: page %d, saved %d\n", last_page_sent(), sim_nvs["page"]);
  CHECK(last_page_sent() == PAGE_MEDIA && sim_nvs["page"] == PAGE_HW); // forced, not saved
  Serial.tx.clear();
  swipe(180, 150, 40);
  CHECK(count_sent(MSG_PAGE) == 0); // no hardware page to go to
  pc_hello(FEATURE_HARDWARE, false); // a hardware-only companion
  run(200);
  CHECK(last_page_sent() == PAGE_HW);
  Serial.tx.clear();
  swipe(180, 60, 250);
  CHECK(count_sent(MSG_PAGE) == 0);
  pc_hello(FEATURE_MEDIA | FEATURE_HARDWARE, false);
  run(200);
  swipe(180, 60, 250);
  CHECK(last_page_sent() == PAGE_MEDIA);
  puts("  pages follow the PC's features");
}
#endif

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 1)
    out_dir = argv[1];
  if (argc > 2)
    fixtures_dir = argv[2];
  printf("DeskDial simulator: media=%d hardware=%d\n", DESKDIAL_MEDIA, DESKDIAL_HARDWARE);

  test_boot();
#if DESKDIAL_MEDIA
  test_media();
  test_media_dialog();
  test_pc_off();
#if DESKDIAL_HARDWARE
  test_pages();
#else
  section("pages");
  Serial.tx.clear();
  swipe(180, 150, 40);
  CHECK(count_sent(MSG_PAGE) == 0 && !sent_cmd(CMD_PLAY_PAUSE)); // only one page
  puts("  single page ok");
#endif
  test_text_images();
  test_speed();
#else
  section("hardware page");
  CHECK(lit() == 100);
  hw_screens();
  test_hw_dialog();
  hold(180, 250);
  tap(SLIDER_60, SLIDER_Y);
  tap(BTN_SAVE_X, BTN_Y);
  run(100);
  CHECK(sim_nvs["bright"] == 60);
  bright = 60;
  Serial.tx.clear();
  swipe(180, 60, 250);
  CHECK(count_sent(MSG_PAGE) == 0); // only one page
  test_pc_off();
  CHECK(last_page_sent() == PAGE_HW);
#endif
  puts("ALL OK");
  return 0;
}
