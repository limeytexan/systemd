/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - journal implementation
 *
 * Writes structured log entries as newline-delimited JSON.
 * Each service gets its own log file: <unit>.journal
 * A combined log file "system.journal" receives all entries.
 *
 * JSON format (one line per entry):
 * {"__REALTIME_TIMESTAMP":"1234567890","__MONOTONIC_TIMESTAMP":"123456",
 *  "PRIORITY":"6","_SYSTEMD_UNIT":"foo.service","_PID":"1234",
 *  "SYSLOG_IDENTIFIER":"foo","MESSAGE":"hello"}
 */

#include <stdarg.h>
#include <sys/stat.h>
#include "journal.h"

struct Journal {
        char *dir;
        FILE *system_log;   /* Combined log file */
};

char *journal_dir(void) {
        _cleanup_free_ char *runtime = psm_runtime_dir();
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/psm/journal", runtime);
        return strdup(buf);
}

int journal_open(Journal **ret, const char *dir) {
        Journal *j = NEW0(Journal);
        if (!j) return -ENOMEM;

        j->dir = strdup(dir ? dir : "");
        if (!j->dir) { free(j); return -ENOMEM; }

        /* Ensure the directory exists */
        if (psm_mkdir_p(j->dir, 0700) < 0) {
                /* Non-fatal: we'll log to stderr */
                log_warning("journal: cannot create log dir %s: %s", j->dir, strerror(errno));
        }

        /* Open combined system.journal */
        char path[4096];
        snprintf(path, sizeof(path), "%s/system.journal", j->dir);
        j->system_log = fopen(path, "ae");  /* append, close-on-exec */
        if (!j->system_log) {
                log_warning("journal: cannot open %s: %s", path, strerror(errno));
                /* Continue without file logging */
        }

        *ret = j;
        return 0;
}

void journal_close(Journal *j) {
        if (!j) return;
        if (j->system_log) fclose(j->system_log);
        free(j->dir);
        free(j);
}

/* JSON-escape a string into buf (buf must be at least 2*strlen(s)+3 bytes) */
static void json_escape(char *buf, size_t bufsz, const char *s) {
        size_t i = 0;
        buf[i++] = '"';
        for (; *s && i + 4 < bufsz; s++) {
                unsigned char c = (unsigned char)*s;
                if (c == '"') { buf[i++] = '\\'; buf[i++] = '"'; }
                else if (c == '\\') { buf[i++] = '\\'; buf[i++] = '\\'; }
                else if (c == '\n') { buf[i++] = '\\'; buf[i++] = 'n'; }
                else if (c == '\r') { buf[i++] = '\\'; buf[i++] = 'r'; }
                else if (c == '\t') { buf[i++] = '\\'; buf[i++] = 't'; }
                else if (c < 0x20) {
                        i += (size_t)snprintf(buf + i, bufsz - i, "\\u%04x", c);
                } else {
                        buf[i++] = (char)c;
                }
        }
        buf[i++] = '"';
        buf[i]   = '\0';
}

