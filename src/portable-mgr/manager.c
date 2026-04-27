/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - service manager implementation */

#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include "manager.h"

/* Default timeouts */
#define DEFAULT_TIMEOUT_START_USEC (90  * USEC_PER_SEC)
#define DEFAULT_TIMEOUT_STOP_USEC  (90  * USEC_PER_SEC)
#define DEFAULT_RESTART_USEC       (100 * USEC_PER_MSEC)

/* ---- Forward declarations ---- */
static void unit_process_exited(EventLoop *el, pid_t pid, int status, void *userdata);
static void unit_restart_timer_fired(EventLoop *el, uint64_t id, void *userdata);
static int  unit_start_service(Unit *u);
static void unit_set_state(Unit *u, UnitActiveState state, const char *msg);

/* ---- Unit helpers ---- */

static Unit *unit_new(const char *name, Manager *m) {
        Unit *u = NEW0(Unit);
        if (!u) return NULL;
        u->name = strdup(name);
        if (!u->name) { free(u); return NULL; }
        u->type    = unit_type_from_name(name);
        u->state   = UNIT_INACTIVE;
        u->manager = m;
        return u;
}

static void unit_free(Unit *u) {
        if (!u) return;
        free(u->name);
        free(u->state_msg);
        unit_file_config_free(u->config);
        /* Dependency arrays hold borrowed pointers; don't free elements */
        free(u->deps_after);
        free(u->deps_before);
        free(u->deps_requires);
        free(u->deps_wants);
        free(u);
}

static void unit_set_state(Unit *u, UnitActiveState state, const char *msg) {
        u->state = state;
        free(u->state_msg);
        u->state_msg = msg ? strdup(msg) : NULL;

        if (state == UNIT_ACTIVE)
                u->active_enter_usec = now_usec();
        else if (state == UNIT_INACTIVE || state == UNIT_FAILED)
                u->inactive_enter_usec = now_usec();

        log_debug_unit(u->name, "state → %s%s%s",
                unit_active_state_to_string(state),
                msg ? ": " : "", msg ? msg : "");
}

/* ---- Manager lifecycle ---- */

int manager_new(Manager **ret) {
        Manager *m = NEW0(Manager);
        if (!m) return -ENOMEM;

        int r = event_loop_new(&m->event);
        if (r < 0) { free(m); return r; }

        /* Build unit search paths */
        _cleanup_free_ char *runtime = psm_runtime_dir();
        _cleanup_free_ char *config  = psm_config_dir();
        _cleanup_free_ char *data    = psm_data_dir();

        char paths[8192];
        snprintf(paths, sizeof(paths),
                "%s/systemd/user:"
                "%s/systemd/user:"
                "%s/systemd/user",
                config, data, runtime);
        m->unit_search_path = strdup(paths);

        /* Wants dir for enable/disable */
        char wdir[4096];
        snprintf(wdir, sizeof(wdir), "%s/systemd/user/default.target.wants", config);
        m->wants_dir = strdup(wdir);

        /* Open journal */
        _cleanup_free_ char *jdir = journal_dir();
        journal_open(&m->journal, jdir);

        *ret = m;
        return 0;
}

void manager_free(Manager *m) {
        if (!m) return;
        for (int i = 0; i < m->n_units; i++)
                unit_free(m->units[i]);
        free(m->units);
        event_loop_free(m->event);
        journal_close(m->journal);
        free(m->unit_search_path);
        free(m->wants_dir);
        free(m);
}

/* ---- Unit search ---- */

Unit *manager_find_unit(Manager *m, const char *name) {
        for (int i = 0; i < m->n_units; i++)
                if (streq(m->units[i]->name, name))
                        return m->units[i];
        return NULL;
}

static int manager_add_unit(Manager *m, Unit *u) {
        if (m->n_units >= m->units_cap) {
                int new_cap = m->units_cap ? m->units_cap * 2 : 16;
                Unit **nv = realloc(m->units, (size_t)new_cap * sizeof(Unit*));
                if (!nv) return -ENOMEM;
                m->units = nv;
                m->units_cap = new_cap;
        }
        m->units[m->n_units++] = u;
        return 0;
}

