/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - systemd unit file parser
 *
 * Implements a subset of the systemd unit file format sufficient for
 * managing user-space services.
 */

#include <ctype.h>
#include <limits.h>
#include "parse-unit.h"

/* ---- Type tables ---- */

const char *unit_type_suffix(UnitType t) {
        switch (t) {
        case UNIT_SERVICE: return ".service";
        case UNIT_TARGET:  return ".target";
        case UNIT_SOCKET:  return ".socket";
        case UNIT_TIMER:   return ".timer";
        default:           return NULL;
        }
}

const char *unit_type_to_string(UnitType t) {
        switch (t) {
        case UNIT_SERVICE: return "service";
        case UNIT_TARGET:  return "target";
        case UNIT_SOCKET:  return "socket";
        case UNIT_TIMER:   return "timer";
        default:           return "unknown";
        }
}

UnitType unit_type_from_name(const char *name) {
        if (!name) return _UNIT_TYPE_INVALID;
        const char *dot = strrchr(name, '.');
        if (!dot) return _UNIT_TYPE_INVALID;
        if (streq(dot, ".service")) return UNIT_SERVICE;
        if (streq(dot, ".target"))  return UNIT_TARGET;
        if (streq(dot, ".socket"))  return UNIT_SOCKET;
        if (streq(dot, ".timer"))   return UNIT_TIMER;
        return _UNIT_TYPE_INVALID;
}

const char *service_type_to_string(ServiceType t) {
        switch (t) {
        case SERVICE_SIMPLE:  return "simple";
        case SERVICE_FORKING: return "forking";
        case SERVICE_ONESHOT: return "oneshot";
        case SERVICE_NOTIFY:  return "notify";
        case SERVICE_EXEC:    return "exec";
        case SERVICE_IDLE:    return "idle";
        default:              return "simple";
        }
}

ServiceType service_type_from_string(const char *s) {
        if (!s) return SERVICE_SIMPLE;
        if (streq(s, "simple"))  return SERVICE_SIMPLE;
        if (streq(s, "forking")) return SERVICE_FORKING;
        if (streq(s, "oneshot")) return SERVICE_ONESHOT;
        if (streq(s, "notify"))  return SERVICE_NOTIFY;
        if (streq(s, "exec"))    return SERVICE_EXEC;
        if (streq(s, "idle"))    return SERVICE_IDLE;
        return SERVICE_SIMPLE;
}

const char *service_restart_to_string(ServiceRestart r) {
        switch (r) {
        case SERVICE_RESTART_NO:          return "no";
        case SERVICE_RESTART_ON_SUCCESS:  return "on-success";
        case SERVICE_RESTART_ON_FAILURE:  return "on-failure";
        case SERVICE_RESTART_ON_ABNORMAL: return "on-abnormal";
        case SERVICE_RESTART_ON_ABORT:    return "on-abort";
        case SERVICE_RESTART_ALWAYS:      return "always";
        default:                          return "no";
        }
}

ServiceRestart service_restart_from_string(const char *s) {
        if (!s) return SERVICE_RESTART_NO;
        if (streq(s, "no"))           return SERVICE_RESTART_NO;
        if (streq(s, "on-success"))   return SERVICE_RESTART_ON_SUCCESS;
        if (streq(s, "on-failure"))   return SERVICE_RESTART_ON_FAILURE;
        if (streq(s, "on-abnormal"))  return SERVICE_RESTART_ON_ABNORMAL;
        if (streq(s, "on-abort"))     return SERVICE_RESTART_ON_ABORT;
        if (streq(s, "always"))       return SERVICE_RESTART_ALWAYS;
        return SERVICE_RESTART_NO;
}

const char *unit_active_state_to_string(UnitActiveState s) {
        switch (s) {
        case UNIT_INACTIVE:    return "inactive";
        case UNIT_ACTIVATING:  return "activating";
        case UNIT_ACTIVE:      return "active";
        case UNIT_DEACTIVATING:return "deactivating";
        case UNIT_FAILED:      return "failed";
        case UNIT_RELOADING:   return "reloading";
        default:               return "unknown";
        }
}

/* ---- Parsing helpers ---- */

