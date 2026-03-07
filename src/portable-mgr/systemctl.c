/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - systemctl client
 *
 * Provides a systemctl-compatible CLI for controlling the portable
 * service manager daemon.
 *
 * Supported commands:
 *   start <unit>...      Start unit(s)
 *   stop <unit>...       Stop unit(s)
 *   restart <unit>...    Restart unit(s)
 *   reload <unit>...     Reload unit(s) (sends ExecReload command)
 *   status [unit]...     Show unit status
 *   is-active <unit>     Check if unit is active (exit 0 if yes)
 *   is-failed <unit>     Check if unit is failed
 *   is-enabled <unit>    Check if unit is enabled
 *   enable <unit>...     Enable unit(s)
 *   disable <unit>...    Disable unit(s)
 *   list-units [pat]     List loaded units
 *   daemon-reload        Reload manager configuration
 *   show <unit>          Show unit properties (JSON)
 *   cat <unit>           Show unit file contents
 *   --version            Print version
 */

#include <getopt.h>
#include <time.h>
#include "ipc.h"
#include "journal.h"

static bool opt_quiet    = false;
static bool opt_no_pager = false;
static bool opt_json     = false;
static int  opt_lines    = 10; /* for status: number of journal lines */
static const char *opt_host = NULL;

/* ---- Formatting helpers ---- */

static void print_colored(const char *color, const char *text) {
        /* Only use colors when writing to a terminal */
        if (!opt_quiet && isatty(STDOUT_FILENO) && !opt_no_pager) {
                printf("%s%s\033[0m", color, text);
        } else {
                printf("%s", text);
        }
}

#define COLOR_GREEN   "\033[32m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"
#define DOT_ACTIVE    "●"
#define DOT_INACTIVE  "○"
#define DOT_FAILED    "×"

/* Format a microsecond timestamp as "Mon 2024-01-15 10:00:00 UTC" */
static void format_timestamp(char *buf, size_t sz, uint64_t usec) {
        if (usec == 0) { snprintf(buf, sz, "n/a"); return; }
        time_t t = (time_t)(usec / USEC_PER_SEC);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(buf, sz, "%a %Y-%m-%d %H:%M:%S UTC", &tm);
}

/* Format duration */
static void format_duration(char *buf, size_t sz, uint64_t start_usec) {
        if (start_usec == 0) { buf[0] = '\0'; return; }
        uint64_t now  = now_usec();
        uint64_t diff = now > start_usec ? now - start_usec : 0;
        uint64_t secs = diff / USEC_PER_SEC;

        if (secs < 60)       snprintf(buf, sz, "%"PRIu64"s ago", secs);
        else if (secs < 3600) snprintf(buf, sz, "%"PRIu64"min %"PRIu64"s ago", secs/60, secs%60);
        else if (secs < 86400)snprintf(buf, sz, "%"PRIu64"h %"PRIu64"min ago", secs/3600, (secs%3600)/60);
        else                  snprintf(buf, sz, "%"PRIu64" days ago", secs/86400);
}

/* ---- JSON field extraction (minimal; for response parsing) ---- */