/* Find a unit file in the search path; fills buf with full path */
static bool find_unit_file(const char *search_path, const char *name, char *buf, size_t sz) {
        char *paths = strdup(search_path);
        if (!paths) return false;

        char *saveptr = NULL;
        char *dir = strtok_r(paths, ":", &saveptr);
        while (dir) {
                snprintf(buf, sz, "%s/%s", dir, name);
                struct stat st;
                if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)) {
                        free(paths);
                        return true;
                }
                dir = strtok_r(NULL, ":", &saveptr);
        }
        free(paths);
        return false;
}

/* ---- Unit loading ---- */

int manager_load_unit(Manager *m, const char *name, Unit **ret) {
        /* Return existing unit if already loaded */
        Unit *existing = manager_find_unit(m, name);
        if (existing) {
                if (ret) *ret = existing;
                return 0;
        }

        Unit *u = unit_new(name, m);
        if (!u) return -ENOMEM;

        /* Try to find and parse the unit file */
        char path[4096];
        if (find_unit_file(m->unit_search_path, name, path, sizeof(path))) {
                int r = parse_unit_file(path, &u->config);
                if (r < 0) {
                        log_warning("Failed to parse %s: %s", path, strerror(-r));
                        /* Continue with empty config */
                }
        }

        /* Targets don't need a file */
        if (u->type == UNIT_TARGET && !u->config) {
                /* Create a minimal synthetic config */
                u->config = NEW0(UnitFileConfig);
                if (u->config) {
                        u->config->name = strdup(name);
                        u->config->type = UNIT_TARGET;
                }
        }

        /* Check if enabled */
        u->enabled = manager_unit_is_enabled(m, name);

        int r = manager_add_unit(m, u);
        if (r < 0) { unit_free(u); return r; }

        log_debug("Loaded unit %s", name);

        if (ret) *ret = u;
        return 0;
}

int manager_load_all(Manager *m) {
        char *paths = strdup(m->unit_search_path);
        if (!paths) return -ENOMEM;

        char *saveptr = NULL;
        char *dir_path = strtok_r(paths, ":", &saveptr);
        while (dir_path) {
                DIR *d = opendir(dir_path);
                if (d) {
                        struct dirent *de;
                        while ((de = readdir(d))) {
                                if (de->d_name[0] == '.') continue;
                                UnitType t = unit_type_from_name(de->d_name);
                                if (t == _UNIT_TYPE_INVALID) continue;
                                /* Skip if already loaded */
                                if (manager_find_unit(m, de->d_name)) continue;
                                manager_load_unit(m, de->d_name, NULL);
                        }
                        closedir(d);
                }
                dir_path = strtok_r(NULL, ":", &saveptr);
        }
        free(paths);

        /* Resolve dependencies */
        for (int i = 0; i < m->n_units; i++) {
                Unit *u = m->units[i];
                if (!u->config) continue;

                UnitSection *us = &u->config->unit;

#define RESOLVE_DEPS(field, arr, narr) do { \
        if (us->field) { \
                for (char **dep = us->field; *dep; dep++) { \
                        Unit *du = manager_find_unit(m, *dep); \
                        if (!du) manager_load_unit(m, *dep, &du); \
                        if (du) { \
                                void *nv = realloc(u->arr, \
                                        (size_t)(u->narr + 2) * sizeof(Unit*)); \
                                if (nv) { u->arr = nv; u->arr[u->narr++] = du; u->arr[u->narr] = NULL; } \
                        } \
                } \
        } \
} while (0)

                RESOLVE_DEPS(after,    deps_after,    n_deps_after);
                RESOLVE_DEPS(before,   deps_before,   n_deps_before);
                RESOLVE_DEPS(requires, deps_requires, n_deps_requires);
                RESOLVE_DEPS(wants,    deps_wants,    n_deps_wants);
#undef RESOLVE_DEPS
        }

        return 0;
}

void manager_reload(Manager *m) {
        log_info("Reloading unit files");
        /* Simple approach: reload configs for all units */
        for (int i = 0; i < m->n_units; i++) {
                Unit *u = m->units[i];
                if (!u->config || !u->config->path) continue;
                UnitFileConfig *newcfg = NULL;
                if (parse_unit_file(u->config->path, &newcfg) == 0) {
                        unit_file_config_free(u->config);
                        u->config = newcfg;
                }
        }
        /* Load any new units */
        manager_load_all(m);
}

/* ---- Process execution ---- */

/* Parse an exec string into argv[]. Handles leading -, @, + modifiers.
 * Returns malloc'd argv (caller frees each element and array).
 * Sets *ignore_failure if prefixed with '-'. */