int journal_write(
        Journal    *j,
        int         priority,
        const char *unit,
        pid_t       pid,
        const char *identifier,
        const char *message) {

        uint64_t rt  = now_usec();
        uint64_t mon = monotonic_usec();

        /* Build the JSON line */
        char msg_esc[8192];
        json_escape(msg_esc, sizeof(msg_esc), message ? message : "");

        char unit_esc[256];
        if (unit)
                json_escape(unit_esc, sizeof(unit_esc), unit);

        char id_esc[256];
        if (identifier)
                json_escape(id_esc, sizeof(id_esc), identifier);

        char line[16384];
        int n = snprintf(line, sizeof(line),
                "{\"__REALTIME_TIMESTAMP\":\"%"PRIu64"\","
                "\"__MONOTONIC_TIMESTAMP\":\"%"PRIu64"\","
                "\"PRIORITY\":\"%d\"",
                rt, mon, priority);

        if (unit)
                n += snprintf(line + n, sizeof(line) - (size_t)n,
                        ",\"_SYSTEMD_UNIT\":%s", unit_esc);
        if (pid > 0)
                n += snprintf(line + n, sizeof(line) - (size_t)n,
                        ",\"_PID\":\"%d\"", (int)pid);
        if (identifier)
                n += snprintf(line + n, sizeof(line) - (size_t)n,
                        ",\"SYSLOG_IDENTIFIER\":%s", id_esc);

        n += snprintf(line + n, sizeof(line) - (size_t)n,
                ",\"MESSAGE\":%s}\n", msg_esc);

        /* Write to system journal */
        if (j && j->system_log) {
                fputs(line, j->system_log);
                fflush(j->system_log);
        }

        /* Write to per-unit journal if unit is known */
        if (j && unit && j->dir[0]) {
                char upath[4096];
                /* Replace '/' in unit name with '_' to get a safe filename */
                char safe_unit[256];
                size_t ui = 0;
                for (const char *up = unit; *up && ui + 1 < sizeof(safe_unit); up++, ui++)
                        safe_unit[ui] = (*up == '/') ? '_' : *up;
                safe_unit[ui] = '\0';

                snprintf(upath, sizeof(upath), "%s/%s.journal", j->dir, safe_unit);
                FILE *uf = fopen(upath, "ae");
                if (uf) {
                        fputs(line, uf);
                        fclose(uf);
                }
        }

        /* Also print to stderr for priority <= WARNING */
        if (priority <= LOG_WARNING) {
                const char *pri_name;
                switch (priority) {
                case LOG_EMERG:   pri_name = "EMERG";   break;
                case LOG_ALERT:   pri_name = "ALERT";   break;
                case LOG_CRIT:    pri_name = "CRIT";    break;
                case LOG_ERR:     pri_name = "ERR";     break;
                case LOG_WARNING: pri_name = "WARNING"; break;
                default:          pri_name = "NOTICE";  break;
                }
                if (unit)
                        fprintf(stderr, "%s %s: %s\n", pri_name, unit, message ? message : "");
                else
                        fprintf(stderr, "%s: %s\n", pri_name, message ? message : "");
        }

        return 0;
}

int journal_writef(
        Journal *j, int priority, const char *unit, pid_t pid,
        const char *identifier, const char *fmt, ...) {
        char buf[4096];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        return journal_write(j, priority, unit, pid, identifier, buf);
}

/* ---- Reader ---- */

struct JournalReader {
        char  *dir;
        FILE  *f;           /* Currently open file */
        char  *current_file;
        long   start_offset; /* For seek_tail */
};

int journal_reader_open(JournalReader **ret, const char *dir) {
        JournalReader *r = NEW0(JournalReader);
        if (!r) return -ENOMEM;

        r->dir = strdup(dir ? dir : "");
        if (!r->dir) { free(r); return -ENOMEM; }

        /* Default: open system.journal */
        char path[4096];
        snprintf(path, sizeof(path), "%s/system.journal", r->dir);
        r->f = fopen(path, "r");
        r->current_file = strdup(path);

        *ret = r;
        return 0;
}

/* Open a per-unit reader */
int journal_reader_open_unit(JournalReader **ret, const char *dir, const char *unit) {
        JournalReader *r = NEW0(JournalReader);
        if (!r) return -ENOMEM;

        r->dir = strdup(dir ? dir : "");
        if (!r->dir) { free(r); return -ENOMEM; }

        char safe_unit[256];
        size_t ui = 0;
        for (const char *up = unit; *up && ui + 1 < sizeof(safe_unit); up++, ui++)
                safe_unit[ui] = (*up == '/') ? '_' : *up;
        safe_unit[ui] = '\0';

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s.journal", r->dir, safe_unit);
        r->f = fopen(path, "r");
        r->current_file = strdup(path);

        *ret = r;
        return 0;
}

void journal_reader_close(JournalReader *r) {
        if (!r) return;
        if (r->f) fclose(r->f);
        free(r->dir);
        free(r->current_file);
        free(r);
}

void journal_entry_clear(JournalEntry *e) {
        if (!e) return;
        free(e->unit);
        free(e->message);
        free(e->identifier);
        memset(e, 0, sizeof(*e));
}

/* Parse a JSON field "key":value from a line, return pointer to value string.
 * For simplicity, we look for "key":"value" or "key":number patterns. */
static const char *json_get_field(const char *line, const char *key) {
        /* Build search pattern: "key": */
        char pattern[128];
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);

        const char *p = strstr(line, pattern);
        if (!p) return NULL;
        p += strlen(pattern);

        /* Skip whitespace */
        while (*p == ' ') p++;
        return p;
}