static char *json_get_str(const char *json, const char *key) {
        char pat[128];
        snprintf(pat, sizeof(pat), "\"%s\":", key);
        const char *p = strstr(json, pat);
        if (!p) return NULL;
        p += strlen(pat);
        while (*p == ' ') p++;
        if (*p != '"') return NULL;
        p++;
        char buf[4096];
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < sizeof(buf)) {
                if (*p == '\\' && *(p+1)) {
                        p++;
                        switch (*p) {
                        case '"':  buf[i++] = '"';  break;
                        case '\\': buf[i++] = '\\'; break;
                        case 'n':  buf[i++] = '\n'; break;
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

static long long json_get_int(const char *json, const char *key) {
        char pat[128];
        snprintf(pat, sizeof(pat), "\"%s\":", key);
        const char *p = strstr(json, pat);
        if (!p) return -1;
        p += strlen(pat);
        while (*p == ' ') p++;
        /* May be quoted or unquoted */
        if (*p == '"') p++;
        char *end;
        long long v = strtoll(p, &end, 10);
        return end != p ? v : -1;
}

static bool json_get_bool(const char *json, const char *key) {
        char pat[128];
        snprintf(pat, sizeof(pat), "\"%s\":", key);
        const char *p = strstr(json, pat);
        if (!p) return false;
        p += strlen(pat);
        while (*p == ' ') p++;
        return strprefix(p, "true");
}

/* ---- IPC helpers ---- */

static int call_daemon(const char *request, char *response, size_t resp_sz) {
        _cleanup_free_ char *sock = ipc_socket_path();
        int r = ipc_client_call(sock, request, response, resp_sz);
        if (r < 0) {
                if (r == -ENOENT || r == -ECONNREFUSED) {
                        fprintf(stderr, "Failed to connect to systemd manager: %s\n"
                                "Is the service manager running? Start with: systemd --user\n",
                                strerror(-r));
                } else {
                        fprintf(stderr, "Communication error: %s\n", strerror(-r));
                }
        }
        return r;
}

/* Check if response is ok and print error if not */
static bool check_response(const char *resp, const char *unit) {
        IpcMessage msg;
        char copy[32768];
        strncpy(copy, resp, sizeof(copy) - 1);
        copy[sizeof(copy)-1] = '\0';

        if (ipc_msg_parse(&msg, copy) < 0) {
                fprintf(stderr, "Malformed response from service manager\n");
                return false;
        }
        const char *ok = ipc_msg_get(&msg, "ok");
        if (!ok || !streq(ok, "true")) {
                const char *message = ipc_msg_get(&msg, "message");
                fprintf(stderr, "Failed%s%s: %s\n",
                        unit ? " for " : "", unit ? unit : "",
                        message ? message : "unknown error");
                return false;
        }
        return true;
}

/* ---- Command implementations ---- */

static int cmd_start(int argc, char *argv[]) {
        int ret = 0;
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"StartUnit\",\"name\":\"%s\",\"mode\":\"replace\"}",
                        argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;
                if (!check_response(resp, argv[i])) ret = 1;
                else if (!opt_quiet)
                        printf("Started %s\n", argv[i]);
        }
        return ret;
}

static int cmd_stop(int argc, char *argv[]) {
        int ret = 0;
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"StopUnit\",\"name\":\"%s\",\"mode\":\"replace\"}",
                        argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;
                if (!check_response(resp, argv[i])) ret = 1;
                else if (!opt_quiet)
                        printf("Stopped %s\n", argv[i]);
        }
        return ret;
}

static int cmd_restart(int argc, char *argv[]) {
        int ret = 0;
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"RestartUnit\",\"name\":\"%s\",\"mode\":\"replace\"}",
                        argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;
                if (!check_response(resp, argv[i])) ret = 1;
                else if (!opt_quiet)
                        printf("Restarted %s\n", argv[i]);
        }
        return ret;
}

static int cmd_reload(int argc, char *argv[]) {
        int ret = 0;
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"ReloadUnit\",\"name\":\"%s\"}",
                        argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;
                if (!check_response(resp, argv[i])) ret = 1;
        }
        return ret;
}

