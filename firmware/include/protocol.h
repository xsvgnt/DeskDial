// DeskDial serial protocol, shared by the board firmware and the Windows
// companion (host/Core/Protocol.cs mirrors these values). Full description:
// docs/protocol.md.
// SPDX-License-Identifier: MIT
//
// Every frame, in both directions:
//   0xA5 0x5A | type (u8) | length (u32 LE) | payload | checksum (u8)
// checksum = low byte of the sum of type, the 4 length bytes and the payload.
//
// Both directions
//   MSG_HELLO     'D' 'D' version u8 features u8 (FEATURE_* bits) flags u8
//                 (HELLO_FLAG_*): each side says which modules it was built
//                 with. A HELLO without HELLO_FLAG_REPLY asks the other side
//                 to answer with its own HELLO (flag set); a reply is never
//                 answered. The board asks at boot and every 2 s while
//                 unlinked; the PC asks when it opens the port (that's how it
//                 finds the board's COM port). The PC resends the current
//                 track, artwork and text images on every HELLO from the board.
// PC -> board
//   MSG_STATE     flags u8 (bit0 session, bit1 playing), position_ms u32,
//                 duration_ms u32, art_id u32,
//                 title_len u8, title UTF-8, artist_len u8, artist UTF-8,
//                 [rate u16 LE: playback speed x1000, optional, 1000 if absent]
//   MSG_ART       art_id u32, 150*150 RGB565 big-endian (LV_COLOR_16_SWAP=1)
//   MSG_ART_NONE  art_id u32                 track has no artwork
//   MSG_TEXT_IMG  field u8 (TEXT_FIELD_*), width u16 LE, height u16 LE, then
//                 4-bit alpha pixels (rows of ceil(width/2) bytes, first pixel
//                 in the high nibble). Title/artist rendered by the PC when
//                 they use characters the board's fonts lack (CJK, Arabic...).
//                 Width 0 = use the normal text label. Sent before the
//                 MSG_STATE carrying the matching text.
//   MSG_HW        count u8, then count x (metric id u8, value float32 LE);
//                 metrics the PC can't read are absent. Once a second, only
//                 while the board shows the hardware page.
//   MSG_HW_TEMPS  count u8, then count x (label_len u8, label UTF-8,
//                 value float32 LE): labelled temperatures (RAM, SSD...),
//                 sent right after MSG_HW. At most HW_TEMPS_MAX.
// Board -> PC
//   MSG_CMD       command u8 (CMD_*)
//   MSG_PAGE      page u8 (PAGE_*), after every HELLO and on each change
#pragma once
#include <stdint.h>

#define DD_SYNC0 0xA5
#define DD_SYNC1 0x5A
#define DD_BAUD 921600 // ignored by native USB CDC, used by a USB-UART bridge
#define DD_PROTOCOL_VERSION 1

enum : uint8_t
{
  FEATURE_MEDIA = 0x01,
  FEATURE_HARDWARE = 0x02,
};
#define HELLO_FLAG_REPLY 0x01
#define HELLO_LEN 5

enum : uint8_t
{
  MSG_HELLO = 0x01,
  MSG_STATE = 0x02,
  MSG_ART = 0x03,
  MSG_ART_NONE = 0x04,
  MSG_HW = 0x05,
  MSG_HW_TEMPS = 0x06,
  MSG_TEXT_IMG = 0x07,
  MSG_CMD = 0x10,
  MSG_PAGE = 0x11,
};

enum : uint8_t
{
  PAGE_MEDIA = 0,
  PAGE_HW = 1,
};

enum : uint8_t
{
  CMD_PLAY_PAUSE = 1,
  CMD_NEXT = 2,
  CMD_PREV = 3,
};

// ------------------------------------------------------------------ media --
#define STATE_FLAG_SESSION 0x01
#define STATE_FLAG_PLAYING 0x02

#define ART_W 150
#define ART_H 150
#define ART_BYTES (ART_W * ART_H * 2)

enum : uint8_t
{
  TEXT_FIELD_TITLE = 0,
  TEXT_FIELD_ARTIST = 1,
};
// Text images have a fixed height with the baseline on a fixed row, so the
// board can line them up exactly where its own label would draw.
#define TEXT_IMG_TITLE_H 30
#define TEXT_IMG_TITLE_BASELINE 23
#define TEXT_IMG_TITLE_MAX_W 1024 // longer titles are cut with an ellipsis
#define TEXT_IMG_ARTIST_H 24
#define TEXT_IMG_ARTIST_BASELINE 18
#define TEXT_IMG_ARTIST_MAX_W 240 // = visible width, ellipsis like the label

// --------------------------------------------------------------- hardware --
// Units: temperatures °C, loads %, clocks MHz, power W, memory and disk GB.
// New ids can be added without breaking older firmware (unknown ids are
// ignored); retired ids are never reused.
enum : uint8_t
{
  HW_CPU_TEMP = 1,
  HW_CPU_LOAD = 2,
  HW_CPU_CLOCK = 3,
  HW_CPU_POWER = 4,
  // 5: retired (CPU fan)
  HW_RAM_USED = 6,
  HW_RAM_TOTAL = 7,
  HW_GPU_TEMP = 8,
  HW_GPU_LOAD = 9,
  HW_GPU_CLOCK = 10,
  HW_GPU_POWER = 11,
  // 12: retired (GPU fan)
  HW_VRAM_USED = 13,
  HW_VRAM_TOTAL = 14,
  HW_DISK_USED = 15, // system drive, not shown yet
  HW_DISK_TOTAL = 16,
  HW_COUNT = 17,
};
#define HW_TEMPS_MAX 3
#define HW_TEMP_LABEL_MAX 11 // bytes, UTF-8

// Largest payload the board accepts: the artwork frame.
#define DD_MAX_PAYLOAD (4 + ART_BYTES)
