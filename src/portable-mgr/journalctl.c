/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - journalctl
 *
 * Reads the portable service manager's structured log files and
 * displays them in a human-readable or JSON format.
 *
 * Supported options:
 *   -u, --unit=UNIT          Show entries from this unit
 *   -f, --follow             Follow new log entries
 *   -n, --lines=N            Show last N lines (default 50)
 *   -p, --priority=LEVEL     Show entries with >= priority
 *   --since=TIME             Show entries since TIME
 *   --until=TIME             Show entries until TIME
 *   -o, --output=FORMAT      Output format: short (default), json, cat
 *   --no-pager               Don't use pager
 *   -q, --quiet              Suppress informational output
 *   --disk-usage             Show disk usage of journal
 */

#include <dirent.h>
#include <getopt.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include "journal.h"

static const char *opt_unit    = NULL;
static bool        opt_follow  = false;
static int         opt_lines   = 50;
static int         opt_priority= LOG_DEBUG;  /* show all by default */
static uint64_t    opt_since   = 0;
static uint64_t    opt_until   = USEC_INFINITY;
static const char *opt_output  = "short";
static bool        opt_quiet   = false;
static bool        opt_reverse = false;
static bool        opt_no_pager= false;

/* ---- Time parsing ---- */

/* Parse a time like "2024-01-15 10:00:00", "yesterday", "today", "now", "-1h" */
static uint64_t parse_time_arg(const char *s) {
        if (!s) return 0;

        uint64_t now = now_usec();

        if (streq(s, "now"))       return now;
        if (streq(s, "today")) {
                time_t t = (time_t)(now / USEC_PER_SEC);
                struct tm tm;
                localtime_r(&t, &tm);
                tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
                return (uint64_t)mktime(&tm) * USEC_PER_SEC;
        }
        if (streq(s, "yesterday")) {
                time_t t = (time_t)(now / USEC_PER_SEC) - 86400;
                struct tm tm;
                localtime_r(&t, &tm);
                tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
                return (uint64_t)mktime(&tm) * USEC_PER_SEC;
        }

        /* Relative: -1h, -30m, -5s */
        if (*s == '-' || *s == '+') {
                char *end;
                double v = strtod(s + 1, &end);
                if (end != s + 1) {
                        uint64_t mult;
                        if (streq(end, "h") || streq(end, "hour"))    mult = 3600 * USEC_PER_SEC;
                        else if (streq(end, "m") || streq(end, "min"))mult = 60   * USEC_PER_SEC;
                        else if (streq(end, "s") || *end == '\0')     mult = USEC_PER_SEC;
                        else if (streq(end, "ms"))                     mult = USEC_PER_MSEC;
                        else                                           mult = USEC_PER_SEC;
                        uint64_t delta = (uint64_t)(v * (double)mult);
                        return *s == '-' ? (now > delta ? now - delta : 0) : now + delta;
                }
        }

        /* Absolute: "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DD" */
        struct tm tm = {0};
        if (strptime(s, "%Y-%m-%d %H:%M:%S", &tm) ||
            strptime(s, "%Y-%m-%d %H:%M", &tm) ||
            strptime(s, "%Y-%m-%d", &tm)) {
                time_t t = mktime(&tm);
                if (t != (time_t)-1)
                        return (uint64_t)t * USEC_PER_SEC;
        }

        return 0;
}

/* ---- Display ---- */

static const char *priority_color(int p) {
        if (p <= LOG_ERR)     return "\033[31m";  /* red */
        if (p <= LOG_WARNING) return "\033[33m";  /* yellow */
        if (p <= LOG_NOTICE)  return "\033[1m";   /* bold */
        return "";
}

