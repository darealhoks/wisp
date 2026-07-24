/* rtnetlink link/addr/route monitor — replaces the 1 Hz vpn()/net() poll.
 *
 * One AF_NETLINK NETLINK_ROUTE socket subscribed to RTMGRP_LINK |
 * RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE. A wg/tun interface appearing or
 * vanishing, a carrier flip, an address change, or the default route moving
 * wakes us; we don't parse the payload — the event is just a "go re-sample"
 * trigger for status.c. Kernel-owned socket, so a failure to open it is
 * fatal (die), consistent with the other transports. */
#include "wisp.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

int nl_fd = -1;
int uev_fd = -1;

/* Emitted by wispc only when a config uses vpn()/net(); weak so a build that
 * links netlink.c without such a source (there is none today) still resolves. */
void wispgen_netlink_changed(void) __attribute__((weak));
/* Emitted by wispc only when a config uses bat() / backlight(); weak likewise. */
void wispgen_uevent_power(void) __attribute__((weak));
void wispgen_uevent_backlight(void) __attribute__((weak));

void nl_init(void) {
    nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (nl_fd < 0) die("netlink: socket: %s", strerror(errno));
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK,
                              .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE };
    if (bind(nl_fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("netlink: bind: %s", strerror(errno));
}

void nl_dispatch(void) {
    /* Aligned so NLMSG_* pointer math is well-defined on the raw bytes. */
    char buf[8192] __attribute__((aligned(NLMSG_ALIGNTO)));
    for (;;) {
        ssize_t n = recv(nl_fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            die("netlink: recv: %s", strerror(errno));
        }
        if (n == 0) break;
        int changed = 0;
        /* NLMSG_OK bounds-checks each header against the remaining length. */
        for (struct nlmsghdr *nh = (struct nlmsghdr *)buf;
             NLMSG_OK(nh, (size_t)n); nh = NLMSG_NEXT(nh, n)) {
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) continue;
            switch (nh->nlmsg_type) {
            case RTM_NEWLINK: case RTM_DELLINK:
            case RTM_NEWADDR: case RTM_DELADDR:
            case RTM_NEWROUTE: case RTM_DELROUTE: changed = 1; break;
            default: break;
            }
        }
        if (changed && wispgen_netlink_changed) wispgen_netlink_changed();
    }
}

/* Kernel uevent monitor (NETLINK_KOBJECT_UEVENT, multicast group 1) — instant
 * AC plug/unplug and charging flips for bat(), brightness writes for
 * backlight() (the backlight class emits a change uevent). No root needed; a bind
 * failure is fatal like the route socket. Payload is untrusted-ish: the format
 * is "action@devpath\0KEY=VALUE\0KEY=VALUE\0…" (or libudev's "libudev\0…"
 * monitor frames), which we walk with strict bounds. */
void uev_init(void) {
    uev_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (uev_fd < 0) die("uevent: socket: %s", strerror(errno));
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    if (bind(uev_fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("uevent: bind: %s", strerror(errno));
}

void uev_dispatch(void) {
    char buf[8192];
    for (;;) {
        ssize_t n = recv(uev_fd, buf, sizeof buf - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            die("uevent: recv: %s", strerror(errno));
        }
        if (n == 0) break;
        buf[n] = 0;
        int power = 0, backlight = 0;
        /* Skip the leading prefix line (up to its NUL), then scan key=value
         * records; only the SUBSYSTEM keys we consume matter. */
        size_t i = 0;
        while (i < (size_t)n && buf[i]) i++;
        for (i++; i < (size_t)n; i += strlen(buf + i) + 1) {
            if (!strcmp(buf + i, "SUBSYSTEM=power_supply")) { power = 1; break; }
            if (!strcmp(buf + i, "SUBSYSTEM=backlight"))    { backlight = 1; break; }
        }
        if (power && wispgen_uevent_power) wispgen_uevent_power();
        if (backlight && wispgen_uevent_backlight) wispgen_uevent_backlight();
    }
}