/* Extract a string value from JSON: "..." → copy without quotes/escapes */
static char *json_extract_string(const char *p) {
        if (!p || *p != '"') return NULL;
        p++; /* skip opening quote */

        char buf[8192];
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < sizeof(buf)) {
                if (*p == '\\' && *(p+1)) {
                        p++;
                        switch (*p) {
                        case '"':  buf[i++] = '"'; break;
                        case '\\': buf[i++] = '\\'; break;
                        case 'n':  buf[i++] = '\n'; break;
                        case 'r':  buf[i++] = '\r'; break;
                        case 't':  buf[i++] = '\t'; break;
                        default:   buf[i++] = *p;   break;
                        }
                } else {
                        buf[i++] = *p;
                }
                p++;
        }
        buf[i] = '\0';
        return strdup(buf);
}

/* Extract a number from JSON (quoted or unquoted) */
static uint64_t json_extract_uint64(const char *p) {
        if (!p) return 0;
        if (*p == '"') p++;
        char *end;
        uint64_t v = (uint64_t)strtoull(p, &end, 10);
        return v;
}

int journal_reader_next(JournalReader *r, JournalEntry *ret) {
        if (!r || !r->f)
                return 1; /* EOF */

        char line[16384];
        while (fgets(line, sizeof(line), r->f)) {
                /* Strip trailing newline */
                size_t len = strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                        line[--len] = '\0';
                if (!len) continue;

                /* Parse JSON fields */
                JournalEntry e = {0};

                const char *rt_s = json_get_field(line, "__REALTIME_TIMESTAMP");
                e.realtime_usec = json_extract_uint64(rt_s);

                const char *mon_s = json_get_field(line, "__MONOTONIC_TIMESTAMP");
                e.monotonic_usec = json_extract_uint64(mon_s);

                const char *pri_s = json_get_field(line, "PRIORITY");
                e.priority = pri_s ? (int)json_extract_uint64(pri_s) : LOG_INFO;

                const char *unit_s = json_get_field(line, "_SYSTEMD_UNIT");
                e.unit = json_extract_string(unit_s);

                const char *msg_s = json_get_field(line, "MESSAGE");
                e.message = json_extract_string(msg_s);

                const char *id_s = json_get_field(line, "SYSLOG_IDENTIFIER");
                e.identifier = json_extract_string(id_s);

                const char *pid_s = json_get_field(line, "_PID");
                e.pid = pid_s ? (pid_t)json_extract_uint64(pid_s) : 0;

                *ret = e;
                return 0;
        }

        return 1; /* EOF */
}

int journal_reader_seek_realtime(JournalReader *r, uint64_t usec) {
        if (!r || !r->f) return -EINVAL;
        rewind(r->f);

        /* Skip entries until we find one with timestamp >= usec */
        char line[16384];
        long last_ok = 0;

        while (fgets(line, sizeof(line), r->f)) {
                const char *rt_s = json_get_field(line, "__REALTIME_TIMESTAMP");
                uint64_t rt = json_extract_uint64(rt_s);
                if (rt >= usec) {
                        /* Seek back to beginning of this line */
                        fseek(r->f, last_ok, SEEK_SET);
                        return 0;
                }
                last_ok = ftell(r->f);
        }
        return 0; /* Will return EOF on first next() call */
}

int journal_reader_fileno(JournalReader *r) {
        if (!r || !r->f) return -1;
        return fileno(r->f);
}

int journal_reader_seek_tail(JournalReader *r, int n) {
        if (!r || !r->f || n <= 0) return -EINVAL;
        if (n == 0) {
                fseek(r->f, 0, SEEK_END);
                return 0;
        }

        /* Count total lines */
        rewind(r->f);
        int total = 0;
        char line[16384];
        while (fgets(line, sizeof(line), r->f)) {
                if (line[0] != '\n' && line[0] != '\0')
                        total++;
        }

        /* Seek to (total - n)th line */
        rewind(r->f);
        int skip = total - n;
        if (skip <= 0) {
                rewind(r->f);
                return 0;
        }
        int cnt = 0;
        while (fgets(line, sizeof(line), r->f)) {
                if (line[0] != '\n' && line[0] != '\0') {
                        cnt++;
                        if (cnt >= skip) break;
                }
        }
        return 0;
}