static char **parse_exec_argv(const char *exec, bool *ignore_failure) {
        if (!exec) return NULL;

        /* Skip leading whitespace and modifiers */
        while (*exec == ' ' || *exec == '\t') exec++;

        if (ignore_failure) *ignore_failure = false;

        /* Modifiers: - (ignore failure), @ (alt argv0), + (full privileges), ! (privilege escalation) */
        while (*exec == '-' || *exec == '@' || *exec == '+' || *exec == '!') {
                if (*exec == '-' && ignore_failure) *ignore_failure = true;
                exec++;
        }

        /* Allocate argv array */
        char **argv = calloc(PSM_MAX_EXEC_ARGS + 1, sizeof(char*));
        if (!argv) return NULL;

        /* Simple shell-word splitting (no full shell expansion) */
        int argc = 0;
        char *copy = strdup(exec);
        if (!copy) { free(argv); return NULL; }

        char *p = copy;
        while (*p && argc < PSM_MAX_EXEC_ARGS) {
                /* Skip whitespace */
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;

                char word[4096];
                size_t wi = 0;
                bool in_double = false, in_single = false;

                while (*p && wi + 1 < sizeof(word)) {
                        if (!in_single && *p == '"') { in_double = !in_double; p++; continue; }
                        if (!in_double && *p == '\'') { in_single = !in_single; p++; continue; }
                        if (!in_double && !in_single && (*p == ' ' || *p == '\t')) break;
                        if (!in_single && *p == '\\' && *(p+1)) { p++; }
                        word[wi++] = *p++;
                }
                word[wi] = '\0';
                if (wi > 0)
                        argv[argc++] = strdup(word);
        }
        argv[argc] = NULL;
        free(copy);
        return argv;
}

static void free_argv(char **argv) {
        if (!argv) return;
        for (char **p = argv; *p; p++) free(*p);
        free(argv);
}

/* Build the environment array for a child process */
static char **build_env(Unit *u) {
        /* Start with current environment */
        extern char **environ;
        size_t base = 0;
        while (environ[base]) base++;

        /* Count extra vars from config */
        size_t extra = 0;
        if (u->config && u->config->service.environment)
                extra = strv_length(u->config->service.environment);

        char **env = calloc(base + extra + 2, sizeof(char*));
        if (!env) return NULL;

        /* Copy base env */
        size_t i = 0;
        for (size_t j = 0; j < base; j++)
                env[i++] = strdup(environ[j]);

        /* Override/append from unit config */
        if (u->config && u->config->service.environment) {
                for (char **kv = u->config->service.environment; *kv; kv++) {
                        /* Check if this key already exists and override */
                        char *eq = strchr(*kv, '=');
                        size_t klen = eq ? (size_t)(eq - *kv) : strlen(*kv);
                        bool found = false;
                        for (size_t j = 0; j < base; j++) {
                                if (strneq(env[j], *kv, klen) && env[j][klen] == '=') {
                                        free(env[j]);
                                        env[j] = strdup(*kv);
                                        found = true;
                                        break;
                                }
                        }
                        if (!found)
                                env[i++] = strdup(*kv);
                }
        }

        /* Read environment files */
        if (u->config && u->config->service.environment_file) {
                for (char **ef = u->config->service.environment_file; *ef; ef++) {
                        const char *fpath = *ef;
                        bool required = true;
                        if (*fpath == '-') { fpath++; required = false; }

                        FILE *f = fopen(fpath, "r");
                        if (!f) {
                                if (required)
                                        log_warning_unit(u->name, "Cannot open EnvironmentFile %s: %s",
                                                fpath, strerror(errno));
                                continue;
                        }
                        char line[4096];
                        while (fgets(line, sizeof(line), f)) {
                                char *lp = strstrip(line);
                                if (!*lp || *lp == '#') continue;
                                if (i < base + extra + 1)
                                        env[i++] = strdup(lp);
                        }
                        fclose(f);
                }
        }

        env[i] = NULL;
        return env;
}

static void free_env(char **env) {
        if (!env) return;
        for (char **p = env; *p; p++) free(*p);
        free(env);
}

