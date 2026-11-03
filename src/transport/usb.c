/*
 * USB transport over FreeBSD ugen nodes (see usb.h).
 */

#include "../../include/transport/usb.h"
#include "../../include/controller.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <ps5/klog.h>

#define REJECT_MAX       32
#define NAME_LEN         16
#define PROBE_RETRY_US   1000000ULL   /* re-probe a device that failed to open */

static UsbDevice usb_devices[USB_MAX_DEVICES];

/* Devices we already looked at, rebuilt on every scan from the nodes still
 * present. Two kinds of rejection, because they mean different things: an
 * unsupported device stays rejected for as long as its node exists, while a
 * failed probe (endpoint not published yet during enumeration, transient open
 * error) is retried after a backoff instead of hiding the device for good. */
typedef struct {
    char     name[NAME_LEN];
    bool     permanent;
    uint64_t retry_us;
} RejectEntry;

static RejectEntry rejected[REJECT_MAX];
static int         rejected_count;

int usb_init(void)
{
    memset(usb_devices, 0, sizeof(usb_devices));
    memset(rejected, 0, sizeof(rejected));
    rejected_count = 0;
    return 0;
}

static uint64_t usb_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static void reject_unsupported(const char *name)
{
    if (rejected_count >= REJECT_MAX) return;

    RejectEntry *e = &rejected[rejected_count++];
    snprintf(e->name, NAME_LEN, "%s", name);
    e->permanent = true;
    e->retry_us  = 0;
}

static void reject_probe(const char *name, uint64_t now)
{
    if (rejected_count >= REJECT_MAX) return;

    RejectEntry *e = &rejected[rejected_count++];
    snprintf(e->name, NAME_LEN, "%s", name);
    e->permanent = false;
    e->retry_us  = now + PROBE_RETRY_US;
}

static const RejectEntry *reject_find(const RejectEntry *table, int count,
                                      const char *name)
{
    for (int i = 0; i < count; i++)
        if (strcmp(table[i].name, name) == 0)
            return &table[i];
    return NULL;
}

static void reject_carry(const RejectEntry *e)
{
    if (rejected_count < REJECT_MAX)
        rejected[rejected_count++] = *e;
}

static bool is_held(const char *name)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].active && strcmp(usb_devices[i].name, name) == 0)
            return true;
    return false;
}

static int try_open(const char *name, Controller *ctrl, uint64_t now)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/usb/%s.0", name);

    int cfd = open(path, O_RDWR);
    if (cfd < 0) return -1;

    struct usb_device_descriptor dd;
    memset(&dd, 0, sizeof(dd));
    if (ioctl(cfd, USB_GET_DEVICE_DESC, &dd) != 0) {
        close(cfd);
        return -1;
    }
    uint16_t vid = UGETW(dd.idVendor);
    uint16_t pid = UGETW(dd.idProduct);

    const ControllerDriver *drv = controller_driver_find(vid, pid);
    if (!drv) {
        reject_unsupported(name);
        close(cfd);
        return -1;
    }

    UsbDevice *slot = NULL;
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (!usb_devices[i].active) { slot = &usb_devices[i]; break; }
    if (!slot) {
        close(cfd);
        return -1;
    }

    int iface = 0;
    ioctl(cfd, USB_IFACE_DRIVER_DETACH, &iface);
    iface = 0;
    if (ioctl(cfd, USB_CLAIM_INTERFACE, &iface) != 0)
        klog_printf("[AnyController] usb %s: claim interface errno=%d\n", name, errno);

    /* ugen exposes each direction of an endpoint separately and refuses an
     * open in a direction the endpoint lacks (EPERM), so probe for the first
     * IN and first OUT endpoint instead of assuming one bidirectional ep. */
    int in_fd = -1, out_fd = -1, in_ep = 0, out_ep = 0;
    for (int ep = 1; ep <= USB_MAX_ENDPOINT && in_fd < 0; ep++) {
        snprintf(path, sizeof(path), "/dev/usb/%s.%d", name, ep);
        in_fd = open(path, O_RDONLY | O_NONBLOCK);
        in_ep = ep;
    }
    for (int ep = 1; ep <= USB_MAX_ENDPOINT && out_fd < 0; ep++) {
        snprintf(path, sizeof(path), "/dev/usb/%s.%d", name, ep);
        out_fd = open(path, O_WRONLY | O_NONBLOCK);
        out_ep = ep;
    }
    if (in_fd < 0) {
        klog_printf("[AnyController] usb %s: no readable endpoint yet, retrying\n", name);
        if (out_fd >= 0) close(out_fd);
        close(cfd);
        reject_probe(name, now);
        return -1;
    }
    int one = 1;
    ioctl(in_fd, USB_SET_RX_SHORT_XFER, &one);
    klog_printf("[AnyController] usb %s: IN ep%d, OUT %s%d\n", name, in_ep,
                out_fd >= 0 ? "ep" : "none ", out_fd >= 0 ? out_ep : 0);

    slot->ctrl_fd    = cfd;
    slot->in_fd      = in_fd;
    slot->out_fd     = out_fd;
    slot->vendor_id  = vid;
    slot->product_id = pid;
    snprintf(slot->name, NAME_LEN, "%s", name);
    slot->active     = true;

    ctrl->driver    = drv;
    ctrl->transport = TRANSPORT_USB;
    ctrl->fd        = in_fd;
    ctrl->state     = CTRL_STATE_CONNECTING;

    klog_printf("[AnyController] usb %s: opened %s (%04x:%04x)\n",
                name, drv->name, vid, pid);
    return 0;
}

int usb_open_next_controller(Controller *ctrl)
{
    DIR *d = opendir("/dev");
    if (!d) return -1;

    RejectEntry prev[REJECT_MAX];
    int  prev_count = rejected_count;
    memcpy(prev, rejected, sizeof(prev));
    rejected_count = 0;

    uint64_t now = usb_now_us();
    int result = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "ugen", 4) != 0) continue;
        const char *name = e->d_name + 4;

        /* Carry forward what we already know about this node, so the 25 ms
         * rescan neither re-reads descriptors of unsupported devices nor
         * retries a failed probe before its backoff expires. */
        const RejectEntry *was = reject_find(prev, prev_count, name);
        if (was && was->permanent) {
            reject_carry(was);
            continue;
        }
        if (was && now < was->retry_us) {
            reject_carry(was);
            continue;
        }
        if (result == 0 || is_held(name)) continue;
        if (try_open(name, ctrl, now) == 0) result = 0;
    }
    closedir(d);
    return result;
}

int usb_read_report(int fd, uint8_t *buf, int max_len)
{
    int n = read(fd, buf, max_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return n;
}

static UsbDevice *find_by_in_fd(int fd)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].active && usb_devices[i].in_fd == fd)
            return &usb_devices[i];
    return NULL;
}

int usb_write_report(int fd, const uint8_t *buf, int len)
{
    UsbDevice *dev = find_by_in_fd(fd);
    if (!dev || dev->out_fd < 0) return -1;
    int n = write(dev->out_fd, buf, len);
    if (n < 0) return -1;
    return n;
}

static void close_device(UsbDevice *dev)
{
    close(dev->in_fd);
    if (dev->out_fd >= 0) close(dev->out_fd);
    close(dev->ctrl_fd);
    dev->active = false;
}

void usb_close_device(int fd)
{
    UsbDevice *dev = find_by_in_fd(fd);
    if (dev) close_device(dev);
}

void usb_shutdown(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].active)
            close_device(&usb_devices[i]);
}
