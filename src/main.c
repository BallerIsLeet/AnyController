/*
 * AnyController — main entry point.
 *
 * Uses the /dev/hid virtual pad API to register external controllers
 * as DualSense devices at the PS5 system level.
 */

#include "../include/anycontroller.h"
#include "../include/controller.h"
#include "../include/vpad.h"
#include "../include/transport/usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <ps5/klog.h>

#define PID_FILE        "/data/anycontroller.pid"
#define SCAN_EVERY_N    3   /* frames between USB rescans (~25ms) */

typedef struct {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *fmt, ...)
{
    notify_request_t req;
    va_list args;

    memset(&req, 0, sizeof(req));
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    klog_printf("[AnyController] %s\n", req.message);
}

static Controller controllers[MAX_CONTROLLERS];
static Vpad      *vpads[MAX_CONTROLLERS];

static volatile bool running = true;

static void signal_handler(int sig)
{
    (void)sig;
    running = false;
}

static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static bool is_payload_process(pid_t pid)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    memset(&kp, 0, sizeof(kp));
    /* The PS5 kinfo_proc is larger than the SDK's, so sysctl reports ENOMEM
     * but still fills the leading fields; check those instead of the result. */
    sysctl(mib, 4, &kp, &len, NULL, 0);
    return kp.ki_pid == pid && strcmp(kp.ki_comm, "payload.elf") == 0;
}

/* Stop a previously deployed instance so two copies don't fight over the
 * same USB device, then record our own pid. Returns true when no previous
 * instance is left running, i.e. the pads it recorded in the ledger are stale
 * and safe to remove. */
static bool take_over_previous_instance(void)
{
    bool reap_stale = true;

    FILE *f = fopen(PID_FILE, "r");
    if (f) {
        int old = 0;
        if (fscanf(f, "%d", &old) == 1 && old > 0 && old != getpid() &&
            is_payload_process(old)) {
            klog_printf("[AnyController] stopping previous instance pid=%d\n", old);
            kill(old, SIGTERM);
            for (int i = 0; i < 50 && kill(old, 0) == 0; i++)
                usleep(20000);
            if (kill(old, 0) == 0) {
                /* It refused to go; deleting its pads would fight a live
                 * process, so leave them registered. */
                klog_printf("[AnyController] previous instance pid=%d still running\n", old);
                reap_stale = false;
            }
        }
        fclose(f);
    }
    f = fopen(PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
    return reap_stale;
}

#define RETRY_BACKOFF_US  2000000ULL
#define RESUME_GRACE_US   3000000ULL
#define KEEPALIVE_US      1000000ULL
#define STATS_WINDOW_US   5000000ULL
#define STALL_US          500000ULL
#define SUBMIT_FAIL_LIMIT 30           /* failed inserts before rebuilding the pad (~250 ms) */
#define VPAD_RETRY_US     2000000ULL   /* wait this long before rebuilding the pad again */

typedef struct {
    uint64_t last_report_us;
    uint64_t last_keepalive_us;
    uint64_t window_start_us;
    uint64_t grace_deadline_us;   /* nonzero: USB gone, vpad kept until then */
    unsigned reports;
    unsigned total_reports;
    unsigned submits_failed;
    unsigned submit_fail_streak;   /* consecutive HID_INSERT_DATA failures */
    uint64_t vpad_retry_us;        /* earliest time to rebuild the virtual pad */
    bool     stalled;
    bool     ps_down;
} SlotStats;

static SlotStats stats[MAX_CONTROLLERS];
static uint64_t  next_attach_us;
static bool      vpad_failure_notified;

static uint64_t get_time_us(void);

static void release_usb(int i)
{
    Controller *ctrl = &controllers[i];
    if (ctrl->driver && ctrl->driver->cleanup)
        ctrl->driver->cleanup(ctrl);
    usb_close_device(ctrl->fd);
    const ControllerDriver *drv = ctrl->driver;
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->driver = drv;   /* remembered so a resume only matches the same type */
    ctrl->fd = -1;
    ctrl->state = CTRL_STATE_DISCONNECTED;
}

static void drop_slot(int i, const char *why)
{
    klog_printf("[AnyController] slot %d: %s\n", i, why);
    if (vpads[i]) {
        vpad_destroy(vpads[i]);
        vpads[i] = NULL;
    }
    release_usb(i);
    controllers[i].driver = NULL;
    memset(&stats[i], 0, sizeof(stats[i]));
}

static void submit_neutral(int i)
{
    NormalizedInput idle;
    memset(&idle, 0, sizeof(idle));
    idle.left_stick_x = idle.left_stick_y = 128;
    idle.right_stick_x = idle.right_stick_y = 128;
    vpad_submit_input(vpads[i], &idle);
}

/* The kernel can invalidate a pad handle (account switch, system UI reset) and
 * from then on every insert fails. Rebuild the pad on the same slot so the
 * controller keeps working without the user re-sending the payload. */
static void recover_vpad(int i, uint64_t now)
{
    if (now < stats[i].vpad_retry_us)
        return;

    if (vpads[i])
        vpad_destroy(vpads[i]);
    vpads[i] = vpad_create(VPAD_DEFAULT_USER);

    stats[i].submit_fail_streak = 0;
    stats[i].ps_down = false;
    stats[i].vpad_retry_us = now + VPAD_RETRY_US;

    if (!vpads[i])
        klog_printf("[AnyController] slot %d: could not rebuild virtual pad errno=%d\n",
                    i, errno);
    else
        klog_printf("[AnyController] slot %d: virtual pad rebuilt -> handle=%d\n",
                    i, vpads[i]->handle);
}

/* USB vanished: keep the virtual DualSense registered for a moment so a
 * short controller re-enumeration doesn't look like an unplug to the PS5. */
static void begin_grace(int i, uint64_t now)
{
    /* Nothing to hold on to when the pad could not be rebuilt: drop the slot
     * outright, so the resume path can never touch a NULL virtual pad. */
    if (!vpads[i]) {
        drop_slot(i, "USB lost without a virtual pad");
        return;
    }

    klog_printf("[AnyController] slot %d: USB lost, holding DualSense for %llu ms\n",
                i, (unsigned long long)(RESUME_GRACE_US / 1000));
    release_usb(i);
    submit_neutral(i);
    stats[i].grace_deadline_us = now + RESUME_GRACE_US;
    stats[i].window_start_us = 0;
}

static void expire_grace(uint64_t now)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (stats[i].grace_deadline_us && now >= stats[i].grace_deadline_us) {
            const char *name = controllers[i].driver ? controllers[i].driver->name : "Controller";
            notify("%s disconnected", name);
            drop_slot(i, "did not come back");
        }
    }
}

