/* wisp-lock — standalone session locker.
 *
 * Split out from the wisp daemon so that:
 *   - a crash in the daemon never leaves the session unlocked;
 *   - a crash in the locker never leaves the user permanently locked out
 *     (wisp keeps running; the user can SIGKILL wisp-lock from a TTY and
 *      their session is recoverable).
 *
 * Architecture:
 *   - own Wayland connection (wl.c), no ctl socket, no dbus, no D-Bus.
 *   - one process per lock attempt; spawned by `wispctl lock` via execvp.
 *   - reuses the lock state machine in src/lock.c (per-output W_LOCK widgets,
 *     PAM via the existing wisp-lock-helper).
 *   - styling driven by the DSL: `lock { … }` lowers to LOCK_* macros in
 *     gen_overrides.h, which is force-included at build time exactly like in
 *     the daemon build.
 *
 * Lifecycle:
 *   1. connect to Wayland, ensure ext_session_lock_manager_v1 is present.
 *   2. lock_engage() — one lock_surface per output.
 *   3. dispatch loop on { wl_fd, helper_fd, key_rep_tfd, signal_fd }.
 *   4. on successful unlock the lock module tears everything down and
 *      lock_active() returns 0; we then drain the wl socket once and exit 0.
 *   5. on SIGTERM/SIGINT we exit non-zero WITHOUT issuing unlock — the
 *      session stays locked. To intentionally unlock from the outside, kill
 *      the helper or send SIGKILL after authenticating. */

#include "wisp.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int ep_fd = -1;

void epoll_add_fd(int fd) {
    if (ep_fd < 0 || fd < 0) return;
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &ev);
}
void epoll_del_fd(int fd) {
    if (ep_fd < 0 || fd < 0) return;
    epoll_ctl(ep_fd, EPOLL_CTL_DEL, fd, NULL);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Block SIGINT/SIGTERM/SIGCHLD via signalfd so we can drive them from
     * epoll instead of async handlers (would race with wl I/O otherwise). */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);

    wl_connect();
    if (!id_compositor) die("compositor missing");
    if (!id_shm)        die("wl_shm missing");
    if (!id_slock_mgr)  die("ext_session_lock_manager_v1 missing");
    if (output_count() == 0) die("no outputs");

    ep_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ep_fd < 0) die("epoll_create1: %s", strerror(errno));

    int sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sig_fd < 0) die("signalfd: %s", strerror(errno));

    key_rep_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

    epoll_add_fd(wl_fd);
    epoll_add_fd(sig_fd);
    if (key_rep_tfd >= 0) epoll_add_fd(key_rep_tfd);

    lock_engage();
    if (!lock_active()) return 1;

    while (lock_active()) {
        struct epoll_event evs[8];
        int n = epoll_wait(ep_fd, evs, 8, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait: %s", strerror(errno));
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == wl_fd) {
                if (wl_recv(0) < 0) die("wl recv: %s", strerror(errno));
                wl_dispatch();
            } else if (fd == sig_fd) {
                struct signalfd_siginfo si;
                while (read(sig_fd, &si, sizeof si) == (ssize_t)sizeof si) {
                    if (si.ssi_signo == SIGCHLD) {
                        int st; while (waitpid(-1, &st, WNOHANG) > 0) {}
                        continue;
                    }
                    /* SIGINT/SIGTERM — refuse to exit while locked. The
                     * session must stay locked even if init tries to clean
                     * us up; only PAM success ends the lock. */
                }
            } else if (fd == lock_helper_fd()) {
                lock_on_helper_event();
            } else if (fd == key_rep_tfd) {
                uint64_t exp; (void)!read(key_rep_tfd, &exp, sizeof exp);
                if (key_rep_key) lock_on_key(NULL, key_rep_key, 1, 0);
            }
        }
    }

    /* Flush any pending wl writes before exit. wl_send is synchronous so
     * unlock_and_destroy has already reached the kernel; nothing more to do. */
    return 0;
}