/* Parse a time value like "5s", "500ms", "1min", "2h"; returns usec or USEC_INFINITY on error */
static uint64_t parse_time_usec(const char *s) {
        if (!s || !*s) return USEC_INFINITY;

        /* bare number is seconds */
        char *end;
        double v = strtod(s, &end);
        if (end == s) return USEC_INFINITY;

        end = (char*)strstrip(end);
        uint64_t mult;
        if (*end == '\0' || streq(end, "s") || streq(end, "sec") || streq(end, "second") || streq(end, "seconds"))
                mult = USEC_PER_SEC;
        else if (streq(end, "ms") || streq(end, "msec") || streq(end, "millisecond") || streq(end, "milliseconds"))
                mult = USEC_PER_MSEC;
        else if (streq(end, "us") || streq(end, "usec") || streq(end, "microsecond") || streq(end, "microseconds"))
                mult = 1;
        else if (streq(end, "m") || streq(end, "min") || streq(end, "minute") || streq(end, "minutes"))
                mult = 60 * USEC_PER_SEC;
        else if (streq(end, "h") || streq(end, "hr") || streq(end, "hour") || streq(end, "hours"))
                mult = 3600 * USEC_PER_SEC;
        else if (streq(end, "d") || streq(end, "day") || streq(end, "days"))
                mult = 86400 * USEC_PER_SEC;
        else
                return USEC_INFINITY;

        return (uint64_t)(v * (double)mult);
}

/* Parse boolean: "yes"/"true"/"1" → true; "no"/"false"/"0" → false */
static int parse_bool(const char *s, bool *ret) {
        if (!s) return -EINVAL;
        if (streq(s, "yes") || streq(s, "true") || streq(s, "1") || streq(s, "on")) {
                *ret = true; return 0;
        }
        if (streq(s, "no") || streq(s, "false") || streq(s, "0") || streq(s, "off")) {
                *ret = false; return 0;
        }
        return -EINVAL;
}

/* Append a value to a NULL-terminated string array pointed to by *arr */
static int strv_append_inplace(char ***arr, const char *val) {
        char **nv = strv_append(*arr, val);
        if (!nv) return -ENOMEM;
        *arr = nv;
        return 0;
}

/* Append possibly space-separated values to strv, e.g. "a.service b.service" */
static int strv_append_words(char ***arr, const char *val) {
        char *copy = strdup(val);
        if (!copy) return -ENOMEM;

        char *p = copy;
        char *tok;
        int r = 0;
        while ((tok = strtok(p, " \t")) != NULL) {
                p = NULL;
                r = strv_append_inplace(arr, tok);
                if (r < 0) break;
        }
        free(copy);
        return r;
}

/* ---- Per-section parsers ---- */

static int parse_unit_key(UnitSection *u, const char *key, const char *val) {
        if (streq(key, "Description"))
                return strdup_assign(&u->description, val);
        if (streq(key, "Documentation"))
                return strdup_assign(&u->documentation, val);
        if (streq(key, "After"))
                return strv_append_words(&u->after, val);
        if (streq(key, "Before"))
                return strv_append_words(&u->before, val);
        if (streq(key, "Requires"))
                return strv_append_words(&u->requires, val);
        if (streq(key, "Wants"))
                return strv_append_words(&u->wants, val);
        if (streq(key, "Conflicts"))
                return strv_append_words(&u->conflicts, val);
        if (streq(key, "BindsTo"))
                return strv_append_words(&u->binds_to, val);
        if (streq(key, "PartOf"))
                return strv_append_words(&u->part_of, val);
        if (streq(key, "OnFailure"))
                return strv_append_words(&u->on_failure, val);
        if (streq(key, "ConditionPathExists"))
                return strdup_assign(&u->condition_path_exists, val);
        if (streq(key, "ConditionPathExistsGlob"))
                return strdup_assign(&u->condition_path_exists_glob, val);
        if (streq(key, "DefaultDependencies"))
                return parse_bool(val, &u->default_dependencies);
        /* Silently ignore unknown [Unit] keys */
        return 0;
}

