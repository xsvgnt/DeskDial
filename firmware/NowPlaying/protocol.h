// Serial link protocol shared with the Windows bridge (NowPlayingBridge).
//
// Every frame, in both directions:
//   0xA5 0x5A | type (u8) | length (u32 LE) | payload | checksum (u8)
// checksum = low byte of the sum of type, the 4 length bytes and the payload.
//
// PC -> ESP32
//   MSG_HELLO     "NPR1"                     probe / resync request
//   MSG_ART       art_id u32, 150*150 RGB565 big-endian (LV_COLOR_16_SWAP=1)
//   MSG_ART_NONE  art_id u32                 track has no artwork
//   MSG_STATE     flags u8 (bit0 session, bit1 playing), position_ms u32,
//                 duration_ms u32, art_id u32,
//                 title_len u8, title UTF-8, artist_len u8, artist UTF-8
//   MSG_HW        count u8, then count x (metric id u8, value float32 LE);
//                 metrics the PC can't read are simply absent. Sent once a
//                 second, only while the board shows the hardware page.
//   MSG_HW_TEMPS  count u8, then count x (label_len u8, label UTF-8,
//                 value float32 LE): extra labelled temperatures (RAM
//                 modules, SSD...), sent right after MSG_HW. Max HW_TEMPS_MAX.
// ESP32 -> PC
//   MSG_HELLO     "NPR1"                     at boot, every 2 s while unlinked,
//                                            and in reply to the PC's HELLO
//   MSG_CMD       command u8
//   MSG_PAGE      page u8 (PAGE_*), after every HELLO and on each change
#pragma once
#include <stdint.h>

#define NPR_SYNC0 0xA5
#define NPR_SYNC1 0x5A
#define NPR_BAUD 921600 // ignored by native USB CDC, used by a USB-UART bridge

#define ART_W 150
#define ART_H 150
#define ART_BYTES (ART_W * ART_H * 2)
#define NPR_MAX_PAYLOAD (4 + ART_BYTES)

enum : uint8_t
{
  MSG_HELLO = 0x01,
  MSG_STATE = 0x02,
  MSG_ART = 0x03,
  MSG_ART_NONE = 0x04,
  MSG_HW = 0x05,
  MSG_HW_TEMPS = 0x06,
  MSG_CMD = 0x10,
  MSG_PAGE = 0x11,
};

enum : uint8_t
{
  PAGE_MEDIA = 0,
  PAGE_HW = 1,
};

// Hardware metrics. Units: temperatures °C, loads %, clocks MHz, power W,
// fans RPM, memory and disk GB. New ids can be added without breaking
// older firmware (unknown ids are ignored).
enum : uint8_t
{
  HW_CPU_TEMP = 1,
  HW_CPU_LOAD = 2,
  HW_CPU_CLOCK = 3,
  HW_CPU_POWER = 4,
  HW_CPU_FAN = 5,     // not displayed (layout v2), no longer sent
  HW_RAM_USED = 6,
  HW_RAM_TOTAL = 7,
  HW_GPU_TEMP = 8,
  HW_GPU_LOAD = 9,
  HW_GPU_CLOCK = 10,
  HW_GPU_POWER = 11,
  HW_GPU_FAN = 12,    // not displayed (layout v2), no longer sent
  HW_VRAM_USED = 13,
  HW_VRAM_TOTAL = 14,
  HW_DISK_USED = 15,  // system drive, not shown yet
  HW_DISK_TOTAL = 16,
  HW_COUNT = 17,
};

#define HW_TEMPS_MAX 3
#define HW_TEMP_LABEL_MAX 11 // bytes, UTF-8

enum : uint8_t
{
  CMD_PLAY_PAUSE = 1,
  CMD_NEXT = 2,
  CMD_PREV = 3,
};

#define STATE_FLAG_SESSION 0x01
#define STATE_FLAG_PLAYING 0x02
