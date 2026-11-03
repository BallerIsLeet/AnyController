/*
 * Virtual Pad — register virtual DualSense controllers via /dev/hid.
 *
 * libScePad's virtual-device functions are thin wrappers over three ioctls on
 * /dev/hid that don't check the caller, so a payload can register a pad the
 * whole system (home screen, user picker, games) treats as a DualSense.
 * Interface as used by FGG-XSense (https://github.com/FGGstore/FGG-XSense).
 */

#ifndef VPAD_H
#define VPAD_H

#include "anycontroller.h"
#include "sce_pad.h"
#include "controller.h"

#include <stdint.h>
#include <stdbool.h>

#define HID_ADD_DEVICE   0xC018482AUL   /* _IOWR('H', 0x2A, 24) */
#define HID_INSERT_DATA  0x8018482CUL   /* _IOW ('H', 0x2C, 24) */
#define HID_DEL_DEVICE   0x80104850UL   /* _IOW ('H', 0x50, 16) */

#define HID_CMD_ADD      3
#define HID_CMD_DEL      5

/* Device descriptor handed to HID_ADD_DEVICE */
#define VDEV_DESC_SIZE   168
#define VDEV_PROTO_OFF   0x00
#define VDEV_USER_OFF    0x1C
#define VDEV_CLASS_OFF   0x20
#define VDEV_PROTO       3
#define VDEV_CLASS_PAD   1

/* User id FGG-XSense registers pads under; verified on hardware. */
#define VPAD_DEFAULT_USER 1

/* Handles of the pads we register are recorded here, so that a payload killed
 * without cleaning up leaves nothing behind for the next run to trip over. */
#define VPAD_LEDGER_FILE "/data/anycontroller.vpads"

/* Request taken by HID_ADD_DEVICE and HID_INSERT_DATA */
typedef struct {
    uint32_t  cmd;       /* HID_CMD_ADD, or the device handle for insert */
    uint32_t  reserved;
    void     *data;      /* descriptor (add) or VpadReport (insert) */
    void     *out;       /* int32_t handle (add) */
} HidRequest;

/* Request taken by HID_DEL_DEVICE */
typedef struct {
    uint8_t   cmd;       /* HID_CMD_DEL */
    uint8_t   pad[7];
    uint32_t  handle;
    uint32_t  pad2;
} HidDelRequest;

/* The report the kernel reads. NOT a ScePadData: these are the offsets
 * libScePad copies ScePadData fields to. Sticks use PlayStation convention
 * (0..255, center 128, Y increasing downwards). */
typedef struct __attribute__((packed)) {
    uint8_t  reserved0[0x0C];
    uint32_t buttons;           /* 0x0C: SCE_PAD_BUTTON_* */
    uint8_t  left_stick_x;      /* 0x10 */
    uint8_t  left_stick_y;      /* 0x11 */
    uint8_t  right_stick_x;     /* 0x12 */
    uint8_t  right_stick_y;     /* 0x13 */
    uint8_t  l2_analog;         /* 0x14 */
    uint8_t  r2_analog;         /* 0x15 */
    uint8_t  reserved1;         /* 0x16 */
    uint8_t  active;            /* 0x17: libScePad always writes 1 */
    uint8_t  reserved2[0x60];   /* 0x18 */
    float    orient_x;          /* 0x78: motion block, eight floats */
    float    orient_y;
    float    orient_z;
    float    orient_w;
    float    motion_rest[4];    /* 0x88 */
    uint8_t  reserved3[0x08];   /* 0x98 */
} VpadReport;

_Static_assert(sizeof(VpadReport) == 0xA0, "kernel reads a 0xA0-byte report");

typedef struct {
    int32_t     handle;
    uint32_t    user_id;
    bool        active;
    VpadReport  report;
} Vpad;

/* Open /dev/hid. Returns 0 on success. */
int vpad_init(void);

/* Register a virtual DualSense owned by user_id. NULL on failure (errno set). */
Vpad *vpad_create(uint32_t user_id);

/* Translate and submit one frame of input. Returns 0 on success. */
int vpad_submit_input(Vpad *vpad, const NormalizedInput *input);

/* Remove a virtual pad. A pad left registered by a payload that was killed
 * (SIGKILL, crash) is removed by the next instance; see vpad_reap_stale. */
void vpad_destroy(Vpad *vpad);

/* Remove the pads a previous instance recorded in the ledger and clear the
 * ledger. Call once after vpad_init(), before creating any pad: handles are
 * not enumerable, so this is the only way back to a clean state. */
void vpad_reap_stale(void);

/* Remove all pads and close /dev/hid. */
void vpad_shutdown(void);

#endif /* VPAD_H */