static int parse_service_key(ServiceSection *s, const char *key, const char *val) {
        if (streq(key, "Type")) {
                s->type = service_type_from_string(val);
                return 0;
        }
        if (streq(key, "ExecStart"))
                return strdup_assign(&s->exec_start, val);
        if (streq(key, "ExecStartPre"))
                return strv_append_inplace(&s->exec_start_pre, val);
        if (streq(key, "ExecStartPost"))
                return strv_append_inplace(&s->exec_start_post, val);
        if (streq(key, "ExecStop"))
                return strdup_assign(&s->exec_stop, val);
        if (streq(key, "ExecStopPost"))
                return strv_append_inplace(&s->exec_stop_post, val);
        if (streq(key, "ExecReload"))
                return strdup_assign(&s->exec_reload, val);
        if (streq(key, "ExecCondition"))
                return strdup_assign(&s->exec_condition, val);
        if (streq(key, "Restart")) {
                s->restart = service_restart_from_string(val);
                return 0;
        }
        if (streq(key, "RestartSec")) {
                uint64_t v = parse_time_usec(val);
                if (v != USEC_INFINITY) s->restart_usec = v;
                return 0;
        }
        if (streq(key, "TimeoutStartSec") || streq(key, "TimeoutSec")) {
                uint64_t v = parse_time_usec(val);
                if (v != USEC_INFINITY) s->timeout_start_usec = v;
                return 0;
        }
        if (streq(key, "TimeoutStopSec")) {
                uint64_t v = parse_time_usec(val);
                if (v != USEC_INFINITY) s->timeout_stop_usec = v;
                return 0;
        }
        if (streq(key, "User"))
                return strdup_assign(&s->user, val);
        if (streq(key, "Group"))
                return strdup_assign(&s->group, val);
        if (streq(key, "SupplementaryGroups"))
                return strv_append_words(&s->supplementary_groups, val);
        if (streq(key, "WorkingDirectory"))
                return strdup_assign(&s->working_directory, val);
        if (streq(key, "RootDirectory"))
                return strdup_assign(&s->root_directory, val);
        if (streq(key, "Environment"))
                return strv_append_inplace(&s->environment, val);
        if (streq(key, "EnvironmentFile"))
                return strv_append_inplace(&s->environment_file, val);
        if (streq(key, "StandardOutput"))
                return strdup_assign(&s->standard_output, val);
        if (streq(key, "StandardError"))
                return strdup_assign(&s->standard_error, val);
        if (streq(key, "SyslogIdentifier"))
                return strdup_assign(&s->syslog_identifier, val);
        if (streq(key, "PIDFile"))
                return strdup_assign(&s->pid_file, val);
        if (streq(key, "KillMode")) {
                if (streq(val, "control-group"))    s->kill_mode = KILL_CONTROL_GROUP;
                else if (streq(val, "process"))     s->kill_mode = KILL_PROCESS;
                else if (streq(val, "mixed"))       s->kill_mode = KILL_MIXED;
                else if (streq(val, "none"))        s->kill_mode = KILL_NONE;
                return 0;
        }
        if (streq(key, "KillSignal")) {
                char *end;
                long v = strtol(val, &end, 10);
                if (end != val && *end == '\0' && v > 0 && v < 64)
                        s->kill_signal = (int)v;
                /* Could also parse signal names like "SIGTERM" here */
                return 0;
        }
        if (streq(key, "RemainAfterExit"))
                return parse_bool(val, &s->remain_after_exit);
        if (streq(key, "GuessMainPID"))
                return parse_bool(val, &s->guess_main_pid);
        if (streq(key, "RuntimeDirectory"))
                return strdup_assign(&s->runtime_directory, val);
        if (streq(key, "LimitNOFILE")) {
                char *end;
                unsigned long v = strtoul(val, &end, 10);
                if (end != val) s->limit_nofile = v;
                return 0;
        }
        if (streq(key, "UMask")) {
                char *end;
                unsigned long v = strtoul(val, &end, 8);
                if (end != val) s->umask = (mode_t)v;
                return 0;
        }
        if (streq(key, "NotifyAccess"))
                return strdup_assign(&s->notify_access, val);
        if (streq(key, "WatchdogSec")) {
                uint64_t v = parse_time_usec(val);
                if (v != USEC_INFINITY) s->watchdog_usec = v;
                return 0;
        }
        if (streq(key, "PrivateTmp"))
                return parse_bool(val, &s->private_tmp);
        if (streq(key, "PrivateNetwork"))
                return parse_bool(val, &s->private_network);

        /* Silently ignore unknown/unsupported [Service] keys */
        return 0;
}

static int parse_install_key(InstallSection *inst, const char *key, const char *val) {
        if (streq(key, "WantedBy"))
                return strv_append_words(&inst->wanted_by, val);
        if (streq(key, "RequiredBy"))
                return strv_append_words(&inst->required_by, val);
        if (streq(key, "Also"))
                return strv_append_words(&inst->also, val);
        if (streq(key, "Alias"))
                return strdup_assign(&inst->alias, val);
        return 0;
}

/* ---- Main parser ---- */