static int cmd_status(int argc, char *argv[]) {
        int ret = 0;

        if (argc == 0) {
                /* Show all units */
                char resp[65536];
                if (call_daemon("{\"method\":\"ListUnits\"}", resp, sizeof(resp)) < 0) return 1;
                if (opt_json) { printf("%s\n", resp); return 0; }
                /* Parse and display */
                /* Quick scan: find "units":[...] and print each */
                printf("%-40s %-10s %-10s %s\n", "UNIT", "LOAD", "ACTIVE", "DESCRIPTION");
                const char *p = strstr(resp, "\"units\":[");
                if (p) {
                        p += strlen("\"units\":[");
                        while (*p && *p != ']') {
                                if (*p == '{') {
                                        /* Find matching } */
                                        int depth = 0;
                                        const char *start = p;
                                        while (*p) {
                                                if (*p == '{') depth++;
                                                else if (*p == '}') { depth--; if (!depth) { p++; break; } }
                                                p++;
                                        }
                                        /* Extract into buffer */
                                        char obj[4096];
                                        size_t olen = (size_t)(p - start);
                                        if (olen >= sizeof(obj)) olen = sizeof(obj) - 1;
                                        memcpy(obj, start, olen);
                                        obj[olen] = '\0';

                                        _cleanup_free_ char *name   = json_get_str(obj, "name");
                                        _cleanup_free_ char *desc   = json_get_str(obj, "description");
                                        _cleanup_free_ char *load   = json_get_str(obj, "load_state");
                                        _cleanup_free_ char *active = json_get_str(obj, "active_state");
                                        long long mpid = json_get_int(obj, "main_pid");

                                        if (name)
                                                printf("%-40s %-10s %-10s %s%s%s\n",
                                                        name ? name : "?",
                                                        load ? load : "?",
                                                        active ? active : "?",
                                                        desc ? desc : "",
                                                        mpid > 0 ? " (pid " : "",
                                                        mpid > 0 ? "" : "");
                                } else {
                                        p++;
                                }
                        }
                }
                return 0;
        }

        for (int i = 0; i < argc; i++) {
                char req[512], resp[16384];
                snprintf(req, sizeof(req), "{\"method\":\"GetUnitStatus\",\"name\":\"%s\"}", argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;

                if (opt_json) { printf("%s\n", resp); continue; }

                /* Check for error */
                if (strstr(resp, "\"ok\":false")) {
                        _cleanup_free_ char *msg = json_get_str(resp, "message");
                        fprintf(stderr, "%s\n", msg ? msg : "Unit not found");
                        ret = 4;
                        continue;
                }

                _cleanup_free_ char *name   = json_get_str(resp, "name");
                _cleanup_free_ char *desc   = json_get_str(resp, "description");
                _cleanup_free_ char *load   = json_get_str(resp, "load_state");
                _cleanup_free_ char *active = json_get_str(resp, "active_state");
                _cleanup_free_ char *sub    = json_get_str(resp, "sub_state");
                _cleanup_free_ char *path   = json_get_str(resp, "path");
                _cleanup_free_ char *enabled_str = json_get_str(resp, "enabled");
                long long pid       = json_get_int(resp, "main_pid");
                long long since_raw = json_get_int(resp, "active_since");
                long long restarts  = json_get_int(resp, "restart_count");

                const char *active_s = active ? active : "unknown";
                bool is_active  = active && streq(active, "active");
                bool is_failed  = active && streq(active, "failed");
                bool is_enabled = enabled_str && streq(enabled_str, "true");

                /* Print dot indicator */
                if (is_active)
                        print_colored(COLOR_GREEN, DOT_ACTIVE " ");
                else if (is_failed)
                        print_colored(COLOR_RED, DOT_FAILED " ");
                else
                        print_colored(COLOR_DIM, DOT_INACTIVE " ");

                printf("%s", name ? name : argv[i]);
                if (desc && !streq(desc, name ? name : argv[i]))
                        printf(" - %s", desc);
                printf("\n");

                printf("     Loaded: %s (%s; %s)\n",
                        load ? load : "?",
                        path ? path : "n/a",
                        is_enabled ? "enabled" : "disabled");

                char ts_buf[64] = "", dur_buf[64] = "";
                if (since_raw > 0) {
                        format_timestamp(ts_buf, sizeof(ts_buf), (uint64_t)since_raw);
                        format_duration(dur_buf, sizeof(dur_buf), (uint64_t)since_raw);
                }

                printf("     Active: ");
                if (is_active)
                        print_colored(COLOR_GREEN, "active");
                else if (is_failed)
                        print_colored(COLOR_RED, "failed");
                else
                        printf("%s", active_s);

                printf(" (%s)", sub ? sub : active_s);
                if (ts_buf[0])
                        printf(" since %s; %s", ts_buf, dur_buf);
                printf("\n");

                if (pid > 0)
                        printf("   Main PID: %lld\n", pid);
                if (restarts > 0)
                        printf("   Restarts: %lld\n", restarts);

                /* Show last journal lines */
                if (!opt_quiet && opt_lines > 0) {
                        _cleanup_free_ char *jdir = journal_dir();
                        JournalReader *jr = NULL;
                        if (journal_reader_open_unit(&jr, jdir, argv[i]) == 0 && jr) {
                                journal_reader_seek_tail(jr, opt_lines);
                                printf("\n");
                                JournalEntry entry;
                                int line_count = 0;
                                while (journal_reader_next(jr, &entry) == 0 && line_count < opt_lines) {
                                        char ts[32];
                                        time_t t = (time_t)(entry.realtime_usec / USEC_PER_SEC);
                                        struct tm tm;
                                        gmtime_r(&t, &tm);
                                        strftime(ts, sizeof(ts), "%b %d %H:%M:%S", &tm);
                                        printf("%s %s[%d]: %s\n",
                                                ts,
                                                entry.identifier ? entry.identifier : (name ? name : argv[i]),
                                                (int)entry.pid,
                                                entry.message ? entry.message : "");
                                        journal_entry_clear(&entry);
                                        line_count++;
                                }
                                journal_reader_close(jr);
                        }
                }

                if (i + 1 < argc) printf("\n");
        }
        return ret;
}

static int cmd_enable(int argc, char *argv[]) {
        int ret = 0;
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"EnableUnit\",\"name\":\"%s\"}", argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;
                if (!check_response(resp, argv[i])) { ret = 1; continue; }
                if (!opt_quiet) {
                        bool changed = json_get_bool(resp, "changed");
                        if (changed)
                                printf("Created symlink for %s\n", argv[i]);
                        else
                                printf("%s is already enabled\n", argv[i]);
                }
        }
        return ret;
}

