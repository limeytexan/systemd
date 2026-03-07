/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - event loop implementation
 *
 * Uses poll(2) + self-pipe trick for signals (POSIX; works on Linux and macOS).
 * Process monitoring uses SIGCHLD + waitpid(WNOHANG).
 * Timers use poll's timeout parameter with clock_gettime(CLOCK_MONOTONIC).
 */

#include <poll.h>
#include "event.h"

/* ---- Internal structures ---- */

typedef struct FDWatch {
        int fd;
        bool writable;
        EventIOCallback cb;
        void *userdata;
} FDWatch;

typedef struct ProcWatch {
        pid_t pid;
        EventProcCallback cb;
        void *userdata;
} ProcWatch;

typedef struct SignalWatch {
        int signo;
        EventSignalCallback cb;
        void *userdata;
} SignalWatch;

typedef struct Timer {
        uint64_t id;
        uint64_t fire_at;   /* monotonic_usec() absolute time */
        EventTimerCallback cb;
        void *userdata;
} Timer;

struct EventLoop {
        int signal_pipe[2]; /* [0]=read [1]=write; write from signal handlers */

        FDWatch     fd_watches[PSM_MAX_FD_WATCHES];
        int         n_fd_watches;

        ProcWatch   proc_watches[PSM_MAX_PROC_WATCHES];
        int         n_proc_watches;

        SignalWatch signal_watches[PSM_MAX_SIGNAL_WATCHES];
        int         n_signal_watches;

        Timer       timers[PSM_MAX_TIMERS];
        int         n_timers;
        uint64_t    next_timer_id;

        bool        quit;
        int         exit_code;
};

/* Global reference for async-signal-safe signal handler */
static EventLoop *g_event_loop = NULL;

/* ---- Signal handler ---- */

static void psm_signal_handler(int signo) {
        if (!g_event_loop)
                return;
        /* write() is async-signal-safe */
        unsigned char s = (unsigned char)(signo & 0xff);
        (void)write(g_event_loop->signal_pipe[1], &s, 1);
}

/* ---- Lifecycle ---- */

