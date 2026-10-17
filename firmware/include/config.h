// DeskDial firmware: build features and tuning.
// SPDX-License-Identifier: MIT
#pragma once

// ---------------------------------------------------------------- features --
// 1 = compiled in. The PlatformIO environments (full / media / hardware) set
// these with -D; with any other build system, edit the defaults here.
#ifndef DESKDIAL_MEDIA
#define DESKDIAL_MEDIA 1 // "now playing" page: track, artwork, controls
#endif
#ifndef DESKDIAL_HARDWARE
#define DESKDIAL_HARDWARE 1 // hardware page: loads, temperatures, memory
#endif
#if !DESKDIAL_MEDIA && !DESKDIAL_HARDWARE
#error "Enable at least one of DESKDIAL_MEDIA and DESKDIAL_HARDWARE"
#endif

#define DESKDIAL_VERSION "1.0.0"

// ------------------------------------------------------------------ timing --
#define LINK_TIMEOUT_MS 5000     // PC sends something at least every second
#define HELLO_PERIOD_MS 2000     // handshake retry while unlinked
#define PAUSED_IDLE_MS 120000    // paused this long -> "Nothing is playing"
#define DIM_AFTER_MS 10000       // idle this long -> backlight dims
#define TAP_FEEDBACK_MS 700      // play/pause glyph over the artwork
#define OPTIMISTIC_HOLD_MS 1500  // ignore stale "playing" from the PC after a tap
#define SETTINGS_HOLD_MS 5000    // long press -> brightness dialog
#define PC_OFF_AFTER_MS 2000     // no USB heartbeat this long -> PC off/asleep
#define PC_OFF_PEEK_MS 5000      // "PC is off" shown after a touch
#define PC_OFF_POLL_MS 100       // loop period while dark
#define PC_OFF_CPU_MHZ 80        // CPU clock while dark (0 = leave as is)
#define HW_STALE_MS 3000         // no hardware data this long -> "--"
#define PAGE_ANIM_MS 250         // page swipe animation

// -------------------------------------------------------------- brightness --
#define BRIGHT_DEFAULT 100 // %
#define BRIGHT_DIM 10      // % while idle