int parse_unit_file(const char *path, UnitFileConfig **ret) {
        UnitFileConfig *c = NEW0(UnitFileConfig);
        if (!c) return -ENOMEM;

        /* Detect unit type from filename */
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        c->name = strdup(base);
        if (!c->name) goto oom;
        c->path = strdup(path);
        if (!c->path) goto oom;
        c->type = unit_type_from_name(base);

        /* Initialize defaults */
        c->unit.default_dependencies = true;
        c->service.type         = SERVICE_SIMPLE;
        c->service.restart      = SERVICE_RESTART_NO;
        c->service.restart_usec = 100 * USEC_PER_MSEC;  /* 100ms default */
        c->service.kill_signal  = SIGTERM;
        c->service.kill_mode    = KILL_CONTROL_GROUP;
        c->service.umask        = (mode_t)-1;
        c->service.syslog_facility = -1;
        c->service.guess_main_pid  = true;

        FILE *f = fopen(path, "r");
        if (!f) {
                int e = errno;
                unit_file_config_free(c);
                return -e;
        }

        char line[4096];
        char section[64] = "";
        int lineno = 0;

        while (fgets(line, sizeof(line), f)) {
                lineno++;
                char *p = strstrip(line);

                /* Skip blank lines and comments */
                if (!*p || *p == '#' || *p == ';')
                        continue;

                /* Section header */
                if (*p == '[') {
                        char *end = strchr(p, ']');
                        if (!end) {
                                log_warning("parse-unit: %s:%d: unterminated section header",
                                            path, lineno);
                                continue;
                        }
                        *end = '\0';
                        size_t slen = (size_t)(end - p - 1);
                        if (slen >= sizeof(section)) slen = sizeof(section) - 1;
                        memcpy(section, p + 1, slen);
                        section[slen] = '\0';
                        continue;
                }

                /* Key=Value */
                char *eq = strchr(p, '=');
                if (!eq) {
                        /* Could be a continuation; skip for now */
                        continue;
                }
                *eq = '\0';
                char *key = strstrip(p);
                char *val = strstrip(eq + 1);

                /* Handle line continuations (trailing \) */
                size_t vlen = strlen(val);
                char continued[4096];
                if (vlen > 0 && val[vlen - 1] == '\\') {
                        val[vlen - 1] = '\0';
                        snprintf(continued, sizeof(continued), "%s", val);
                        size_t clen = strlen(continued);
                        while (fgets(line, sizeof(line), f)) {
                                lineno++;
                                char *lp = strstrip(line);
                                size_t ll = strlen(lp);
                                bool more = ll > 0 && lp[ll-1] == '\\';
                                if (more) lp[ll-1] = '\0', ll--;
                                size_t add = MIN(ll, sizeof(continued) - clen - 2);
                                if (clen > 0) continued[clen++] = ' ';
                                memcpy(continued + clen, lp, add);
                                clen += add;
                                continued[clen] = '\0';
                                if (!more) break;
                        }
                        val = continued;
                }

                int r = 0;
                if (streq(section, "Unit"))
                        r = parse_unit_key(&c->unit, key, val);
                else if (streq(section, "Service"))
                        r = parse_service_key(&c->service, key, val);
                else if (streq(section, "Install"))
                        r = parse_install_key(&c->install, key, val);
                /* Other sections (Socket, Timer, etc.) silently ignored for now */

                if (r < 0)
                        log_warning("parse-unit: %s:%d: failed to parse %s=%s: %s",
                                    path, lineno, key, val, strerror(-r));
        }

        fclose(f);
        *ret = c;
        return 0;

oom:
        unit_file_config_free(c);
        return -ENOMEM;
}

/* ---- Cleanup ---- */

void unit_file_config_free(UnitFileConfig *c) {
        if (!c) return;
        free(c->name);
        free(c->path);

        /* [Unit] */
        free(c->unit.description);
        free(c->unit.documentation);
        strv_free(c->unit.after);
        strv_free(c->unit.before);
        strv_free(c->unit.requires);
        strv_free(c->unit.wants);
        strv_free(c->unit.conflicts);
        strv_free(c->unit.binds_to);
        strv_free(c->unit.part_of);
        strv_free(c->unit.on_failure);
        free(c->unit.condition_path_exists);
        free(c->unit.condition_path_exists_glob);

        /* [Service] */
        free(c->service.exec_start);
        strv_free(c->service.exec_start_pre);
        strv_free(c->service.exec_start_post);
        free(c->service.exec_stop);
        strv_free(c->service.exec_stop_post);
        free(c->service.exec_reload);
        free(c->service.exec_condition);
        free(c->service.user);
        free(c->service.group);
        strv_free(c->service.supplementary_groups);
        free(c->service.working_directory);
        free(c->service.root_directory);
        strv_free(c->service.environment);
        strv_free(c->service.environment_file);
        free(c->service.standard_output);
        free(c->service.standard_error);
        free(c->service.syslog_identifier);
        free(c->service.pid_file);
        free(c->service.runtime_directory);
        free(c->service.notify_access);

        /* [Install] */
        strv_free(c->install.wanted_by);
        strv_free(c->install.required_by);
        strv_free(c->install.also);
        free(c->install.alias);

        free(c);
}