/* Redirect one fd based on StandardOutput/StandardError config value */
static void setup_one_fd(int target_fd, const char *cfg) {
        if (!cfg || streq(cfg, "inherit") || streq(cfg, "journal") ||
            streq(cfg, "syslog") || streq(cfg, "kmsg"))
                return; /* keep as-is */
        if (streq(cfg, "null")) {
                int fd = open("/dev/null", O_RDWR);
                if (fd >= 0) { dup2(fd, target_fd); close(fd); }
                return;
        }
        if (strprefix(cfg, "file:")) {
                const char *fpath = cfg + 5;
                int fd = open(fpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd >= 0) { dup2(fd, target_fd); close(fd); }
                return;
        }
}

/* Set up file descriptors for a child process based on StandardOutput/StandardError config */
static void setup_child_stdio(Unit *u) {
        const char *out_cfg = u->config ? u->config->service.standard_output : NULL;
        const char *err_cfg = u->config ? u->config->service.standard_error  : NULL;

        /* stdin → /dev/null by default */
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) { dup2(null, STDIN_FILENO); close(null); }

        setup_one_fd(STDOUT_FILENO, out_cfg);
        setup_one_fd(STDERR_FILENO, err_cfg);
}

/* Execute a single ExecXxx command; returns child PID or <0 on error */
static pid_t exec_command(Unit *u, const char *exec_str, bool *ignore_failure) {
        bool ig = false;
        char **argv = parse_exec_argv(exec_str, &ig);
        if (ignore_failure) *ignore_failure = ig;
        if (!argv || !argv[0]) {
                free_argv(argv);
                log_error_unit(u->name, "Empty or invalid ExecStart");
                return -EINVAL;
        }

        char **env = build_env(u);

        pid_t pid = fork();
        if (pid < 0) {
                free_argv(argv);
                free_env(env);
                return -errno;
        }

        if (pid == 0) {
                /* Child process */

                /* Become process group leader */
                setsid();

                /* Set up stdio */
                if (u->config) setup_child_stdio(u);

                /* Set working directory */
                const char *wd = u->config ? u->config->service.working_directory : NULL;
                if (wd && chdir(wd) < 0) {
                        fprintf(stderr, "chdir(%s): %s\n", wd, strerror(errno));
                        _exit(EXIT_FAILURE);
                }

                /* Set user/group if specified */
                if (u->config && u->config->service.group) {
                        struct group *gr = getgrnam(u->config->service.group);
                        if (!gr) {
                                fprintf(stderr, "Unknown group: %s\n", u->config->service.group);
                                _exit(EXIT_FAILURE);
                        }
                        if (setgid(gr->gr_gid) < 0) {
                                fprintf(stderr, "setgid: %s\n", strerror(errno));
                                _exit(EXIT_FAILURE);
                        }
                }
                if (u->config && u->config->service.user) {
                        struct passwd *pw = getpwnam(u->config->service.user);
                        if (!pw) {
                                fprintf(stderr, "Unknown user: %s\n", u->config->service.user);
                                _exit(EXIT_FAILURE);
                        }
                        if (setuid(pw->pw_uid) < 0) {
                                fprintf(stderr, "setuid: %s\n", strerror(errno));
                                _exit(EXIT_FAILURE);
                        }
                }

                /* Set resource limits */
                if (u->config && u->config->service.limit_nofile > 0) {
                        struct rlimit rl = {
                                .rlim_cur = u->config->service.limit_nofile,
                                .rlim_max = u->config->service.limit_nofile,
                        };
                        setrlimit(RLIMIT_NOFILE, &rl);
                }

                /* Set umask */
                if (u->config && u->config->service.umask != (mode_t)-1)
                        umask(u->config->service.umask);

                /* Execute */
                execve(argv[0], argv, env ? env : (char*[]){NULL});
                fprintf(stderr, "execve(%s): %s\n", argv[0], strerror(errno));
                _exit(127);
        }

        free_argv(argv);
        free_env(env);
        return pid;
}

/* ---- Unit state machine ---- */

