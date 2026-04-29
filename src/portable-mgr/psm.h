/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - common types, macros, and portability layer */
#pragma once

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- Portability ---- */

#ifdef __APPLE__
#  include <sys/wait.h>
#  include <sys/socket.h>
#  include <sys/un.h>
/* MSG_NOSIGNAL is Linux-specific; on macOS use SO_NOSIGPIPE per-socket */
#  ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#  endif
/* pipe2() is not available on macOS */
static inline int psm_pipe2(int pipefd[2], int flags) {
        if (pipe(pipefd) < 0) return -1;
        if (flags & O_CLOEXEC) {
                fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
                fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
        }
        if (flags & O_NONBLOCK) {
                fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
                fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
        }
        return 0;
}
#  define pipe2(fds, flags) psm_pipe2(fds, flags)
#else
#  include <sys/wait.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#endif

/* ---- Version ---- */
#define PSM_VERSION "0.1"

/* ---- Time type (matches systemd's usec_t = uint64_t) ---- */
typedef uint64_t usec_t;

/* ---- Limits ---- */
#define PSM_MAX_UNITS           512
#define PSM_MAX_EXEC_ARGS        64
#define PSM_MAX_ENV_VARS        256
#define PSM_MAX_DEPS             32
#define PSM_MAX_TIMERS          128
#define PSM_MAX_FD_WATCHES       64
#define PSM_MAX_PROC_WATCHES    128
#define PSM_MAX_SIGNAL_WATCHES   32

/* ---- Unit types ---- */
typedef enum UnitType {
        UNIT_SERVICE,
        UNIT_TARGET,
        UNIT_SOCKET,
        UNIT_TIMER,
        _UNIT_TYPE_MAX,
        _UNIT_TYPE_INVALID = -1,
} UnitType;

/* ---- Unit active states ---- */
typedef enum UnitActiveState {
        UNIT_INACTIVE,
        UNIT_ACTIVATING,
        UNIT_ACTIVE,
        UNIT_DEACTIVATING,
        UNIT_FAILED,
        UNIT_RELOADING,
        _UNIT_ACTIVE_STATE_MAX,
} UnitActiveState;

/* ---- Service-specific types ---- */
typedef enum ServiceType {
        SERVICE_SIMPLE,    /* ExecStart is main process */
        SERVICE_FORKING,   /* Main process forks; parent exits */
        SERVICE_ONESHOT,   /* Runs once; considered done on exit */
        SERVICE_NOTIFY,    /* Like simple; sends sd_notify("READY=1") */
        SERVICE_EXEC,      /* Like simple; considered started after exec() */
        SERVICE_IDLE,      /* Like simple; delayed until all active jobs done */
        _SERVICE_TYPE_MAX,
        _SERVICE_TYPE_INVALID = -1,
} ServiceType;

typedef enum ServiceRestart {
        SERVICE_RESTART_NO,
        SERVICE_RESTART_ON_SUCCESS,
        SERVICE_RESTART_ON_FAILURE,
        SERVICE_RESTART_ON_ABNORMAL,
        SERVICE_RESTART_ON_ABORT,
        SERVICE_RESTART_ALWAYS,
        _SERVICE_RESTART_MAX,
        _SERVICE_RESTART_INVALID = -1,
} ServiceRestart;

typedef enum KillMode {
        KILL_CONTROL_GROUP,
        KILL_PROCESS,
        KILL_MIXED,
        KILL_NONE,
        _KILL_MODE_MAX,
} KillMode;

/* ---- Utility macros ---- */
#define NEW0(t)         ((t*)calloc(1, sizeof(t)))
#define NEWN(t, n)      ((t*)calloc((n), sizeof(t)))
#define streq(a, b)     (strcmp((a), (b)) == 0)
#define strneq(a,b,n)   (strncmp((a),(b),(n)) == 0)
#define strprefix(s,p)  (strncmp((s),(p),strlen(p)) == 0)
#define isempty(s)      (!(s) || (s)[0] == '\0')
#define ARRAY_SIZE(a)   (sizeof(a)/sizeof((a)[0]))
#ifndef MIN
#define MIN(a,b)        ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b)        ((a)>(b)?(a):(b))
#endif
#define CLAMP(x,lo,hi)  MIN(MAX(x,lo),hi)

/* GCC/Clang cleanup attribute helpers */
static inline void freep(void *p) { free(*(void**)p); }
#define _cleanup_free_   __attribute__((__cleanup__(freep)))

