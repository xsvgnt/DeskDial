# Serial protocol

The board and the Windows companion talk over the USB serial port (921600 baud
when a USB-to-UART chip is in the way; native USB ignores the baud rate). The
constants live in [`firmware/include/protocol.h`](../firmware/include/protocol.h)
and [`host/Core/Protocol.cs`](../host/Core/Protocol.cs), which must agree.

## Frames

Every message, in both directions:

```
A5 5A | type u8 | length u32 LE | payload (length bytes) | checksum u8
```

`checksum` is the low byte of the sum of `type`, the four length bytes and the
payload. The receiver resynchronises on `A5 5A` after a bad checksum or a length
above 45,004 bytes (the artwork frame, the largest one). All integers are
little-endian; floats are IEEE-754 `float32`.

## Handshake

| Type | Name | Payload |
|---|---|---|
| `0x01` | HELLO | `'D' 'D'` · version u8 (`1`) · features u8 · flags u8 |

`features` says which modules the sender was built with: bit 0 media, bit 1
hardware. A HELLO without the reply flag (`flags` bit 0) asks the other side to
answer with its own HELLO, flag set; a reply is never answered, so the two
sides can't ping-pong.

- The **companion** sends a HELLO when it opens a port. That's how it finds the
  board: it tries every COM port and keeps the one that answers.
- The **board** sends one at boot, every 2 s while it has no link, and after the
  PC comes back from sleep. It always follows a HELLO with a PAGE.
- On every HELLO from the board, the companion resends everything it shows
  (track, artwork, text images).

Each side then only uses the features both have. The board offers the pages
both support, and falls back to all of its own pages if the two builds have
nothing in common. The companion only runs a module when the board has it.
Fields may be added at the end of HELLO in later versions; readers ignore extra
bytes.

The link counts as up while any frame arrived in the last 5 s. The companion
sends at least one frame a second.

## PC → board

| Type | Name | Payload |
|---|---|---|
| `0x02` | STATE | flags u8 (bit 0 session, bit 1 playing) · position_ms u32 · duration_ms u32 · art_id u32 · title_len u8 · title UTF-8 · artist_len u8 · artist UTF-8 · [rate u16] |
| `0x03` | ART | art_id u32 · 150 × 150 RGB565 pixels, high byte first (45,000 bytes) |
| `0x04` | ART_NONE | art_id u32: the track has no artwork |
| `0x07` | TEXT_IMG | field u8 (0 title, 1 artist) · width u16 · height u16 · 4-bit alpha pixels |
| `0x05` | HW | count u8 · count × (metric id u8 · value float32) |
| `0x06` | HW_TEMPS | count u8 · count × (label_len u8 · label UTF-8 · value float32) |

**STATE** is sent on every change and once a second. Duration 0 means the
position is unknown (some browsers publish an empty timeline), and the board
hides the time. `rate` is the playback speed × 1000; if absent, 1000. The board
counts the position forward at that speed between frames. Title and artist are
limited to what the board's fonts contain (Latin-1 plus a few typographic marks).

**ART** is pre-scaled by the PC (centre crop), so the board never decodes an
image. The byte order matches LVGL's `LV_COLOR_16_SWAP=1`.

**TEXT_IMG** carries a title or artist line rendered by the PC when it contains
characters the board's fonts lack (Chinese, Japanese, Korean, Arabic...). Rows
are `ceil(width / 2)` bytes, first pixel in the high nibble. The image height is
fixed with the text baseline on a fixed row, so the board lines it up where its
own label would draw:

| Field | Height | Baseline row | Max width |
|---|---|---|---|
| title | 30 | 23 | 1024 (scrolls) |
| artist | 24 | 18 | 240 (ellipsis) |

Width 0 means "use the normal label again". A TEXT_IMG is sent before the STATE
that carries the matching text.

**HW** is sent once a second, only while the board shows the hardware page.
Metrics the PC can't read are left out. Units: °C, %, MHz, W, GB.

| Id | Metric | Id | Metric |
|---|---|---|---|
| 1 | CPU temperature | 9 | GPU load |
| 2 | CPU load | 10 | GPU clock |
| 3 | CPU clock | 11 | GPU power |
| 4 | CPU power | 13 | GPU memory used |
| 6 | RAM used | 14 | GPU memory total |
| 7 | RAM total | 15 | system drive used (not shown) |
| 8 | GPU temperature | 16 | system drive total (not shown) |

Ids 5 and 12 (CPU and GPU fan) are retired and never reused. Unknown ids are
ignored, so new metrics don't break older firmware.

**HW_TEMPS** follows each HW frame with up to three labelled temperatures for
the bottom row (RAM, SSD...). Labels are at most 11 bytes.

## Board → PC

| Type | Name | Payload |
|---|---|---|
| `0x10` | CMD | 1 play/pause · 2 next · 3 previous |
| `0x11` | PAGE | 0 media · 1 hardware: the page on screen, after every HELLO and on each change |
