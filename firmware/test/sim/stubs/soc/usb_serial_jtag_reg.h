// Simulator stand-in: the USB start-of-frame counter comes from the test.
#pragma once
#include <stdint.h>
uint32_t sim_usb_frame();
#define USB_SERIAL_JTAG_FRAM_NUM_REG 0
#define USB_SERIAL_JTAG_SOF_FRAME_INDEX 0
#define REG_GET_FIELD(r, f) sim_usb_frame()