static inline void fclosep(FILE **f) { if (*f) { fclose(*f); *f = NULL; } }
#define _cleanup_fclose_ __attribute__((__cleanup__(fclosep)))

static inline void closep(int *fd) { if (*fd >= 0) { close(*fd); *fd = -1; } }
#define _cleanup_close_  __attribute__((__cleanup__(closep)))

/* ---- Logging ---- */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

extern int psm_log_level;

void psm_logv(int priority, const char *unit, const char *fmt, va_list ap);
void psm_log(int priority, const char *unit, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

#define log_error(fmt, ...)       psm_log(LOG_ERR,     NULL, fmt, ##__VA_ARGS__)
#define log_warning(fmt, ...)     psm_log(LOG_WARNING,  NULL, fmt, ##__VA_ARGS__)
#define log_notice(fmt, ...)      psm_log(LOG_NOTICE,   NULL, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)        psm_log(LOG_INFO,     NULL, fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...)       psm_log(LOG_DEBUG,    NULL, fmt, ##__VA_ARGS__)

#define log_error_unit(u, fmt, ...)   psm_log(LOG_ERR,     (u), fmt, ##__VA_ARGS__)
#define log_warning_unit(u, fmt, ...) psm_log(LOG_WARNING, (u), fmt, ##__VA_ARGS__)
#define log_notice_unit(u, fmt, ...)  psm_log(LOG_NOTICE,  (u), fmt, ##__VA_ARGS__)
#define log_info_unit(u, fmt, ...)    psm_log(LOG_INFO,    (u), fmt, ##__VA_ARGS__)
#define log_debug_unit(u, fmt, ...)   psm_log(LOG_DEBUG,   (u), fmt, ##__VA_ARGS__)

/* ---- Time utilities ---- */

/* Realtime clock in microseconds since epoch */
static inline uint64_t now_usec(void) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Monotonic clock in microseconds */
static inline uint64_t monotonic_usec(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

#define USEC_PER_SEC  1000000ULL
#define USEC_PER_MSEC    1000ULL
#define USEC_INFINITY  UINT64_MAX

/* ---- String helpers ---- */

static inline char *xstrdup(const char *s) {
        if (!s) return NULL;
        return strdup(s);
}

/* Duplicate a string and assign, freeing old value */
static inline int strdup_assign(char **dest, const char *src) {
        char *n = src ? strdup(src) : NULL;
        if (src && !n) return -ENOMEM;
        free(*dest);
        *dest = n;
        return 0;
}

/* Trim leading and trailing whitespace in-place; returns pointer into s */
static inline char *strstrip(char *s) {
        if (!s) return s;
        while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
                s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
                e--;
        *e = '\0';
        return s;
}

/* NULL-terminated array of strings */
static inline size_t strv_length(char **v) {
        size_t n = 0;
        if (!v) return 0;
        while (v[n]) n++;
        return n;
}

static inline void strv_free(char **v) {
        if (!v) return;
        for (char **p = v; *p; p++)
                free(*p);
        free(v);
}

/* Append a string to a NULL-terminated array; returns new array or NULL on OOM */
static inline char **strv_append(char **v, const char *s) {
        size_t n = strv_length(v);
        char **nv = realloc(v, (n + 2) * sizeof(char*));
        if (!nv) return NULL;
        nv[n] = strdup(s);
        if (!nv[n]) { free(nv); return NULL; }
        nv[n+1] = NULL;
        return nv;
}

/* ---- Path helpers ---- */

/* Get the runtime directory; caller must free result */
char *psm_runtime_dir(void);
/* Get the user config dir; caller must free result */
char *psm_config_dir(void);
/* Get the data dir; caller must free result */
char *psm_data_dir(void);
/* Ensure a directory exists (creating parents as needed); returns 0 or -errno */
int psm_mkdir_p(const char *path, mode_t mode);

/* ---- fd helpers ---- */

static inline int set_cloexec(int fd) {
        int f = fcntl(fd, F_GETFD);
        if (f < 0) return -errno;
        return fcntl(fd, F_SETFD, f | FD_CLOEXEC) < 0 ? -errno : 0;
}

static inline int set_nonblock(int fd) {
        int f = fcntl(fd, F_GETFL);
        if (f < 0) return -errno;
        return fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0 ? -errno : 0;
}
