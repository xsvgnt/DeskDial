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
// ESP32 -> PC
//   MSG_HELLO     "NPR1"                     at boot, every 2 s while unlinked,
//                                            and in reply to the PC's HELLO
//   MSG_CMD       command u8
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
  MSG_CMD = 0x10,
};

enum : uint8_t
{
  CMD_PLAY_PAUSE = 1,
  CMD_NEXT = 2,
  CMD_PREV = 3,
};

#define STATE_FLAG_SESSION 0x01
#define STATE_FLAG_PLAYING 0x02
