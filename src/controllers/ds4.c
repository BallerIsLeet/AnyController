/*
 * DualShock 4 driver — maps USB input report 0x01 to NormalizedInput.
 */

#include "../../include/controllers/ds4.h"

#include <string.h>

/* The d-pad is a hat switch; the virtual pad wants individual directions, so
 * diagonals set two of them. */
static void map_dpad(uint8_t hat, NormalizedInput *out)
{
    if (hat >= DS4_DPAD_RELEASED) return;

    out->dpad_up    = (hat == 0 || hat == 1 || hat == 7);
    out->dpad_right = (hat == 1 || hat == 2 || hat == 3);
    out->dpad_down  = (hat == 3 || hat == 4 || hat == 5);
    out->dpad_left  = (hat == 5 || hat == 6 || hat == 7);
}

static int ds4_init(Controller *ctrl)
{
    ctrl->priv = NULL;
    return 0;
}

static int ds4_parse_input(Controller *ctrl, const uint8_t *r, int len,
                           NormalizedInput *out)
{
    (void)ctrl;

    if (len < DS4_INPUT_LEN || r[0] != DS4_REPORT_INPUT_USB)
        return -1;

    memset(out, 0, sizeof(*out));

    /* Sticks are already 0..255, centre 128, Y growing downwards — exactly
     * what the virtual pad report takes, so no scaling or inversion. */
    out->left_stick_x  = r[1];
    out->left_stick_y  = r[2];
    out->right_stick_x = r[3];
    out->right_stick_y = r[4];

    uint8_t face = r[5];
    out->square   = !!(face & DS4_SQUARE);
    out->cross    = !!(face & DS4_CROSS);
    out->circle   = !!(face & DS4_CIRCLE);
    out->triangle = !!(face & DS4_TRIANGLE);
    map_dpad(face & DS4_DPAD_MASK, out);

    uint8_t sys = r[6];
    out->l1      = !!(sys & DS4_L1);
    out->r1      = !!(sys & DS4_R1);
    out->l3      = !!(sys & DS4_L3);
    out->r3      = !!(sys & DS4_R3);
    out->create  = !!(sys & DS4_SHARE);      /* Share → Create */
    out->options = !!(sys & DS4_OPTIONS);

    out->ps_button = !!(r[7] & DS4_PS);
    out->touchpad  = !!(r[7] & DS4_TOUCHPAD);

    /* Analog triggers; the pad's own digital L2/R2 bits are redundant, vpad
     * derives those from the analog value. */
    out->l2_analog = r[8];
    out->r2_analog = r[9];

    return 0;
}

static void ds4_cleanup(Controller *ctrl)
{
    (void)ctrl;
}

static const ControllerDriver ds4_driver_v1 = {
    .name         = "DualShock 4",
    .type         = CTRL_TYPE_DS4,
    .vendor_id    = SONY_VID,
    .product_id   = DS4_PID_V1,
    .init         = ds4_init,
    .setup        = NULL,   /* no handshake: the pad streams input on its own */
    .parse_input  = ds4_parse_input,
    .build_output = NULL,   /* no rumble/lightbar support yet */
    .cleanup      = ds4_cleanup,
};

static const ControllerDriver ds4_driver_v2 = {
    .name         = "DualShock 4 v2",
    .type         = CTRL_TYPE_DS4,
    .vendor_id    = SONY_VID,
    .product_id   = DS4_PID_V2,
    .init         = ds4_init,
    .setup        = NULL,
    .parse_input  = ds4_parse_input,
    .build_output = NULL,
    .cleanup      = ds4_cleanup,
};

const ControllerDriver *ds4_get_driver(int variant)
{
    return variant == 1 ? &ds4_driver_v2 : &ds4_driver_v1;
}