static int pick_slot(const ControllerDriver *drv)
{
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (stats[i].grace_deadline_us && controllers[i].driver == drv)
            return i;
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].state != CTRL_STATE_CONNECTED &&
            !stats[i].grace_deadline_us && !vpads[i])
            return i;
    return -1;
}

static void attach_new_controllers(void)
{
    if (get_time_us() < next_attach_us)
        return;

    for (;;) {
        Controller found;
        memset(&found, 0, sizeof(found));
        found.fd = -1;
        if (usb_open_next_controller(&found) != 0)
            return;

        int i = pick_slot(found.driver);
        if (i < 0) {
            klog_printf("[AnyController] no free slot for %s\n", found.driver->name);
            usb_close_device(found.fd);
            next_attach_us = get_time_us() + RETRY_BACKOFF_US;
            return;
        }

        Controller *ctrl = &controllers[i];
        bool resuming = stats[i].grace_deadline_us != 0;
        *ctrl = found;
        ctrl->slot = i;

        if (ctrl->driver->init(ctrl) != 0) {
            release_usb(i);
            if (!resuming)
                ctrl->driver = NULL;
            return;
        }
        if (ctrl->driver->setup && ctrl->driver->setup(ctrl) != 0) {
            klog_printf("[AnyController] slot %d: setup failed, will retry\n", i);
            release_usb(i);
            if (!resuming)
                ctrl->driver = NULL;
            next_attach_us = get_time_us() + RETRY_BACKOFF_US;
            return;
        }

        if (resuming) {
            stats[i].grace_deadline_us = 0;
            ctrl->state = CTRL_STATE_CONNECTED;
            klog_printf("[AnyController] slot %d: %s resumed on vpad handle=%d\n",
                        i, ctrl->driver->name, vpads[i]->handle);
            continue;
        }

        vpads[i] = vpad_create(VPAD_DEFAULT_USER);
        if (!vpads[i]) {
            klog_printf("[AnyController] vpad_create failed errno=%d\n", errno);
            drop_slot(i, "no virtual pad");
            next_attach_us = get_time_us() + RETRY_BACKOFF_US;
            if (!vpad_failure_notified) {
                vpad_failure_notified = true;
                notify("AnyController: could not create virtual DualSense");
            }
            return;
        }

        ctrl->state = CTRL_STATE_CONNECTED;
        klog_printf("[AnyController] slot %d: %s -> vpad handle=%d\n",
                    i, ctrl->driver->name, vpads[i]->handle);
        notify("%s connected as DualSense", ctrl->driver->name);
    }
}

/* Periodic neutral output report, like SDL/hid-nintendo keep sending. */
static void send_keepalive(Controller *ctrl)
{
    uint8_t out[64];
    if (!ctrl->driver->build_output)
        return;
    int len = ctrl->driver->build_output(ctrl, 0, 0, 0, 0, 0, out, sizeof(out));
    if (len > 0)
        usb_write_report(ctrl->fd, out, len);
}

