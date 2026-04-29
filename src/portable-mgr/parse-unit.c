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

/* Parse a compound time value like "5s", "1h30min", "2d"; returns usec or USEC_INFINITY.
 * Supports "infinity" and all units systemd's parse_sec() accepts.
 * Multiple terms are summed: "1h 30min" or "1h30min" both equal 90 min. */
static usec_t parse_time_usec(const char *s) {
        if (!s || !*s) return USEC_INFINITY;
        while (*s == ' ' || *s == '\t') s++;
        if (streq(s, "infinity")) return USEC_INFINITY;

        usec_t total = 0;
        bool any = false;
        const char *p = s;

        while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;

                char *end;
                double v = strtod(p, &end);
                if (end == p) return USEC_INFINITY;
                any = true;

                while (*end == ' ' || *end == '\t') end++;

                /* Match longest unit suffix first; require non-alpha after to avoid
                 * "min" matching "minutes" partially. */
#define UNIT(str, mult_val) \
        if (strprefix(end, str) && !isalpha((unsigned char)end[sizeof(str)-1])) { \
                total += (usec_t)(v * (double)(mult_val)); \
                end += sizeof(str) - 1; \
                p = end; continue; \
        }
                UNIT("microseconds", 1ULL)
                UNIT("microsecond",  1ULL)
                UNIT("milliseconds", USEC_PER_MSEC)
                UNIT("millisecond",  USEC_PER_MSEC)
                UNIT("msec",         USEC_PER_MSEC)
                UNIT("seconds",      USEC_PER_SEC)
                UNIT("second",       USEC_PER_SEC)
                UNIT("minutes",      60ULL * USEC_PER_SEC)
                UNIT("minute",       60ULL * USEC_PER_SEC)
                UNIT("months",       2629800ULL * USEC_PER_SEC)
                UNIT("month",        2629800ULL * USEC_PER_SEC)
                UNIT("hours",        3600ULL * USEC_PER_SEC)
                UNIT("hour",         3600ULL * USEC_PER_SEC)
                UNIT("days",         86400ULL * USEC_PER_SEC)
                UNIT("day",          86400ULL * USEC_PER_SEC)
                UNIT("weeks",        604800ULL * USEC_PER_SEC)
                UNIT("week",         604800ULL * USEC_PER_SEC)
                UNIT("years",        31557600ULL * USEC_PER_SEC)
                UNIT("year",         31557600ULL * USEC_PER_SEC)
                /* Short forms: must check multi-char before single-char */
                UNIT("usec", 1ULL)
                UNIT("ms",   USEC_PER_MSEC)
                UNIT("sec",  USEC_PER_SEC)
                UNIT("min",  60ULL * USEC_PER_SEC)
                UNIT("hr",   3600ULL * USEC_PER_SEC)
                UNIT("us",   1ULL)
                /* Single-character units */
                if (*end == 's' && !isalpha((unsigned char)end[1]))
                        { total += (usec_t)(v * USEC_PER_SEC); end++; p = end; continue; }
                if (*end == 'm' && !isalpha((unsigned char)end[1]))
                        { total += (usec_t)(v * 60ULL * USEC_PER_SEC); end++; p = end; continue; }
                if (*end == 'h' && !isalpha((unsigned char)end[1]))
                        { total += (usec_t)(v * 3600ULL * USEC_PER_SEC); end++; p = end; continue; }
                if (*end == 'd' && !isalpha((unsigned char)end[1]))
                        { total += (usec_t)(v * 86400ULL * USEC_PER_SEC); end++; p = end; continue; }
                if (*end == 'w' && !isalpha((unsigned char)end[1]))
                        { total += (usec_t)(v * 604800ULL * USEC_PER_SEC); end++; p = end; continue; }
                if (*end == 'y' && !isalpha((unsigned char)end[1]))
                        { total += (usec_t)(v * 31557600ULL * USEC_PER_SEC); end++; p = end; continue; }
#undef UNIT
                /* No recognisable unit: treat as seconds if at end-of-string */
                if (*end == '\0' || *end == ' ' || *end == '\t')
                        { total += (usec_t)(v * USEC_PER_SEC); p = end; continue; }
                return USEC_INFINITY; /* unrecognised suffix */
        }

        return any ? total : USEC_INFINITY;
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

static int parse_timer_key(TimerSection *t, const char *key, const char *val) {
        /* Relative-offset variants — each adds a TimerValue */
        TimerBase base = _TIMER_BASE_MAX;
        if      (streq(key, "OnActiveSec"))        base = TIMER_ACTIVE;
        else if (streq(key, "OnBootSec"))          base = TIMER_BOOT;
        else if (streq(key, "OnStartupSec"))       base = TIMER_STARTUP;
        else if (streq(key, "OnUnitActiveSec"))    base = TIMER_UNIT_ACTIVE;
        else if (streq(key, "OnUnitInactiveSec"))  base = TIMER_UNIT_INACTIVE;

        if (base != _TIMER_BASE_MAX) {
                if (t->n_values >= PSM_MAX_TIMER_VALUES) return 0;
                usec_t v = parse_time_usec(val);
                if (v == USEC_INFINITY) {
                        log_warning("Timer: invalid duration for %s=%s", key, val);
                        return 0;
                }
                TimerValue *tv = &t->values[t->n_values++];
                tv->base          = base;
                tv->value         = v;
                tv->calendar_spec = NULL;
                return 0;
        }

        if (streq(key, "OnCalendar")) {
                if (t->n_values >= PSM_MAX_TIMER_VALUES) return 0;
                CalendarSpec *spec = NULL;
                int r = calendar_spec_from_string(val, &spec);
                if (r < 0) {
                        log_warning("Timer: invalid OnCalendar= expression '%s': %s", val, strerror(-r));
                        return 0;
                }
                TimerValue *tv = &t->values[t->n_values++];
                tv->base          = TIMER_CALENDAR;
                tv->value         = 0;
                tv->calendar_spec = spec;
                return 0;
        }

        if (streq(key, "Unit"))
                return strdup_assign(&t->unit, val);
        if (streq(key, "Persistent"))
                return parse_bool(val, &t->persistent);
        if (streq(key, "RemainAfterElapse"))
                return parse_bool(val, &t->remain_after_elapse);
        if (streq(key, "AccuracySec")) {
                usec_t v = parse_time_usec(val);
                if (v != USEC_INFINITY) t->accuracy_usec = v;
                return 0;
        }
        if (streq(key, "RandomizedDelaySec")) {
                usec_t v = parse_time_usec(val);
                if (v != USEC_INFINITY) t->randomized_delay_usec = v;
                return 0;
        }
        return 0; /* silently ignore unknown [Timer] keys */
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
                else if (streq(section, "Timer"))
                        r = parse_timer_key(&c->timer, key, val);
                else if (streq(section, "Install"))
                        r = parse_install_key(&c->install, key, val);
                /* Socket and other sections silently ignored */

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

        /* [Timer] */
        for (int i = 0; i < c->timer.n_values; i++)
                calendar_spec_free(c->timer.values[i].calendar_spec);
        free(c->timer.unit);

        /* [Install] */
        strv_free(c->install.wanted_by);
        strv_free(c->install.required_by);
        strv_free(c->install.also);
        free(c->install.alias);

        free(c);
}