static void print_entry_short(const JournalEntry *e) {
        char ts[32] = "";
        if (e->realtime_usec) {
                time_t t = (time_t)(e->realtime_usec / USEC_PER_SEC);
                struct tm tm;
                localtime_r(&t, &tm);
                strftime(ts, sizeof(ts), "%b %d %H:%M:%S", &tm);
        }

        char hostname[64] = "";
        gethostname(hostname, sizeof(hostname));

        const char *id  = e->identifier ? e->identifier :
                          e->unit ? e->unit : "unknown";
        const char *msg = e->message ? e->message : "";

        bool use_color = isatty(STDOUT_FILENO);

        if (use_color && e->priority <= LOG_WARNING) {
                const char *color = priority_color(e->priority);
                printf("%s %s %s", ts, hostname, id);
                if (e->pid > 0) printf("[%d]", (int)e->pid);
                printf(": %s%s\033[0m\n", color, msg);
        } else {
                printf("%s %s %s", ts, hostname, id);
                if (e->pid > 0) printf("[%d]", (int)e->pid);
                printf(": %s\n", msg);
        }
}

static void print_entry_json(const JournalEntry *e) {
        /* Minimal JSON output with escaped message */
        char msg_esc[8192] = "";
        if (e->message) {
                size_t i = 0;
                for (const char *p = e->message; *p && i + 3 < sizeof(msg_esc); p++) {
                        if (*p == '"') { msg_esc[i++] = '\\'; msg_esc[i++] = '"'; }
                        else if (*p == '\n') { msg_esc[i++] = '\\'; msg_esc[i++] = 'n'; }
                        else if (*p == '\\') { msg_esc[i++] = '\\'; msg_esc[i++] = '\\'; }
                        else msg_esc[i++] = *p;
                }
                msg_esc[i] = '\0';
        }
        printf("{\"__REALTIME_TIMESTAMP\":\"%"PRIu64"\","
               "\"PRIORITY\":\"%d\","
               "\"_SYSTEMD_UNIT\":\"%s\","
               "\"_PID\":\"%d\","
               "\"SYSLOG_IDENTIFIER\":\"%s\","
               "\"MESSAGE\":\"%s\"}\n",
               e->realtime_usec,
               e->priority,
               e->unit ? e->unit : "",
               (int)e->pid,
               e->identifier ? e->identifier : "",
               msg_esc);
}

static void print_entry_cat(const JournalEntry *e) {
        printf("%s\n", e->message ? e->message : "");
}

static void print_entry(const JournalEntry *e) {
        if (streq(opt_output, "json") || streq(opt_output, "json-pretty"))
                print_entry_json(e);
        else if (streq(opt_output, "cat"))
                print_entry_cat(e);
        else
                print_entry_short(e);
}

static bool entry_matches(const JournalEntry *e) {
        /* Priority filter */
        if (e->priority > opt_priority) return false;
        /* Unit filter */
        if (opt_unit && (!e->unit || !strstr(e->unit, opt_unit))) return false;
        /* Time range */
        if (opt_since > 0 && e->realtime_usec < opt_since) return false;
        if (opt_until != USEC_INFINITY && e->realtime_usec > opt_until) return false;
        return true;
}

/* ---- Follow mode (tail -f equivalent) ---- */

static void follow_journal(JournalReader *r, bool at_journal_begin) {
        if (at_journal_begin)
                printf("-- Journal begin --\n");
        fflush(stdout);

        for (;;) {
                JournalEntry e;
                int ret = journal_reader_next(r, &e);
                if (ret == 0) {
                        if (entry_matches(&e))
                                print_entry(&e);
                        journal_entry_clear(&e);
                        fflush(stdout);
                        continue;
                }
                /*
                 * EOF: poll() on a regular file always returns immediately
                 * (POSIX allows it; both Linux and macOS do this), so we
                 * can't use the file fd to block.  Sleep 200 ms instead,
                 * then clear the sticky feof() flag before retrying.
                 */
                poll(NULL, 0, 200);
                journal_reader_clearerr(r);
        }
}

/* ---- Main ---- */

