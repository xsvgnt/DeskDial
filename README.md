# ESP32 Media Remote

Shows what Spotify is playing on the JC3636W518 round display (ESP32-S3) and lets you
play/pause, skip and go back from the screen. The board talks to a small Windows program
over the USB-C cable that already powers it. No Home Assistant, Wi-Fi or cloud needed.

```
Spotify ─► Windows media controls ─► NowPlayingBridge.exe ══USB serial══► ESP32-S3
                                    ◄──── play/pause, next, prev ─────────
```

![UI preview](docs/ui-preview.png)

## Folder layout

| Path | What it is |
|---|---|
| `firmware/NowPlaying/` | Arduino sketch. Open `NowPlaying.ino`. |
| `firmware/NowPlaying/font_*.c` | Montserrat 14/16/22 with Latin-1, plus a 48 px play/pause/music icon font |
| `firmware/NowPlaying/scr_st77916.h`, `pincfg.h` | Vendor driver files (unchanged copies) |
| `pc/NowPlayingBridge/` | Windows bridge (C#, .NET 8) |
| `extras/lv_conf.h` | LVGL config, for reference |

## 1. Firmware (Arduino IDE)

**Libraries** (exact versions matter):
- `ESP32_Display_Panel` **0.1.4** and `ESP32_IO_Expander` **0.0.2**
- `lvgl` **8.4.0**
- Copy `extras/lv_conf.h` into your Arduino `libraries` folder, next to the `lvgl` folder.

**Board settings** (Tools menu):
- Board: *ESP32S3 Dev Module*
- Flash size: 16 MB. Partition scheme: any with an app partition of at least 3 MB.
- PSRAM: **OPI PSRAM**. The sketch stops with a serial message if PSRAM is missing.
- USB CDC On Boot: see the next section.

**Which USB port does the board have?** Plug it in and open *Device Manager → Ports (COM & LPT)*:
- *USB Serial Device (COMx)* or *USB JTAG/serial debug unit*: the native ESP32-S3 USB. Set **USB CDC On Boot: Enabled**.
- *USB-SERIAL CH340*, *CP210x* or similar: a USB-to-UART chip. Set **USB CDC On Boot: Disabled**.

The sketch uses `Serial` and works either way. Only that menu setting has to match the hardware.

If the compiler asks for `ESP_Panel_Conf.h` or a board header, copy it from the vendor's demo
sketch into `firmware/NowPlaying/`.

**Before uploading, quit NowPlayingBridge** (end it in Task Manager, or close its console if you started
it with `--debug`). It holds the COM port, and the Arduino Serial Monitor does the same.

## 2. Windows bridge

Install the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0), then:

```
cd pc\NowPlayingBridge
dotnet publish -c Release
```

The program is at `pc\NowPlayingBridge\bin\Release\net8.0-windows10.0.19041.0\win-x64\publish\NowPlayingBridge.exe`.

- First run: `NowPlayingBridge.exe --debug` opens a console showing port probing, track changes and commands.
- It finds the board by probing every COM port for its handshake. Use `--port COM5` to skip probing.
- It prefers Spotify. If Spotify isn't running it shows whatever app Windows considers current
  (a browser tab, VLC…). Use `--spotify-only` to disable that.
- **Start at login:** press Win+R, type `shell:startup`, and put a shortcut to the exe in that folder.

## Behaviour

- **Tap the artwork** to toggle play/pause. The UI flips immediately, and a play or pause glyph shows
  for 700 ms the state it's moving *to*. The PC confirms the real state about 150 ms later.
  Stale frames are ignored for 1.5 s after a tap so the UI doesn't flicker back.
- **Prev / next** buttons sit at the left and right edges.
- **Idle:** shown when Spotify isn't running, when the PC program isn't connected (5 s timeout),
  or when the track has been **paused for 120 s**. The display shows "Nothing is playing" and hides
  the buttons. The artwork stops responding to taps. After 10 s the backlight dims to 10 %, and it
  comes back to full brightness on any touch or when a track starts.
- Track titles are filtered on the PC to the characters the fonts contain. Letters with accents outside
  Latin-1 lose the accent (ą → a). Other scripts show as `?`.

## Serial protocol

Frames in both directions: `A5 5A | type u8 | length u32 LE | payload | checksum u8`.
The full definition is in `firmware/NowPlaying/protocol.h`. Artwork is sent pre-scaled as 150×150 RGB565
(45 KB, byte-swapped for `LV_COLOR_16_SWAP=1`), so the ESP32 never decodes a JPEG.