static int cmd_disable(int argc, char *argv[]) {
        int ret = 0;
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"DisableUnit\",\"name\":\"%s\"}", argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;
                if (!check_response(resp, argv[i])) { ret = 1; continue; }
                if (!opt_quiet) {
                        bool changed = json_get_bool(resp, "changed");
                        if (changed)
                                printf("Removed symlink for %s\n", argv[i]);
                        else
                                printf("%s was not enabled\n", argv[i]);
                }
        }
        return ret;
}

static int cmd_is_active(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"GetUnitStatus\",\"name\":\"%s\"}", argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 3;
                _cleanup_free_ char *state = json_get_str(resp, "active_state");
                bool active = state && streq(state, "active");
                if (!opt_quiet) printf("%s\n", active ? "active" : (state ? state : "unknown"));
                if (!active) return 3;
        }
        return 0;
}

static int cmd_is_failed(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"GetUnitStatus\",\"name\":\"%s\"}", argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 3;
                _cleanup_free_ char *state = json_get_str(resp, "active_state");
                bool failed = state && streq(state, "failed");
                if (!opt_quiet) printf("%s\n", failed ? "failed" : (state ? state : "unknown"));
                if (!failed) return 1;
        }
        return 0;
}

static int cmd_is_enabled(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"GetUnitStatus\",\"name\":\"%s\"}", argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) {
                        /* Daemon may not be running; check filesystem directly */
                        _cleanup_free_ char *config = psm_config_dir();
                        char link[4096];
                        snprintf(link, sizeof(link), "%s/systemd/user/default.target.wants/%s",
                                config, argv[i]);
                        bool en = access(link, F_OK) == 0;
                        if (!opt_quiet) printf("%s\n", en ? "enabled" : "disabled");
                        if (!en) return 1;
                        continue;
                }
                bool en = json_get_bool(resp, "enabled");
                if (!opt_quiet) printf("%s\n", en ? "enabled" : "disabled");
                if (!en) return 1;
        }
        return 0;
}

