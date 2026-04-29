/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Thin compatibility shim so calendarspec.c compiles under the portable-mgr
 * build without pulling in the full systemd internal headers.
 *
 * Provides the exact set of macros/functions that calendarspec.c actually
 * uses from: alloc-util.h, string-util.h, sort-util.h, log.h,
 * memstream-util.h, time-util.h, and the macro/cleanup infrastructure.
 */

#pragma once

#include <alloca.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

#include "psm.h"   /* usec_t, USEC_PER_SEC, isempty, streq, _cleanup_free_, log_* */

/* ---- General cleanup attribute (psm.h only defines specialised variants) ---- */
#define _cleanup_(f) __attribute__((__cleanup__(f)))

/* ---- alloc-util.h ---- */
#define new(t, n)   ((t*) malloc((n) * sizeof(t)))
#define new0(t, n)  ((t*) calloc(1, (n) * sizeof(t)))
#define newa(t, n)  ((t*) alloca((n) * sizeof(t)))

static inline void *mfree(void *p) { free(p); return NULL; }

/* free *a, set a = b, set b = NULL */
#define free_and_replace(a, b) \
        do { void *_old = (void*)(a); (a) = (b); (b) = NULL; free(_old); } while (0)

/* ---- macro.h / cleanup ---- */
#define DEFINE_TRIVIAL_CLEANUP_FUNC(type, func)                         \
        static __attribute__((unused)) void func##p(type *_pp) {        \
                if (*_pp) { func(*_pp); *_pp = NULL; }                  \
        }

#define POINTER_MAY_BE_NULL(p)  /* documentation only */
#define assert_cc(x)            _Static_assert(x, #x)

#define _pure_   __attribute__((__pure__))

#define CMP(a, b) ((a) > (b) ? 1 : ((a) < (b) ? -1 : 0))

/* Steal-pointer: returns the pointer and sets the original to NULL */
#define TAKE_PTR(ptr)                   \
        ({                              \
                typeof(ptr) *_pptr = &(ptr); \
                typeof(ptr) _ptr = *_pptr;   \
                *_pptr = NULL;          \
                _ptr;                   \
        })

/* Assert non-null and return */
#define ASSERT_PTR(expr)                \
        ({                              \
                typeof(expr) _p = (expr); \
                assert(_p);             \
                _p;                     \
        })

#ifndef CONST_MIN
#define CONST_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---- macro.h / array helpers ---- */
#define ELEMENTSOF(x) (sizeof(x) / sizeof((x)[0]))

/* Membership test — GCC statement expression iterates a literal array */
#define IN_SET(x, ...) \
        ({ \
                const typeof(+(x)) _vals[] = { __VA_ARGS__ }; \
                bool _found = false; \
                for (size_t _i = 0; _i < ELEMENTSOF(_vals); _i++) \
                        if (_vals[_i] == (x)) { _found = true; break; } \
                _found; \
        })

/* ---- string-util.h ---- */
/* isempty() already defined in psm.h */
static inline bool streq_ptr(const char *a, const char *b) {
        return (!a && !b) || (a && b && strcmp(a, b) == 0);
}
static inline bool strcaseeq(const char *a, const char *b) {
        return strcasecmp(a, b) == 0;
}
/* Returns pointer past the prefix if s starts with prefix (case-insensitive), else NULL */
static inline const char *startswith_no_case(const char *s, const char *prefix) {
        size_t l = strlen(prefix);
        if (strncasecmp(s, prefix, l) != 0)
                return NULL;
        return s + l;
}

/* Case-insensitive string membership test */
#define STRCASE_IN_SET(x, ...) \
        ({ \
                const char *const _svals[] = { __VA_ARGS__ }; \
                bool _sfound = false; \
                for (size_t _si = 0; _si < ELEMENTSOF(_svals); _si++) \
                        if (strcasecmp(_svals[_si], (x)) == 0) { _sfound = true; break; } \
                _sfound; \
        })

/* ---- sort-util.h ---- */
/* Comparison fn takes ptr-to-element; cast to void* for qsort */
#define typesafe_qsort(arr, n, cmp) \
        qsort((arr), (n), sizeof(*(arr)), (int (*)(const void*, const void*))(cmp))

/* ---- log.h ---- */
/* log_warning() already defined in psm.h.
 * SYNTHETIC_ERRNO just passes the errno through (no synthetic-flag bits). */
#define SYNTHETIC_ERRNO(e) (e)
#define log_warning_errno(err, ...) \
        (log_warning(__VA_ARGS__), -(abs(err)))

/* ---- memstream-util.h ---- */
typedef struct {
        FILE  *f;
        char  *buf;
        size_t sz;
} MemStream;

static inline FILE *memstream_init(MemStream *m) {
        m->buf = NULL;
        m->sz  = 0;
        m->f   = open_memstream(&m->buf, &m->sz);
        return m->f;
}

static inline void memstream_done(MemStream *m) {
        if (m->f) { fclose(m->f); m->f = NULL; }
        free(m->buf); m->buf = NULL;
}

static inline int memstream_finalize(MemStream *m, char **ret_buf, size_t *ret_size) {
        if (fflush(m->f) < 0 || ferror(m->f)) {
                memstream_done(m);
                return -EIO;
        }
        fclose(m->f); m->f = NULL;
        if (ret_buf)  *ret_buf  = m->buf; else free(m->buf);
        m->buf = NULL;
        if (ret_size) *ret_size = m->sz;
        return 0;
}

/* ---- time-util.h (minimal inlines used by calendarspec.c) ---- */