static void poll_slot(int i, uint64_t now)
{
    Controller *ctrl = &controllers[i];
    SlotStats *st = &stats[i];
    uint8_t buf[64];
    NormalizedInput input;
    bool have_input = false;

    if (st->window_start_us == 0) {
        st->window_start_us = now;
        st->last_report_us = now;
        st->last_keepalive_us = now;
    }

    /* Drain everything queued; the controller can report faster than we poll. */
    for (;;) {
        int n = usb_read_report(ctrl->fd, buf, sizeof(buf));
        if (n < 0) {
            klog_printf("[AnyController] slot %d: read failed errno=%d, %llu ms since last report\n",
                        i, errno, (unsigned long long)((now - st->last_report_us) / 1000));
            begin_grace(i, now);
            return;
        }
        if (n == 0)
            break;
        st->reports++;
        st->last_report_us = now;
        if (st->total_reports++ < 2) {
            char hex[3 * 24 + 1] = {0};
            for (int k = 0; k < n && k < 24; k++)
                snprintf(hex + 3 * k, 4, "%02x ", buf[k]);
            klog_printf("[AnyController] slot %d: raw report (%d bytes): %s\n", i, n, hex);
        }
        if (st->stalled) {
            st->stalled = false;
            klog_printf("[AnyController] slot %d: reports resumed\n", i);
        }
        if (ctrl->driver->parse_input(ctrl, buf, n, &input) == 0)
            have_input = true;
    }

    if (!st->stalled && now - st->last_report_us > STALL_US) {
        st->stalled = true;
        klog_printf("[AnyController] slot %d: no reports for %llu ms\n",
                    i, (unsigned long long)((now - st->last_report_us) / 1000));
    }

    if (have_input) {
        if (input.ps_button != st->ps_down) {
            st->ps_down = input.ps_button;
            klog_printf("[AnyController] slot %d: Home/PS %s\n", i,
                        input.ps_button ? "pressed" : "released");
        }
        input.timestamp_us = now;
        if (!vpads[i]) {
            recover_vpad(i, now);
        } else if (vpad_submit_input(vpads[i], &input) == 0) {
            st->submit_fail_streak = 0;
        } else {
            if (st->submits_failed++ == 0)
                klog_printf("[AnyController] slot %d: input submit failed errno=%d\n", i, errno);
            if (++st->submit_fail_streak >= SUBMIT_FAIL_LIMIT)
                recover_vpad(i, now);
        }
    }

    if (now - st->last_keepalive_us >= KEEPALIVE_US) {
        st->last_keepalive_us = now;
        send_keepalive(ctrl);
    }

    if (now - st->window_start_us >= STATS_WINDOW_US) {
        klog_printf("[AnyController] slot %d: %u reports/5s, %u submit failures\n",
                    i, st->reports, st->submits_failed);
        st->window_start_us = now;
        st->reports = 0;
        st->submits_failed = 0;
    }
}

int main(void)
{
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    klog_printf("[AnyController] === Starting v%d.%d.%d (pid %d) ===\n",
                ANYCONTROLLER_VERSION_MAJOR, ANYCONTROLLER_VERSION_MINOR,
                ANYCONTROLLER_VERSION_PATCH, getpid());

    bool reap_stale_vpads = take_over_previous_instance();
    controller_drivers_init();

    if (vpad_init() != 0) {
        klog_printf("[AnyController] /dev/hid open failed errno=%d\n", errno);
        notify("AnyController: /dev/hid failed!");
        return 1;
    }

    /* Pads a crashed instance registered are still attached to the system and
     * would show up as phantom DualSenses; remove them before adding ours. */
    if (reap_stale_vpads)
        vpad_reap_stale();

    usb_init();
    memset(controllers, 0, sizeof(controllers));
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        controllers[i].fd = -1;

    notify("AnyController v%d.%d.%d running - plug in a controller",
           ANYCONTROLLER_VERSION_MAJOR, ANYCONTROLLER_VERSION_MINOR,
           ANYCONTROLLER_VERSION_PATCH);

    unsigned frame = 0;
    while (running) {
        uint64_t frame_start = get_time_us();

        if (frame++ % SCAN_EVERY_N == 0)
            attach_new_controllers();

        for (int i = 0; i < MAX_CONTROLLERS; i++)
            if (controllers[i].state == CTRL_STATE_CONNECTED)
                poll_slot(i, frame_start);
        expire_grace(frame_start);

        uint64_t elapsed = get_time_us() - frame_start;
        if (elapsed < POLL_INTERVAL_US)
            usleep(POLL_INTERVAL_US - elapsed);
    }

    klog_printf("[AnyController] shutting down\n");
    for (int i = 0; i < MAX_CONTROLLERS; i++)
        if (controllers[i].state == CTRL_STATE_CONNECTED || vpads[i])
            drop_slot(i, "shutdown");
    vpad_shutdown();
    usb_shutdown();
    notify("AnyController stopped");
    return 0;
}