static int cmd_list_units(int argc, char *argv[]) {
        (void)argv;
        char resp[65536];
        if (call_daemon("{\"method\":\"ListUnits\"}", resp, sizeof(resp)) < 0) return 1;

        if (opt_json) { printf("%s\n", resp); return 0; }

        /* Print table header */
        printf("  %-38s %-10s %-10s %-10s %s\n",
                "UNIT", "LOAD", "ACTIVE", "SUB", "DESCRIPTION");

        int count = 0;
        const char *p = strstr(resp, "\"units\":[");
        if (p) {
                p += strlen("\"units\":[");
                while (*p && *p != ']') {
                        if (*p != '{') { p++; continue; }

                        int depth = 0;
                        const char *start = p;
                        while (*p) {
                                if (*p == '{') depth++;
                                else if (*p == '}') { depth--; if (!depth) { p++; break; } }
                                p++;
                        }
                        char obj[4096];
                        size_t olen = (size_t)(p - start);
                        if (olen >= sizeof(obj)) olen = sizeof(obj) - 1;
                        memcpy(obj, start, olen);
                        obj[olen] = '\0';

                        _cleanup_free_ char *name   = json_get_str(obj, "name");
                        _cleanup_free_ char *desc   = json_get_str(obj, "description");
                        _cleanup_free_ char *load   = json_get_str(obj, "load_state");
                        _cleanup_free_ char *active = json_get_str(obj, "active_state");
                        _cleanup_free_ char *sub    = json_get_str(obj, "sub_state");

                        /* Filter by pattern if provided */
                        if (argc > 0 && name && !strstr(name, argv[0])) continue;

                        const char *dot;
                        const char *color;
                        if (active && streq(active, "active"))   { dot = DOT_ACTIVE; color = COLOR_GREEN; }
                        else if (active && streq(active, "failed")){ dot = DOT_FAILED; color = COLOR_RED; }
                        else                                      { dot = DOT_INACTIVE; color = COLOR_DIM; }

                        if (isatty(STDOUT_FILENO)) printf("%s%s\033[0m ", color, dot);
                        else printf("  ");

                        printf("%-38s %-10s %-10s %-10s %s\n",
                                name ? name : "?",
                                load ? load : "?",
                                active ? active : "?",
                                sub ? sub : "?",
                                desc ? desc : "");
                        count++;
                }
        }

        printf("\n%d loaded units listed.\n", count);
        return 0;
}

static int cmd_daemon_reload(void) {
        char resp[512];
        if (call_daemon("{\"method\":\"Reload\"}", resp, sizeof(resp)) < 0) return 1;
        if (!check_response(resp, NULL)) return 1;
        if (!opt_quiet) printf("Reloaded service manager configuration\n");
        return 0;
}

static int cmd_show(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
                char req[512], resp[16384];
                snprintf(req, sizeof(req), "{\"method\":\"GetUnitStatus\",\"name\":\"%s\"}", argv[i]);
                if (call_daemon(req, resp, sizeof(resp)) < 0) return 1;
                printf("%s\n", resp);
        }
        return 0;
}

static int cmd_cat(int argc, char *argv[]) {
        for (int i = 0; i < argc; i++) {
                /* Ask daemon for path */
                char req[512], resp[4096];
                snprintf(req, sizeof(req), "{\"method\":\"GetUnitStatus\",\"name\":\"%s\"}", argv[i]);
                call_daemon(req, resp, sizeof(resp));

                _cleanup_free_ char *path = json_get_str(resp, "path");
                if (!path || !path[0]) {
                        fprintf(stderr, "Cannot find unit file for %s\n", argv[i]);
                        continue;
                }
                printf("# %s\n", path);
                FILE *f = fopen(path, "r");
                if (!f) { fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno)); continue; }
                char line[4096];
                while (fgets(line, sizeof(line), f)) fputs(line, stdout);
                fclose(f);
        }
        return 0;
}

static void print_usage(const char *argv0) {
        printf(
"Usage: %s [OPTIONS] COMMAND [UNIT...]\n"
"\n"
"Query or control the portable service manager (systemd-compatible)\n"
"\n"
"Unit Commands:\n"
"  start UNIT...          Start unit(s)\n"
"  stop UNIT...           Stop unit(s)\n"
"  restart UNIT...        Restart unit(s)\n"
"  reload UNIT...         Reload unit(s) configuration\n"
"  status [UNIT...]       Show unit status\n"
"  show UNIT...           Show unit properties (JSON)\n"
"  cat UNIT...            Show unit file(s)\n"
"  is-active UNIT         Check if unit is active (0=yes)\n"
"  is-failed UNIT         Check if unit has failed (0=yes)\n"
"  is-enabled UNIT        Check if unit is enabled (0=yes)\n"
"  enable UNIT...         Enable unit(s)\n"
"  disable UNIT...        Disable unit(s)\n"
"  list-units [PATTERN]   List loaded units\n"
"  daemon-reload          Reload service manager configuration\n"
"\n"
"Options:\n"
"  -q, --quiet            Suppress informational output\n"
"  --no-pager             Don't pipe output to pager\n"
"  --output=json          Output in JSON format\n"
"  --lines=N, -n N        Number of journal lines to show in status\n"
"  --version              Show version\n"
"  -h, --help             Show this help\n",
        argv0);
}