static int unit_start_service(Unit *u) {
        Manager *m = u->manager;

        if (!u->config || !u->config->service.exec_start) {
                log_error_unit(u->name, "No ExecStart defined");
                unit_set_state(u, UNIT_FAILED, "No ExecStart");
                return -EINVAL;
        }

        unit_set_state(u, UNIT_ACTIVATING, "starting");

        journal_writef(m->journal, LOG_INFO, u->name, 0, "psm",
                "Starting %s...",
                u->config->unit.description ? u->config->unit.description : u->name);

        /* Run ExecStartPre commands synchronously */
        if (u->config->service.exec_start_pre) {
                for (char **pre = u->config->service.exec_start_pre; *pre; pre++) {
                        bool ignore = false;
                        pid_t pid = exec_command(u, *pre, &ignore);
                        if (pid < 0) {
                                if (!ignore) {
                                        unit_set_state(u, UNIT_FAILED, "ExecStartPre failed");
                                        return pid;
                                }
                                continue;
                        }
                        /* Wait for ExecStartPre to finish */
                        int status;
                        if (waitpid(pid, &status, 0) < 0) continue;
                        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                        if (exit_code != 0 && !ignore) {
                                log_error_unit(u->name, "ExecStartPre failed with exit code %d", exit_code);
                                unit_set_state(u, UNIT_FAILED, "ExecStartPre failed");
                                return -EPROTO;
                        }
                }
        }

        /* Start main process */
        pid_t pid = exec_command(u, u->config->service.exec_start, NULL);
        if (pid < 0) {
                log_error_unit(u->name, "Failed to start: %s", strerror(-pid));
                unit_set_state(u, UNIT_FAILED, strerror(-pid));
                return pid;
        }

        u->main_pid = pid;
        u->stopping = false;

        /* Watch the process */
        event_loop_watch_pid(m->event, pid, unit_process_exited, u);

        /* For oneshot, consider started immediately (wait for exit) */
        if (u->config->service.type == SERVICE_ONESHOT) {
                unit_set_state(u, UNIT_ACTIVATING, "running (oneshot)");
        } else {
                unit_set_state(u, UNIT_ACTIVE, "running");
                journal_writef(m->journal, LOG_INFO, u->name, pid, "psm",
                        "Started %s",
                        u->config->unit.description ? u->config->unit.description : u->name);
        }

        return 0;
}

/* Called when the main process exits */
static void unit_process_exited(EventLoop *el, pid_t pid, int status, void *userdata) {
        Unit *u = userdata;
        (void)el;

        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                        WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
        u->main_pid         = 0;
        u->last_exit_status = exit_code;

        /* Determine log level: INFO for clean exit or deliberate stop, WARNING otherwise */
        int log_level = (exit_code == 0 || u->stopping) ? LOG_INFO : LOG_WARNING;
        journal_writef(u->manager->journal, log_level,
                u->name, 0, "psm",
                "%s exited with status %d",
                u->config ? (u->config->unit.description ? u->config->unit.description : u->name) : u->name,
                exit_code);

        /* If we were explicitly stopping, don't restart */
        if (u->stopping) {
                unit_set_state(u, UNIT_INACTIVE, "stopped");
                return;
        }

        /* Oneshot: success = ACTIVE, failure = FAILED */
        if (u->config && u->config->service.type == SERVICE_ONESHOT) {
                if (exit_code == 0 && u->config->service.remain_after_exit)
                        unit_set_state(u, UNIT_ACTIVE, "exited");
                else if (exit_code == 0)
                        unit_set_state(u, UNIT_INACTIVE, "exited");
                else
                        unit_set_state(u, UNIT_FAILED, "exited");
                return;
        }

        /* Determine if we should restart */
        bool do_restart = false;
        if (u->config) {
                ServiceRestart r = u->config->service.restart;
                switch (r) {
                case SERVICE_RESTART_ALWAYS:
                        do_restart = true;
                        break;
                case SERVICE_RESTART_ON_FAILURE:
                        do_restart = (exit_code != 0);
                        break;
                case SERVICE_RESTART_ON_SUCCESS:
                        do_restart = (exit_code == 0);
                        break;
                case SERVICE_RESTART_ON_ABNORMAL:
                        do_restart = (exit_code != 0 && !WIFEXITED(status));
                        break;
                case SERVICE_RESTART_ON_ABORT:
                        do_restart = WIFSIGNALED(status);
                        break;
                default:
                        break;
                }
        }

        if (do_restart) {
                uint64_t delay = (u->config && u->config->service.restart_usec > 0)
                        ? u->config->service.restart_usec
                        : DEFAULT_RESTART_USEC;

                unit_set_state(u, UNIT_ACTIVATING, "auto-restart");
                u->restart_count++;
                log_info_unit(u->name, "Restarting in %"PRIu64"ms (attempt %d)",
                        delay / 1000, u->restart_count);

                event_loop_add_timer(u->manager->event, delay,
                        unit_restart_timer_fired, u,
                        &u->restart_timer_id);
        } else {
                if (exit_code == 0)
                        unit_set_state(u, UNIT_INACTIVE, "exited");
                else
                        unit_set_state(u, UNIT_FAILED, "failed");
        }
}