static void print_usage(const char *argv0) {
        printf(
"Usage: %s [OPTIONS]\n"
"\n"
"Query the portable service manager journal\n"
"\n"
"Filtering Options:\n"
"  -u, --unit=UNIT        Show logs for this unit\n"
"  -p, --priority=LEVEL   Minimum priority (emerg/alert/crit/err/warning/notice/info/debug)\n"
"  --since=TIME           Show entries since TIME (e.g. '2024-01-15 10:00', 'yesterday', '-1h')\n"
"  --until=TIME           Show entries until TIME\n"
"  -n, --lines=N          Show last N lines (default 50; 'all' for unlimited)\n"
"  -f, --follow           Follow journal output\n"
"  -r, --reverse          Show newest entries first\n"
"\n"
"Output Options:\n"
"  -o, --output=FORMAT    Output format: short (default), json, cat\n"
"  --no-pager             Don't pipe output to pager\n"
"  -q, --quiet            Suppress informational output\n"
"\n"
"Miscellaneous:\n"
"  --disk-usage           Show disk usage of journal\n"
"  --version              Show version\n"
"  -h, --help             Show this help\n",
        argv0);
}

static int parse_priority(const char *s) {
        if (!s) return LOG_DEBUG;
        if (streq(s, "emerg")   || streq(s, "0")) return LOG_EMERG;
        if (streq(s, "alert")   || streq(s, "1")) return LOG_ALERT;
        if (streq(s, "crit")    || streq(s, "2")) return LOG_CRIT;
        if (streq(s, "err")     || streq(s, "3") || streq(s, "error")) return LOG_ERR;
        if (streq(s, "warning") || streq(s, "4") || streq(s, "warn"))  return LOG_WARNING;
        if (streq(s, "notice")  || streq(s, "5")) return LOG_NOTICE;
        if (streq(s, "info")    || streq(s, "6")) return LOG_INFO;
        if (streq(s, "debug")   || streq(s, "7")) return LOG_DEBUG;
        return LOG_DEBUG;
}

