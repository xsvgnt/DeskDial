// Serial link to the Windows companion: framing, handshake, link state.
// Protocol: include/protocol.h and docs/protocol.md.
// SPDX-License-Identifier: MIT
#include "app.h"
#include <esp_heap_caps.h>

LinkState link_state;

static uint8_t *rx_buf = nullptr; // frame payload, PSRAM (an artwork is 45 KB)
static uint32_t last_hello_ms = 0;

uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

uint8_t board_features()
{
  return (DESKDIAL_MEDIA ? FEATURE_MEDIA : 0) | (DESKDIAL_HARDWARE ? FEATURE_HARDWARE : 0);
}

void link_begin()
{
  Serial.setRxBufferSize(16384);
  Serial.begin(DD_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0); // never block when the PC isn't listening
#endif
  rx_buf = (uint8_t *)heap_caps_malloc(DD_MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rx_buf)
    app_fatal("PSRAM allocation failed: enable OPI PSRAM");
}

void link_send(uint8_t type, const uint8_t *payload, uint32_t len)
{
  uint8_t hdr[7] = {DD_SYNC0, DD_SYNC1, type,
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

void link_send_hello(bool reply)
{
  uint8_t hello[HELLO_LEN] = {'D', 'D', DD_PROTOCOL_VERSION, board_features(),
                              (uint8_t)(reply ? HELLO_FLAG_REPLY : 0)};
  link_send(MSG_HELLO, hello, sizeof hello);
  uint8_t page = page_current();
  link_send(MSG_PAGE, &page, 1); // tells the PC whether to collect hardware data
  last_hello_ms = millis();
}

void link_reset()
{
  link_state.up = false;
}

void link_tick(uint32_t t)
{
  if (link_state.up && t - link_state.last_rx_ms > LINK_TIMEOUT_MS)
  {
    link_state.up = false;
    app_on_link_lost();
  }
  if (!link_state.up && t - last_hello_ms > HELLO_PERIOD_MS)
    link_send_hello();
}

static void handle_hello(const uint8_t *p, uint32_t len)
{
  if (len < HELLO_LEN - 1 || p[0] != 'D' || p[1] != 'D')
    return; // not a DeskDial companion
  // p[2] = protocol version: 1 is the only one so far. Later versions add
  // fields at the end, so a longer HELLO is still read the same way.
  bool reply = len >= HELLO_LEN && (p[4] & HELLO_FLAG_REPLY);
  if (p[3] != link_state.pc_features)
  {
    link_state.pc_features = p[3];
    page_refresh_available();
  }
  if (!reply)
    link_send_hello(true);
}

static void handle_frame(uint8_t type, const uint8_t *p, uint32_t len)
{
  link_state.last_rx_ms = millis();
  link_state.up = true;
  if (type == MSG_HELLO)
    handle_hello(p, len);
  else
    app_on_frame(type, p, len); // unknown types are ignored
}

void link_poll()
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
        if (b == DD_SYNC0)
          st = S_SYNC1;
        break;
      case S_SYNC1:
        st = (b == DD_SYNC1) ? S_HDR : (b == DD_SYNC0 ? S_SYNC1 : S_SYNC0);
        hn = 0;
        break;
      case S_HDR:
        hdr[hn++] = b;
        if (hn == 5)
        {
          plen = rd32(hdr + 1);
          pn = 0;
          sum = hdr[0] + hdr[1] + hdr[2] + hdr[3] + hdr[4];
          st = plen > DD_MAX_PAYLOAD ? S_SYNC0 : (plen ? S_PAYLOAD : S_SUM);
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