static void unit_restart_timer_fired(EventLoop *el, uint64_t id, void *userdata) {
        Unit *u = userdata;
        (void)el; (void)id;
        u->restart_timer_id = 0;
        log_info_unit(u->name, "Restarting service");
        unit_start_service(u);
}

/* ---- Public control interface ---- */

int manager_start_unit(Manager *m, const char *name, char *errbuf, size_t errsz) {
        Unit *u = NULL;
        int r = manager_load_unit(m, name, &u);
        if (r < 0 || !u) {
                if (errbuf) snprintf(errbuf, errsz, "Unit '%s' not found", name);
                return -ENOENT;
        }

        if (u->state == UNIT_ACTIVE || u->state == UNIT_ACTIVATING) {
                if (errbuf) snprintf(errbuf, errsz, "Unit '%s' is already active", name);
                return 0; /* Not an error; idempotent */
        }

        /* For targets, just mark active */
        if (u->type == UNIT_TARGET) {
                unit_set_state(u, UNIT_ACTIVE, "active");
                return 0;
        }

        u->pinned = true;
        r = unit_start_service(u);
        if (r < 0 && errbuf)
                snprintf(errbuf, errsz, "Failed to start '%s': %s", name, strerror(-r));
        return r;
}

int manager_stop_unit(Manager *m, const char *name, char *errbuf, size_t errsz) {
        Unit *u = manager_find_unit(m, name);
        if (!u) {
                if (errbuf) snprintf(errbuf, errsz, "Unit '%s' not found", name);
                return -ENOENT;
        }

        if (u->state == UNIT_INACTIVE || u->state == UNIT_FAILED) {
                return 0; /* Already inactive; idempotent */
        }

        u->stopping = true;
        u->pinned   = false;

        /* Cancel any pending restart */
        if (u->restart_timer_id) {
                event_loop_cancel_timer(m->event, u->restart_timer_id);
                u->restart_timer_id = 0;
        }

        unit_set_state(u, UNIT_DEACTIVATING, "stopping");

        journal_writef(m->journal, LOG_INFO, u->name, 0, "psm",
                "Stopping %s...",
                u->config && u->config->unit.description ?
                        u->config->unit.description : u->name);

        if (u->main_pid > 0) {
                int sig = (u->config && u->config->service.kill_signal > 0)
                        ? u->config->service.kill_signal : SIGTERM;
                kill(u->main_pid, sig);
                /* Process exit handled by event loop callback */
        } else if (u->config && u->config->service.exec_stop) {
                pid_t pid = exec_command(u, u->config->service.exec_stop, NULL);
                if (pid > 0) {
                        int status;
                        waitpid(pid, &status, 0);
                }
                unit_set_state(u, UNIT_INACTIVE, "stopped");
        } else {
                unit_set_state(u, UNIT_INACTIVE, "stopped");
        }

        return 0;
}

int manager_restart_unit(Manager *m, const char *name, char *errbuf, size_t errsz) {
        Unit *u = manager_find_unit(m, name);
        if (!u || u->state == UNIT_INACTIVE || u->state == UNIT_FAILED) {
                /* Not running: just start */
                return manager_start_unit(m, name, errbuf, errsz);
        }

        int r = manager_stop_unit(m, name, errbuf, errsz);
        if (r < 0) return r;

        /* Wait briefly for stop to take effect, then start */
        if (u->main_pid > 0) {
                int status;
                /* Give it up to 5 seconds */
                for (int i = 0; i < 50 && u->main_pid > 0; i++) {
                        pid_t p = waitpid(u->main_pid, &status, WNOHANG);
                        if (p > 0) { u->main_pid = 0; break; }
                        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
                        nanosleep(&ts, NULL);
                }
                if (u->main_pid > 0) {
                        kill(u->main_pid, SIGKILL);
                        waitpid(u->main_pid, &status, 0);
                        u->main_pid = 0;
                }
        }
        return manager_start_unit(m, name, errbuf, errsz);
}