int main(int argc, char *argv[]) {
        const char *opt_since_str = NULL;
        const char *opt_until_str = NULL;
        bool        opt_disk_usage = false;

        static const struct option opts[] = {
                { "unit",       required_argument, NULL, 'u' },
                { "follow",     no_argument,       NULL, 'f' },
                { "lines",      required_argument, NULL, 'n' },
                { "priority",   required_argument, NULL, 'p' },
                { "since",      required_argument, NULL, 'S' },
                { "until",      required_argument, NULL, 'U' },
                { "output",     required_argument, NULL, 'o' },
                { "reverse",    no_argument,       NULL, 'r' },
                { "quiet",      no_argument,       NULL, 'q' },
                { "no-pager",   no_argument,       NULL, 'P' },
                { "disk-usage", no_argument,       NULL, 'D' },
                { "user",       no_argument,       NULL, 'Z' },
                { "system",     no_argument,       NULL, 'Y' },
                { "version",    no_argument,       NULL, 'v' },
                { "help",       no_argument,       NULL, 'h' },
                { NULL, 0, NULL, 0 }
        };

        int c;
        while ((c = getopt_long(argc, argv, "u:fn:p:o:rqh", opts, NULL)) != -1) {
                switch (c) {
                case 'u': {
                        /* Append .service if no unit type suffix, matching systemctl behaviour */
                        static const char *const sfx[] = {
                                ".service", ".socket", ".target", ".path", ".timer",
                                ".mount", ".automount", ".swap", ".slice", ".scope", ".device",
                                NULL
                        };
                        bool has_sfx = false;
                        size_t olen = strlen(optarg);
                        for (const char *const *s = sfx; *s && !has_sfx; s++) {
                                size_t slen = strlen(*s);
                                if (olen >= slen && memcmp(optarg + olen - slen, *s, slen) == 0)
                                        has_sfx = true;
                        }
                        if (has_sfx) {
                                opt_unit = optarg;
                        } else {
                                static char mangled[256];
                                snprintf(mangled, sizeof(mangled), "%s.service", optarg);
                                opt_unit = mangled;
                        }
                        break;
                }
                case 'f': opt_follow     = true;   break;
                case 'n':
                        if (streq(optarg, "all"))
                                opt_lines = INT_MAX;
                        else
                                opt_lines = atoi(optarg);
                        break;
                case 'p': opt_priority   = parse_priority(optarg); break;
                case 'S': opt_since_str  = optarg; break;
                case 'U': opt_until_str  = optarg; break;
                case 'o': opt_output     = optarg; break;
                case 'r': opt_reverse    = true;   break;
                case 'q': opt_quiet      = true;   break;
                case 'P': opt_no_pager   = true;   break;
                case 'D': opt_disk_usage = true;   break;
                case 'Z': case 'Y': break;
                case 'v':
                        printf("journalctl (portable service manager) %s\n", PSM_VERSION);
                        return 0;
                case 'h':
                        print_usage(argv[0]);
                        return 0;
                default: break;
                }
        }

        /* Parse time arguments */
        if (opt_since_str) opt_since = parse_time_arg(opt_since_str);
        if (opt_until_str) opt_until = parse_time_arg(opt_until_str);

        /* Open journal */
        _cleanup_free_ char *jdir = journal_dir();

        if (opt_disk_usage) {
                /* Show total journal size */
                struct stat st;
                long long total = 0;

                DIR *d = opendir(jdir);
                if (d) {
                        struct dirent *de;
                        while ((de = readdir(d))) {
                                char path[4096];
                                snprintf(path, sizeof(path), "%s/%s", jdir, de->d_name);
                                if (stat(path, &st) == 0)
                                        total += (long long)st.st_size;
                        }
                        closedir(d);
                }
                printf("Archived and active journals take up %lld bytes.\n", total);
                return 0;
        }

        JournalReader *r = NULL;
        int ret;

        if (opt_unit) {
                ret = journal_reader_open_unit(&r, jdir, opt_unit);
        } else {
                ret = journal_reader_open(&r, jdir);
        }

        if (ret < 0 || !r) {
                if (!opt_quiet)
                        fprintf(stderr, "No journal files found in %s\n", jdir);
                return 0;
        }

        /* Seek if requested */
        bool at_journal_begin = false;
        if (opt_since > 0)
                journal_reader_seek_realtime(r, opt_since);
        else if (opt_lines < INT_MAX && opt_lines > 0)
                at_journal_begin = journal_reader_seek_tail(r, opt_lines) == 1;

        if (opt_follow) {
                /* For follow mode, first show existing entries then tail */
                follow_journal(r, at_journal_begin);
        } else {
                /* Collect entries (respect reverse flag) */
                JournalEntry *entries = NULL;
                int n_entries = 0, cap = 0;

                JournalEntry e;
                int count = 0;

                while (journal_reader_next(r, &e) == 0) {
                        if (!entry_matches(&e)) {
                                journal_entry_clear(&e);
                                continue;
                        }
                        if (opt_lines < INT_MAX && count >= opt_lines && !opt_reverse) {
                                journal_entry_clear(&e);
                                break;
                        }
                        if (opt_reverse) {
                                /* Collect all, print in reverse later */
                                if (n_entries >= cap) {
                                        int new_cap = cap ? cap * 2 : 64;
                                        void *nv = realloc(entries, (size_t)new_cap * sizeof(JournalEntry));
                                        if (!nv) { journal_entry_clear(&e); break; }
                                        entries = nv;
                                        cap = new_cap;
                                }
                                entries[n_entries++] = e;
                        } else {
                                print_entry(&e);
                                journal_entry_clear(&e);
                                count++;
                        }
                }

                if (opt_reverse && entries) {
                        int start = MAX(0, n_entries - opt_lines);
                        for (int i = n_entries - 1; i >= start; i--) {
                                print_entry(&entries[i]);
                                journal_entry_clear(&entries[i]);
                        }
                        /* Free any uncleaned */
                        for (int i = 0; i < start; i++)
                                journal_entry_clear(&entries[i]);
                        free(entries);
                }
        }

        journal_reader_close(r);
        return 0;
}
