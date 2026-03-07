/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - cross-platform event loop (poll + self-pipe) */
#pragma once

#include "psm.h"

typedef struct EventLoop EventLoop;

/* Callback types */
typedef void (*EventIOCallback)(EventLoop *el, int fd, void *userdata);
typedef void (*EventProcCallback)(EventLoop *el, pid_t pid, int status, void *userdata);
typedef void (*EventSignalCallback)(EventLoop *el, int signo, void *userdata);
typedef void (*EventTimerCallback)(EventLoop *el, uint64_t id, void *userdata);

/* Lifecycle */
int  event_loop_new(EventLoop **ret);
void event_loop_free(EventLoop *el);
int  event_loop_run(EventLoop *el);
void event_loop_quit(EventLoop *el, int exit_code);

/* File descriptor watching */
int event_loop_watch_fd(EventLoop *el, int fd, bool writable,
                        EventIOCallback cb, void *userdata);
int event_loop_unwatch_fd(EventLoop *el, int fd);

/* Process monitoring (via SIGCHLD + waitpid) */
int event_loop_watch_pid(EventLoop *el, pid_t pid,
                         EventProcCallback cb, void *userdata);
int event_loop_unwatch_pid(EventLoop *el, pid_t pid);

/* Signal delivery (via self-pipe trick) */
int event_loop_watch_signal(EventLoop *el, int signo,
                            EventSignalCallback cb, void *userdata);
int event_loop_unwatch_signal(EventLoop *el, int signo);

/* One-shot timers; *ret_id is set to cancellation token */
int event_loop_add_timer(EventLoop *el, uint64_t delay_usec,
                         EventTimerCallback cb, void *userdata,
                         uint64_t *ret_id);
int event_loop_cancel_timer(EventLoop *el, uint64_t id);
