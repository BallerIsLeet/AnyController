/*
 * Virtual Pad — register virtual DualSense controllers via /dev/hid ioctls.
 */

#include "../include/vpad.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <ps5/klog.h>

#define STALE_MAX  16   /* handles remembered for crashed runs */

static int  g_hid_fd = -1;
static Vpad g_vpads[MAX_CONTROLLERS];

/* Rewrite the ledger from the pads we hold open. Pad handles are not
 * enumerable, so this file is the only record of a pad that a killed payload
 * left registered with the kernel. */
static void ledger_save(void)
{
    FILE *f = fopen(VPAD_LEDGER_FILE, "w");
    if (!f) return;

    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (g_vpads[i].active)
            fprintf(f, "%d\n", g_vpads[i].handle);

    fclose(f);
}

int vpad_init(void)
{
    g_hid_fd = open("/dev/hid", O_RDWR);
    if (g_hid_fd < 0) return -1;

    memset(g_vpads, 0, sizeof(g_vpads));
    return 0;
}

Vpad *vpad_create(uint32_t user_id)
{
    if (g_hid_fd < 0) return NULL;

    Vpad *vpad = NULL;
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (!g_vpads[i].active) { vpad = &g_vpads[i]; break; }
    if (!vpad) return NULL;
    memset(vpad, 0, sizeof(*vpad));

    uint8_t desc[VDEV_DESC_SIZE];
    memset(desc, 0, sizeof(desc));
    desc[VDEV_PROTO_OFF] = VDEV_PROTO;
    memcpy(desc + VDEV_USER_OFF, &user_id, sizeof(user_id));
    desc[VDEV_CLASS_OFF] = VDEV_CLASS_PAD;

    int32_t handle = -1;
    HidRequest req;
    memset(&req, 0, sizeof(req));
    req.cmd  = HID_CMD_ADD;
    req.data = desc;
    req.out  = &handle;

    if (ioctl(g_hid_fd, HID_ADD_DEVICE, &req) != 0)
        return NULL;

    vpad->handle  = handle;
    vpad->user_id = user_id;
    vpad->active  = true;

    vpad->report.left_stick_x  = 128;
    vpad->report.left_stick_y  = 128;
    vpad->report.right_stick_x = 128;
    vpad->report.right_stick_y = 128;
    vpad->report.active        = 1;
    vpad->report.orient_w      = 1.0f;

    ledger_save();
    return vpad;
}

int vpad_submit_input(Vpad *vpad, const NormalizedInput *input)
{
    if (!vpad || !vpad->active) return -1;

    VpadReport *rpt = &vpad->report;
    uint32_t buttons = 0;

    if (input->cross)      buttons |= SCE_PAD_BUTTON_CROSS;
    if (input->circle)     buttons |= SCE_PAD_BUTTON_CIRCLE;
    if (input->square)     buttons |= SCE_PAD_BUTTON_SQUARE;
    if (input->triangle)   buttons |= SCE_PAD_BUTTON_TRIANGLE;
    if (input->l1)         buttons |= SCE_PAD_BUTTON_L1;
    if (input->r1)         buttons |= SCE_PAD_BUTTON_R1;
    if (input->l3)         buttons |= SCE_PAD_BUTTON_L3;
    if (input->r3)         buttons |= SCE_PAD_BUTTON_R3;
    if (input->dpad_up)    buttons |= SCE_PAD_BUTTON_UP;
    if (input->dpad_down)  buttons |= SCE_PAD_BUTTON_DOWN;
    if (input->dpad_left)  buttons |= SCE_PAD_BUTTON_LEFT;
    if (input->dpad_right) buttons |= SCE_PAD_BUTTON_RIGHT;
    if (input->options)    buttons |= SCE_PAD_BUTTON_OPTIONS;
    if (input->create)     buttons |= SCE_PAD_BUTTON_CREATE;
    if (input->touchpad)   buttons |= SCE_PAD_BUTTON_TOUCH_PAD;
    if (input->ps_button)  buttons |= SCE_PAD_BUTTON_PS;
    if (input->l2_analog > 128) buttons |= SCE_PAD_BUTTON_L2;
    if (input->r2_analog > 128) buttons |= SCE_PAD_BUTTON_R2;

    rpt->buttons       = buttons;
    rpt->left_stick_x  = input->left_stick_x;
    rpt->left_stick_y  = input->left_stick_y;
    rpt->right_stick_x = input->right_stick_x;
    rpt->right_stick_y = input->right_stick_y;
    rpt->l2_analog     = input->l2_analog;
    rpt->r2_analog     = input->r2_analog;
    rpt->active        = 1;

    if (input->has_orientation) {
        rpt->orient_x = input->orient_x;
        rpt->orient_y = input->orient_y;
        rpt->orient_z = input->orient_z;
        rpt->orient_w = input->orient_w;
    }

    HidRequest req;
    memset(&req, 0, sizeof(req));
    req.cmd  = (uint32_t)vpad->handle;
    req.data = rpt;

    return ioctl(g_hid_fd, HID_INSERT_DATA, &req);
}

void vpad_destroy(Vpad *vpad)
{
    if (!vpad || !vpad->active) return;

    HidDelRequest del;
    memset(&del, 0, sizeof(del));
    del.cmd    = HID_CMD_DEL;
    del.handle = (uint32_t)vpad->handle;
    ioctl(g_hid_fd, HID_DEL_DEVICE, &del);

    vpad->active = false;
    vpad->handle = -1;

    ledger_save();
}

void vpad_reap_stale(void)
{
    if (g_hid_fd < 0) return;

    FILE *f = fopen(VPAD_LEDGER_FILE, "r");
    if (!f) return;

    int32_t handles[STALE_MAX];
    int count = 0;
    while (count < STALE_MAX && fscanf(f, "%d", &handles[count]) == 1)
        count++;
    fclose(f);

    int removed = 0;
    for (int i = 0; i < count; i++) {
        if (handles[i] <= 0) continue;

        HidDelRequest del;
        memset(&del, 0, sizeof(del));
        del.cmd    = HID_CMD_DEL;
        del.handle = (uint32_t)handles[i];
        if (ioctl(g_hid_fd, HID_DEL_DEVICE, &del) == 0)
            removed++;
    }

    if (count)
        klog_printf("[AnyController] removed %d of %d stale virtual pad(s)\n",
                    removed, count);

    ledger_save();
}

void vpad_shutdown(void)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        vpad_destroy(&g_vpads[i]);

    if (g_hid_fd >= 0) {
        close(g_hid_fd);
        g_hid_fd = -1;
    }
}