int event_loop_new(EventLoop **ret) {
        EventLoop *el = NEW0(EventLoop);
        if (!el)
                return -ENOMEM;

        if (pipe2(el->signal_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
                free(el);
                return -errno;
        }

        el->signal_pipe[0] = el->signal_pipe[0];
        el->signal_pipe[1] = el->signal_pipe[1];
        el->next_timer_id = 1;

        *ret = el;
        return 0;
}

void event_loop_free(EventLoop *el) {
        if (!el)
                return;
        if (g_event_loop == el)
                g_event_loop = NULL;
        if (el->signal_pipe[0] >= 0) close(el->signal_pipe[0]);
        if (el->signal_pipe[1] >= 0) close(el->signal_pipe[1]);
        free(el);
}

void event_loop_quit(EventLoop *el, int exit_code) {
        el->quit = true;
        el->exit_code = exit_code;
}

/* ---- FD watching ---- */

int event_loop_watch_fd(EventLoop *el, int fd, bool writable,
                        EventIOCallback cb, void *userdata) {
        if (el->n_fd_watches >= PSM_MAX_FD_WATCHES)
                return -ENOMEM;
        FDWatch *w = &el->fd_watches[el->n_fd_watches++];
        w->fd       = fd;
        w->writable = writable;
        w->cb       = cb;
        w->userdata = userdata;
        return 0;
}

int event_loop_unwatch_fd(EventLoop *el, int fd) {
        for (int i = 0; i < el->n_fd_watches; i++) {
                if (el->fd_watches[i].fd == fd) {
                        el->fd_watches[i] = el->fd_watches[--el->n_fd_watches];
                        return 0;
                }
        }
        return -ENOENT;
}

/* ---- Process watching ---- */

int event_loop_watch_pid(EventLoop *el, pid_t pid,
                         EventProcCallback cb, void *userdata) {
        if (el->n_proc_watches >= PSM_MAX_PROC_WATCHES)
                return -ENOMEM;
        ProcWatch *w = &el->proc_watches[el->n_proc_watches++];
        w->pid      = pid;
        w->cb       = cb;
        w->userdata = userdata;
        return 0;
}

int event_loop_unwatch_pid(EventLoop *el, pid_t pid) {
        for (int i = 0; i < el->n_proc_watches; i++) {
                if (el->proc_watches[i].pid == pid) {
                        el->proc_watches[i] = el->proc_watches[--el->n_proc_watches];
                        return 0;
                }
        }
        return -ENOENT;
}

/* ---- Signal watching ---- */

int event_loop_watch_signal(EventLoop *el, int signo,
                            EventSignalCallback cb, void *userdata) {
        if (el->n_signal_watches >= PSM_MAX_SIGNAL_WATCHES)
                return -ENOMEM;

        /* Install the unified signal handler */
        struct sigaction sa = {
                .sa_handler = psm_signal_handler,
                .sa_flags   = SA_RESTART,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(signo, &sa, NULL);

        g_event_loop = el;

        SignalWatch *w = &el->signal_watches[el->n_signal_watches++];
        w->signo    = signo;
        w->cb       = cb;
        w->userdata = userdata;
        return 0;
}

int event_loop_unwatch_signal(EventLoop *el, int signo) {
        for (int i = 0; i < el->n_signal_watches; i++) {
                if (el->signal_watches[i].signo == signo) {
                        el->signal_watches[i] = el->signal_watches[--el->n_signal_watches];
                        signal(signo, SIG_DFL);
                        return 0;
                }
        }
        return -ENOENT;
}

/* ---- Timers ---- */

int event_loop_add_timer(EventLoop *el, uint64_t delay_usec,
                         EventTimerCallback cb, void *userdata,
                         uint64_t *ret_id) {
        if (el->n_timers >= PSM_MAX_TIMERS)
                return -ENOMEM;
        Timer *t  = &el->timers[el->n_timers++];
        t->id       = el->next_timer_id++;
        t->fire_at  = monotonic_usec() + delay_usec;
        t->cb       = cb;
        t->userdata = userdata;
        if (ret_id)
                *ret_id = t->id;
        return 0;
}

int event_loop_cancel_timer(EventLoop *el, uint64_t id) {
        for (int i = 0; i < el->n_timers; i++) {
                if (el->timers[i].id == id) {
                        el->timers[i] = el->timers[--el->n_timers];
                        return 0;
                }
        }
        return -ENOENT;
}

/* ---- Internal helpers ---- */

/* Calculate poll timeout in ms from next pending timer (-1 = infinite) */
static int calc_timeout_ms(const EventLoop *el) {
        if (el->n_timers == 0)
                return -1;

        uint64_t now = monotonic_usec();
        uint64_t earliest = UINT64_MAX;
        for (int i = 0; i < el->n_timers; i++) {
                if (el->timers[i].fire_at < earliest)
                        earliest = el->timers[i].fire_at;
        }

        if (earliest <= now)
                return 0;

        uint64_t diff_usec = earliest - now;
        /* Convert to ms, rounding up, clamped to INT_MAX */
        if (diff_usec / 1000 > (uint64_t)INT_MAX)
                return INT_MAX;
        return (int)((diff_usec + 999) / 1000);
}

/* Fire all timers whose deadline has passed */
static void fire_timers(EventLoop *el) {
        uint64_t now = monotonic_usec();
        for (int i = 0; i < el->n_timers; ) {
                if (el->timers[i].fire_at <= now) {
                        EventTimerCallback cb = el->timers[i].cb;
                        void *ud              = el->timers[i].userdata;
                        uint64_t id           = el->timers[i].id;
                        /* Remove before calling so callback can re-arm */
                        el->timers[i] = el->timers[--el->n_timers];
                        cb(el, id, ud);
                        /* Don't advance i; check the slot we just filled */
                } else {
                        i++;
                }
        }
}

/* Reap dead children and notify watchers */
static void handle_sigchld(EventLoop *el) {
        for (;;) {
                int status;
                pid_t pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0)
                        break;

                for (int i = 0; i < el->n_proc_watches; i++) {
                        if (el->proc_watches[i].pid == pid) {
                                EventProcCallback cb = el->proc_watches[i].cb;
                                void *ud             = el->proc_watches[i].userdata;
                                /* Remove before calling so callback can re-add */
                                el->proc_watches[i] = el->proc_watches[--el->n_proc_watches];
                                cb(el, pid, status, ud);
                                i--; /* Check the slot we just filled */
                                break;
                        }
                }
        }
}

/* Drain signal pipe and dispatch signal callbacks */
static void handle_signal_pipe(EventLoop *el) {
        unsigned char buf[64];
        ssize_t n;

        while ((n = read(el->signal_pipe[0], buf, sizeof(buf))) > 0) {
                for (ssize_t i = 0; i < n; i++) {
                        int signo = (int)buf[i];
                        if (signo == SIGCHLD) {
                                handle_sigchld(el);
                                continue;
                        }
                        for (int j = 0; j < el->n_signal_watches; j++) {
                                if (el->signal_watches[j].signo == signo) {
                                        el->signal_watches[j].cb(el, signo,
                                                el->signal_watches[j].userdata);
                                        break;
                                }
                        }
                }
        }
}

/* ---- Main loop ---- */

int event_loop_run(EventLoop *el) {
        /* Register SIGCHLD handler so we can reap children */
        g_event_loop = el;
        struct sigaction sa = {
                .sa_handler = psm_signal_handler,
                .sa_flags   = SA_RESTART | SA_NOCLDSTOP,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGCHLD, &sa, NULL);

        while (!el->quit) {
                /* Build pollfd array: signal pipe + fd watches */
                struct pollfd pfds[1 + PSM_MAX_FD_WATCHES];
                int npfds = 0;

                pfds[npfds].fd      = el->signal_pipe[0];
                pfds[npfds].events  = POLLIN;
                pfds[npfds].revents = 0;
                npfds++;

                for (int i = 0; i < el->n_fd_watches; i++) {
                        pfds[npfds].fd      = el->fd_watches[i].fd;
                        pfds[npfds].events  = el->fd_watches[i].writable ? POLLOUT : POLLIN;
                        pfds[npfds].revents = 0;
                        npfds++;
                }

                int timeout = calc_timeout_ms(el);
                int r = poll(pfds, (nfds_t)npfds, timeout);
                if (r < 0) {
                        if (errno == EINTR)
                                continue;
                        log_error("poll: %s", strerror(errno));
                        return -errno;
                }

                /* Always check timers after poll returns */
                fire_timers(el);

                if (el->quit)
                        break;

                /* Signal pipe */
                if (pfds[0].revents & POLLIN)
                        handle_signal_pipe(el);

                if (el->quit)
                        break;

                /* FD watches - iterate with care since callbacks can modify el */
                int n_watches = el->n_fd_watches;
                for (int i = 0; i < n_watches && !el->quit; i++) {
                        short rev = pfds[i + 1].revents;
                        if (rev & (POLLIN | POLLOUT | POLLHUP | POLLERR)) {
                                int fd = el->fd_watches[i].fd;
                                EventIOCallback cb = el->fd_watches[i].cb;
                                void *ud = el->fd_watches[i].userdata;
                                cb(el, fd, ud);
                        }
                }
        }

        return el->exit_code;
}