int manager_reload_unit(Manager *m, const char *name, char *errbuf, size_t errsz) {
        Unit *u = manager_find_unit(m, name);
        if (!u || u->state != UNIT_ACTIVE || u->main_pid <= 0) {
                if (errbuf) snprintf(errbuf, errsz, "Unit '%s' is not running", name);
                return -EINVAL;
        }
        if (!u->config || !u->config->service.exec_reload) {
                if (errbuf) snprintf(errbuf, errsz, "Unit '%s' has no ExecReload", name);
                return -ENOTSUP;
        }
        pid_t pid = exec_command(u, u->config->service.exec_reload, NULL);
        if (pid < 0) return pid;
        int status;
        waitpid(pid, &status, 0);
        return 0;
}

/* ---- Enable / disable ---- */

bool manager_unit_is_enabled(Manager *m, const char *name) {
        (void)m;
        /* Check standard wants dirs */
        _cleanup_free_ char *config = psm_config_dir();
        char path[4096];
        snprintf(path, sizeof(path), "%s/systemd/user/default.target.wants/%s", config, name);
        return access(path, F_OK) == 0;
}

int manager_enable_unit(Manager *m, const char *name, bool *changed, char *errbuf, size_t errsz) {
        if (changed) *changed = false;

        Unit *u = NULL;
        manager_load_unit(m, name, &u);
        if (!u || !u->config) {
                if (errbuf) snprintf(errbuf, errsz, "Unit '%s' not found", name);
                return -ENOENT;
        }

        /* Find target dirs from [Install] WantedBy */
        char **wanted_by = u->config->install.wanted_by;
        if (!wanted_by || !*wanted_by) {
                /* Default to default.target.wants */
                static char *def[] = { "default.target", NULL };
                wanted_by = def;
        }

        _cleanup_free_ char *config = psm_config_dir();

        for (char **wb = wanted_by; *wb; wb++) {
                char wants_dir[4096];
                snprintf(wants_dir, sizeof(wants_dir),
                        "%s/systemd/user/%s.wants", config, *wb);

                if (psm_mkdir_p(wants_dir, 0755) < 0) {
                        if (errbuf) snprintf(errbuf, errsz, "Cannot create %s: %s",
                                wants_dir, strerror(errno));
                        return -errno;
                }

                char link_path[4096];
                snprintf(link_path, sizeof(link_path), "%s/%s", wants_dir, name);

                if (access(link_path, F_OK) == 0) continue; /* Already enabled */

                /* Find source path */
                char src[4096];
                if (!find_unit_file(m->unit_search_path, name, src, sizeof(src))) {
                        if (errbuf) snprintf(errbuf, errsz, "Cannot find unit file for '%s'", name);
                        return -ENOENT;
                }

                if (symlink(src, link_path) < 0) {
                        if (errbuf) snprintf(errbuf, errsz, "Cannot create symlink %s → %s: %s",
                                link_path, src, strerror(errno));
                        return -errno;
                }

                if (changed) *changed = true;
                log_info("Enabled %s → %s", link_path, src);
        }

        if (u) u->enabled = true;
        return 0;
}

int manager_disable_unit(Manager *m, const char *name, bool *changed, char *errbuf, size_t errsz) {
        if (changed) *changed = false;
        (void)m;

        _cleanup_free_ char *config = psm_config_dir();

        /* Search all .wants dirs */
        char base[4096];
        snprintf(base, sizeof(base), "%s/systemd/user", config);

        DIR *d = opendir(base);
        if (!d) return 0; /* Nothing to disable */

        struct dirent *de;
        while ((de = readdir(d))) {
                if (de->d_name[0] == '.') continue;

                char link[4096];
                snprintf(link, sizeof(link), "%s/%s/%s", base, de->d_name, name);
                if (unlink(link) == 0) {
                        if (changed) *changed = true;
                        log_info("Disabled %s", link);
                }
        }
        closedir(d);

        Unit *u = manager_find_unit(m, name);
        if (u) u->enabled = false;
        return 0;
}

/* ---- Status / listing ---- */

