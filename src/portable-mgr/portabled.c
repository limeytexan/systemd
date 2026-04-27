/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - main daemon entry point
 *
 * Usage: systemd [--user] [--log-level=LEVEL] [--no-daemon]
 *
 * This is a minimal, cross-platform service manager compatible with systemd
 * user instances.  It reads unit files from the standard XDG directories,
 * exposes a Unix-socket IPC for systemctl, and manages service processes.
 */

#include <getopt.h>
#include <sys/stat.h>
#include "manager.h"
#include "ipc.h"

static void print_usage(const char *argv0) {
        fprintf(stdout,
                "Usage: %s [OPTIONS]\n"
                "\n"
                "Portable Service Manager (systemd-compatible user instance)\n"
                "\n"
                "Options:\n"
                "  --user              Run as user service manager (default)\n"
                "  --log-level=LEVEL   Set log level (debug/info/notice/warning/error)\n"
                "  --log-target=TARGET Log target (console/journal/auto)\n"
                "  --no-daemon         Don't daemonize (stay in foreground)\n"
                "  --version           Show version and exit\n"
                "  -h, --help          Show this help\n"
                "\n"
                "Unit files are searched in:\n"
                "  $XDG_CONFIG_HOME/systemd/user/\n"
                "  $XDG_DATA_HOME/systemd/user/\n"
                "  /etc/systemd/user/\n"
                "  /usr/local/lib/systemd/user/\n"
                "  /usr/lib/systemd/user/\n",
                argv0);
}

static int parse_log_level(const char *s) {
        if (!s) return LOG_INFO;
        if (streq(s, "debug"))   return LOG_DEBUG;
        if (streq(s, "info"))    return LOG_INFO;
        if (streq(s, "notice"))  return LOG_NOTICE;
        if (streq(s, "warning")) return LOG_WARNING;
        if (streq(s, "error"))   return LOG_ERR;
        /* Numeric */
        char *end;
        long v = strtol(s, &end, 10);
        if (end != s && *end == '\0') return (int)v;
        return LOG_INFO;
}

/* IPC accept callback registered with the event loop */
static void on_ipc_ready(EventLoop *el, int fd, void *userdata) {
        (void)el; (void)fd;
        IpcServer *s = userdata;
        /* Accept all pending connections */
        for (int i = 0; i < 16; i++)
                ipc_server_accept(s);
}

int main(int argc, char *argv[]) {
        bool daemonize = false;  /* Default: stay in foreground */
        bool user_mode = false;
        const char *log_level_str = NULL;

        static const struct option opts[] = {
                { "user",       no_argument,       NULL, 'U' },
                { "system",     no_argument,       NULL, 'S' },
                { "log-level",  required_argument, NULL, 'l' },
                { "log-target", required_argument, NULL, 't' },
                { "no-daemon",  no_argument,       NULL, 'n' },
                { "daemon",     no_argument,       NULL, 'd' },
                { "version",    no_argument,       NULL, 'v' },
                { "help",       no_argument,       NULL, 'h' },
                { NULL, 0, NULL, 0 }
        };

        int c;
        while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
                switch (c) {
                case 'U': user_mode = true;  break;
                case 'S': user_mode = false; break;
                case 'l': log_level_str = optarg; break;
                case 't': /* log target - ignored for now */ break;
                case 'n': daemonize = false; break;
                case 'd': daemonize = true;  break;
                case 'v':
                        printf("systemd (portable service manager) %s\n", PSM_VERSION);
                        printf("Built for: " PSM_BUILD_TARGET "\n");
                        return 0;
                case 'h':
                        print_usage(argv[0]);
                        return 0;
                default:
                        /* Silently ignore unknown flags for systemd compatibility */
                        break;
                }
        }
        (void)user_mode; /* Always user mode in this build */

        psm_log_level = parse_log_level(log_level_str);

        /* Check if already running */
        _cleanup_free_ char *sock_path = ipc_socket_path();
        {
                /* Try a quick ping */
                char resp[256];
                if (ipc_client_call(sock_path, "{\"method\":\"Ping\"}", resp, sizeof(resp)) == 0) {
                        fprintf(stderr, "systemd: service manager already running (socket: %s)\n",
                                sock_path);
                        return 1;
                }
        }

        if (daemonize) {
                pid_t pid = fork();
                if (pid < 0) { perror("fork"); return 1; }
                if (pid > 0) return 0;  /* Parent exits */
                setsid();
                /* Redirect stdio to /dev/null */
                int null = open("/dev/null", O_RDWR);
                if (null >= 0) {
                        dup2(null, STDIN_FILENO);
                        dup2(null, STDOUT_FILENO);
                        dup2(null, STDERR_FILENO);
                        close(null);
                }
        }

        /* Write PID file */
        _cleanup_free_ char *runtime = psm_runtime_dir();
        char pid_path[4096];
        snprintf(pid_path, sizeof(pid_path), "%s/psm/manager.pid", runtime);
        {
                char psmdir[4096];
                snprintf(psmdir, sizeof(psmdir), "%s/psm", runtime);
                psm_mkdir_p(psmdir, 0700);
        }
        {
                FILE *pf = fopen(pid_path, "w");
                if (pf) {
                        fprintf(pf, "%d\n", (int)getpid());
                        fclose(pf);
                }
        }

        log_info("Portable service manager starting (pid %d)", (int)getpid());

        /* Create manager */
        Manager *mgr = NULL;
        int r = manager_new(&mgr);
        if (r < 0) {
                log_error("Failed to create manager: %s", strerror(-r));
                return 1;
        }

        /* Create IPC server */
        IpcServer *ipc = NULL;
        r = ipc_server_new(&ipc, sock_path, mgr);
        if (r < 0) {
                log_error("Failed to create IPC server at %s: %s", sock_path, strerror(-r));
                manager_free(mgr);
                return 1;
        }

        /* Register IPC socket with event loop */
        event_loop_watch_fd(mgr->event, ipc_server_fd(ipc), false, on_ipc_ready, ipc);

        /* Run */
        r = manager_run(mgr);

        /* Cleanup */
        event_loop_unwatch_fd(mgr->event, ipc_server_fd(ipc));
        ipc_server_free(ipc);
        manager_free(mgr);
        unlink(pid_path);

        log_info("Portable service manager exited (code %d)", r);
        return r < 0 ? 1 : 0;
}