int main(int argc, char *argv[]) {
        static const struct option opts[] = {
                { "quiet",     no_argument,       NULL, 'q' },
                { "no-pager",  no_argument,       NULL, 'P' },
                { "output",    required_argument, NULL, 'o' },
                { "lines",     required_argument, NULL, 'n' },
                { "user",      no_argument,       NULL, 'u' },
                { "system",    no_argument,       NULL, 's' },
                { "host",      required_argument, NULL, 'H' },
                { "version",   no_argument,       NULL, 'V' },
                { "help",      no_argument,       NULL, 'h' },
                { NULL, 0, NULL, 0 }
        };

        int c;
        while ((c = getopt_long(argc, argv, "qn:hH:us", opts, NULL)) != -1) {
                switch (c) {
                case 'q': opt_quiet    = true; break;
                case 'P': opt_no_pager = true; break;
                case 'o':
                        if (streq(optarg, "json")) opt_json = true;
                        break;
                case 'n':
                        opt_lines = atoi(optarg);
                        break;
                case 'H': opt_host = optarg; break;
                case 'u': break; /* --user: already default */
                case 's': break; /* --system: not supported */
                case 'V':
                        printf("systemctl (portable service manager) %s\n", PSM_VERSION);
                        return 0;
                case 'h':
                        print_usage(argv[0]);
                        return 0;
                default:
                        break;
                }
        }

        if (opt_host) {
                fprintf(stderr, "Remote host not supported in portable mode\n");
                return 1;
        }

        /* Remaining args after options */
        int rem_argc = argc - optind;
        char **rem_argv = argv + optind;

        if (rem_argc == 0) {
                /* Default: list units */
                return cmd_list_units(0, NULL);
        }

        const char *cmd = rem_argv[0];
        int cmd_argc    = rem_argc - 1;
        char **cmd_argv = rem_argv + 1;

        if (streq(cmd, "start"))          return cmd_start(cmd_argc, cmd_argv);
        if (streq(cmd, "stop"))           return cmd_stop(cmd_argc, cmd_argv);
        if (streq(cmd, "restart"))        return cmd_restart(cmd_argc, cmd_argv);
        if (streq(cmd, "reload"))         return cmd_reload(cmd_argc, cmd_argv);
        if (streq(cmd, "status"))         return cmd_status(cmd_argc, cmd_argv);
        if (streq(cmd, "show"))           return cmd_show(cmd_argc, cmd_argv);
        if (streq(cmd, "cat"))            return cmd_cat(cmd_argc, cmd_argv);
        if (streq(cmd, "enable"))         return cmd_enable(cmd_argc, cmd_argv);
        if (streq(cmd, "disable"))        return cmd_disable(cmd_argc, cmd_argv);
        if (streq(cmd, "is-active"))      return cmd_is_active(cmd_argc, cmd_argv);
        if (streq(cmd, "is-failed"))      return cmd_is_failed(cmd_argc, cmd_argv);
        if (streq(cmd, "is-enabled"))     return cmd_is_enabled(cmd_argc, cmd_argv);
        if (streq(cmd, "list-units"))     return cmd_list_units(cmd_argc, cmd_argv);
        if (streq(cmd, "daemon-reload"))  return cmd_daemon_reload();

        /* Aliases for compatibility */
        if (streq(cmd, "try-restart"))    return cmd_restart(cmd_argc, cmd_argv);
        if (streq(cmd, "reload-or-restart")) return cmd_restart(cmd_argc, cmd_argv);
        if (streq(cmd, "force-reload"))   return cmd_reload(cmd_argc, cmd_argv);
        if (streq(cmd, "kill")) {
                fprintf(stderr, "systemctl kill: not supported; use systemctl stop\n");
                return 1;
        }

        fprintf(stderr, "Unknown command: %s\nTry '%s --help' for usage.\n", cmd, argv[0]);
        return 1;
}
