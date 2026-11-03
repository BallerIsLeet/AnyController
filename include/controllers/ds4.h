/*
 * Sony DualShock 4 driver.
 *
 * Over USB the pad streams 64-byte input report 0x01 at ~250 Hz and needs no
 * handshake, so this driver is pure translation: sticks and triggers already
 * arrive in PlayStation convention and only the d-pad hat has to be unpacked.
 * Bluetooth uses report 0x11, which this driver does not handle.
 *
 * Report 0x01 layout:
 *   0     report id, 0x01
 *   1..4  left X, left Y, right X, right Y   (0..255, centre 128, Y down)
 *   5     [7:4] triangle, circle, cross, square   [3:0] d-pad hat
 *   6     [7] R3 [6] L3 [5] options [4] share [3] R2 [2] L2 [1] R1 [0] L1
 *   7     [1] touchpad click   [0] PS
 *   8..9  L2, R2 analog
 */

#ifndef DS4_H
#define DS4_H

#include "../controller.h"

#define SONY_VID              0x054C
#define DS4_PID_V1            0x05C4  /* CUH-ZCT1x, 2013 */
#define DS4_PID_V2            0x09CC  /* CUH-ZCT2x, 2016 */

#define DS4_REPORT_INPUT_USB  0x01
#define DS4_INPUT_LEN         64

/* Buttons, byte 5 (upper nibble; lower nibble is the d-pad hat) */
#define DS4_SQUARE            0x10
#define DS4_CROSS             0x20
#define DS4_CIRCLE            0x40
#define DS4_TRIANGLE          0x80

/* Buttons, byte 6 */
#define DS4_L1                0x01
#define DS4_R1                0x02
#define DS4_L2                0x04
#define DS4_R2                0x08
#define DS4_SHARE             0x10
#define DS4_OPTIONS           0x20
#define DS4_L3                0x40
#define DS4_R3                0x80

/* Buttons, byte 7 */
#define DS4_PS                0x01
#define DS4_TOUCHPAD          0x02

/* Hat: 0 up, 2 right, 4 down, 6 left, odd values are diagonals, >= 8 is
 * released. */
#define DS4_DPAD_MASK         0x0F
#define DS4_DPAD_RELEASED     8

/* V1 and V2 speak the same protocol; only the USB id differs. */
const ControllerDriver *ds4_get_driver(int variant);   /* 0 = V1, 1 = V2 */

#endif /* DS4_H */