#ifndef TIME_T_MAX
#  define TIME_T_MAX ((time_t)((UINT64_C(1) << (sizeof(time_t) * 8 - 1)) - 1))
#endif

/* Convert struct tm → usec_t (UTC or local).  Mirrors time-util.c. */
static inline int mktime_or_timegm_usec(struct tm *tm, bool utc, usec_t *ret) {
        if (tm->tm_year < 69) /* pre-1970 */
                return -ERANGE;

        time_t t = utc ? timegm(tm) : mktime(tm);
        if (t < 0)
                return -ERANGE;
        if ((usec_t) t >= USEC_INFINITY / USEC_PER_SEC)
                return -ERANGE;

        if (ret) *ret = (usec_t) t * USEC_PER_SEC;
        return 0;
}

/* Convert usec_t → struct tm (UTC or local).  Mirrors time-util.c. */
static inline int localtime_or_gmtime_usec(usec_t t, bool utc, struct tm *ret) {
        usec_t s = t / USEC_PER_SEC;
        if (s > (usec_t) TIME_T_MAX)
                return -ERANGE;
        time_t sec = (time_t) s;
        struct tm buf = {};
        if (!(utc ? gmtime_r(&sec, &buf) : localtime_r(&sec, &buf)))
                return -EINVAL;
        if (ret) *ret = buf;
        return 0;
}

/* USEC_PER_* constants expected by calendarspec.c */
#ifndef USEC_PER_MINUTE
#  define USEC_PER_MINUTE  (60ULL  * USEC_PER_SEC)
#endif
#ifndef USEC_PER_HOUR
#  define USEC_PER_HOUR    (3600ULL * USEC_PER_SEC)
#endif
#ifndef USEC_PER_DAY
#  define USEC_PER_DAY     (86400ULL * USEC_PER_SEC)
#endif
#ifndef USEC_PER_WEEK
#  define USEC_PER_WEEK    (604800ULL * USEC_PER_SEC)
#endif
#ifndef USEC_PER_YEAR
#  define USEC_PER_YEAR    (31557600ULL * USEC_PER_SEC)  /* 365.25 days */
#endif

/* ---- parse-util.h ---- */
#define DIGITS "0123456789"

static inline bool ascii_isdigit(char a) {
        return a >= '0' && a <= '9';
}

/* Parse fractional decimal digits: e.g. ".5" → 500000 for digits=6 */
static inline int parse_fractional_part_u(const char **p, size_t digits, unsigned *res) {
        unsigned val = 0;
        const char *s = *p;
        size_t i;

        for (i = 0; i < digits; i++, s++) {
                if (!ascii_isdigit(*s)) {
                        if (i == 0)
                                return -EINVAL;
                        for (; i < digits; i++)
                                val *= 10;
                        break;
                }
                val *= 10;
                val += (unsigned)(*s - '0');
        }
        /* round up if next digit >= 5 */
        if (*s >= '5' && *s <= '9')
                val++;
        s += strspn(s, DIGITS);
        *p = s;
        *res = val;
        return 0;
}

/* ---- time-util.h ---- */
/* Returns the current timezone name; dst=true for DST name */
static inline const char *get_tzname(bool dst) {
        tzset();
        if (dst && (!tzname[1] || tzname[1][0] == '\0'))
                dst = false;
        return tzname[dst] ? tzname[dst] : "";
}

/* SAVE_TIMEZONE / reset_timezonep — save TZ env var, restore on scope exit */
static inline void reset_timezonep(char **p) {
        if (*p && **p != '\0')
                setenv("TZ", *p, 1);
        else
                unsetenv("TZ");
        tzset();
        free(*p);
        *p = NULL;
}
static inline char *save_timezone(void) {
        const char *tz = getenv("TZ");
        char *s = strdup(tz ? tz : "");
        return s;
}
#define SAVE_TIMEZONE \
        _cleanup_(reset_timezonep) __attribute__((unused)) \
        char *_saved_timezone_ = save_timezone()

/* Max formattable timestamp (9999-12-30 23:59:59 UTC in usec) */
#define USEC_TIMESTAMP_FORMATTABLE_MAX ((usec_t) 253402214399000000ULL)

/* ---- errno-util.h ---- */
static inline int RET_NERRNO(int ret) {
        if (ret < 0)
                return -errno;
        return ret;
}

/* ---- string-util.h ---- */
static inline const char *strna(const char *s) {
        return s ? s : "n/a";
}
static inline const char *endswith_no_case(const char *s, const char *suffix) {
        size_t sl = strlen(s);
        size_t pl = strlen(suffix);
        if (pl == 0)
                return s + sl;
        if (sl < pl)
                return NULL;
        if (strcasecmp(s + sl - pl, suffix) != 0)
                return NULL;
        return s + sl - pl;
}

/* timezone_is_valid — true if name is a valid IANA timezone file under /usr/share/zoneinfo */
static inline bool timezone_is_valid(const char *name, int log_level) {
        (void)log_level;
        if (!name || !name[0])
                return false;
        /* Quick check: no absolute paths, no '..' components */
        if (name[0] == '/')
                return false;
        if (strstr(name, ".."))
                return false;
        /* Check the zoneinfo file exists */
        char path[256];
        snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", name);
        struct stat st;
        return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ---- macro.h ---- */
#define SWAP_TWO(x, y) do { typeof(x) _t = (x); (x) = (y); (y) = (_t); } while (0)
#define ADD_SAFE(ret, a, b) (!__builtin_add_overflow((a), (b), (ret)))
#define ROUND_UP(x, y) \
        ({ typeof(x) _x = (x); typeof(y) _y = (y); (_x + _y - 1) / _y * _y; })