int manager_unit_status_json(Manager *m, const char *name, char *buf, size_t sz) {
        Unit *u = manager_find_unit(m, name);
        if (!u) {
                snprintf(buf, sz, "{\"ok\":false,\"error\":\"NoSuchUnit\",\"message\":\"Unit '%s' not found\"}", name);
                return -ENOENT;
        }

        const char *desc = (u->config && u->config->unit.description)
                ? u->config->unit.description : u->name;
        const char *load_state = u->config ? "loaded" : "not-found";
        const char *active_state = unit_active_state_to_string(u->state);
        const char *sub_state = u->state_msg ? u->state_msg : active_state;

        /* Escape description for JSON */
        char desc_esc[512];
        /* Simple escaping: replace " with \\" */
        size_t di = 0;
        for (const char *p = desc; *p && di + 3 < sizeof(desc_esc); p++) {
                if (*p == '"') { desc_esc[di++] = '\\'; desc_esc[di++] = '"'; }
                else desc_esc[di++] = *p;
        }
        desc_esc[di] = '\0';

        snprintf(buf, sz,
                "{\"ok\":true,"
                "\"name\":\"%s\","
                "\"description\":\"%s\","
                "\"load_state\":\"%s\","
                "\"active_state\":\"%s\","
                "\"sub_state\":\"%s\","
                "\"main_pid\":%d,"
                "\"active_since\":%"PRIu64","
                "\"restart_count\":%d,"
                "\"enabled\":%s,"
                "\"path\":\"%s\"}",
                u->name,
                desc_esc,
                load_state,
                active_state,
                sub_state,
                (int)u->main_pid,
                u->active_enter_usec,
                u->restart_count,
                u->enabled ? "true" : "false",
                (u->config && u->config->path) ? u->config->path : "");

        return 0;
}

int manager_list_units_json(Manager *m, char *buf, size_t sz) {
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, sz - pos, "{\"ok\":true,\"units\":[");

        for (int i = 0; i < m->n_units && pos + 256 < sz; i++) {
                Unit *u = m->units[i];
                const char *desc = (u->config && u->config->unit.description)
                        ? u->config->unit.description : u->name;
                const char *load = u->config ? "loaded" : "not-found";
                const char *active = unit_active_state_to_string(u->state);
                const char *sub = u->state_msg ? u->state_msg : active;

                if (i > 0) buf[pos++] = ',';
                pos += (size_t)snprintf(buf + pos, sz - pos,
                        "{\"name\":\"%s\","
                        "\"description\":\"%s\","
                        "\"load_state\":\"%s\","
                        "\"active_state\":\"%s\","
                        "\"sub_state\":\"%s\","
                        "\"main_pid\":%d}",
                        u->name, desc, load, active, sub, (int)u->main_pid);
        }

        if (pos + 3 < sz) {
                buf[pos++] = ']';
                buf[pos++] = '}';
                buf[pos]   = '\0';
        }
        return 0;
}

/* ---- Signals ---- */

static void on_sigterm(EventLoop *el, int signo, void *userdata) {
        Manager *m = userdata;
        (void)signo;
        log_info("Received SIGTERM, shutting down");

        /* Stop all running units gracefully */
        for (int i = 0; i < m->n_units; i++) {
                if (m->units[i]->main_pid > 0) {
                        m->units[i]->stopping = true;
                        kill(m->units[i]->main_pid, SIGTERM);
                }
        }

        event_loop_quit(el, 0);
}

static void on_sighup(EventLoop *el, int signo, void *userdata) {
        Manager *m = userdata;
        (void)el; (void)signo;
        log_info("Received SIGHUP, reloading");
        manager_reload(m);
}

/* ---- Main entry point ---- */

int manager_run(Manager *m) {
        /* Register signals */
        event_loop_watch_signal(m->event, SIGTERM, on_sigterm, m);
        event_loop_watch_signal(m->event, SIGINT,  on_sigterm, m);
        event_loop_watch_signal(m->event, SIGHUP,  on_sighup,  m);

        /* Load all units */
        manager_load_all(m);

        /* Start units that are enabled (wanted by default.target) */
        _cleanup_free_ char *config = psm_config_dir();
        char wants_dir[4096];
        snprintf(wants_dir, sizeof(wants_dir), "%s/systemd/user/default.target.wants", config);

        DIR *d = opendir(wants_dir);
        if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                        if (de->d_name[0] == '.') continue;
                        UnitType t = unit_type_from_name(de->d_name);
                        if (t == _UNIT_TYPE_INVALID) continue;

                        char errbuf[256] = "";
                        int r = manager_start_unit(m, de->d_name, errbuf, sizeof(errbuf));
                        if (r < 0)
                                log_warning("Failed to auto-start %s: %s", de->d_name, errbuf);
                }
                closedir(d);
        }

        log_info("Portable service manager started");
        return event_loop_run(m->event);
}
