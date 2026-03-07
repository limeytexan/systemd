/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - systemd unit file parser */
#pragma once

#include "psm.h"

/* ---- Parsed [Unit] section ---- */
typedef struct UnitSection {
        char  *description;
        char  *documentation;
        char **after;           /* NULL-terminated; start after these */
        char **before;          /* NULL-terminated; start before these */
        char **requires;        /* hard deps */
        char **wants;           /* soft deps */
        char **conflicts;
        char **binds_to;
        char **part_of;
        char **on_failure;
        char  *condition_path_exists;
        char  *condition_path_exists_glob;
        bool   default_dependencies;
} UnitSection;

/* ---- Parsed [Service] section ---- */
typedef struct ServiceSection {
        ServiceType     type;
        char           *exec_start;         /* Raw value (may start with @/-/+) */
        char          **exec_start_pre;     /* NULL-terminated */
        char          **exec_start_post;    /* NULL-terminated */
        char           *exec_stop;
        char          **exec_stop_post;     /* NULL-terminated */
        char           *exec_reload;
        char           *exec_condition;
        ServiceRestart  restart;
        uint64_t        restart_usec;       /* microseconds; default 100ms */
        uint64_t        timeout_start_usec; /* 0 = use default */
        uint64_t        timeout_stop_usec;  /* 0 = use default */
        char           *user;
        char           *group;
        char          **supplementary_groups;
        char           *working_directory;
        char           *root_directory;
        char          **environment;        /* KEY=VALUE pairs (NULL-terminated) */
        char          **environment_file;   /* File paths (NULL-terminated) */
        char           *standard_output;    /* "journal", "null", "inherit", "file:/path" */
        char           *standard_error;
        char           *syslog_identifier;
        int             syslog_facility;    /* LOG_DAEMON etc.; -1 = not set */
        char           *pid_file;           /* For Type=forking */
        KillMode        kill_mode;
        int             kill_signal;        /* signal number; default SIGTERM */
        bool            remain_after_exit;
        bool            guess_main_pid;
        char           *runtime_directory;
        unsigned long   limit_nofile;       /* 0 = not set */
        mode_t          umask;              /* (mode_t)-1 = not set */
        char           *notify_access;      /* "none", "main", "all" */
        uint64_t        watchdog_usec;      /* 0 = disabled */
        bool            private_tmp;
        bool            private_network;
} ServiceSection;

/* ---- Parsed [Install] section ---- */
typedef struct InstallSection {
        char **wanted_by;       /* NULL-terminated */
        char **required_by;     /* NULL-terminated */
        char **also;            /* NULL-terminated */
        char  *alias;
} InstallSection;

/* ---- Complete parsed unit ---- */
typedef struct UnitFileConfig {
        UnitType       type;
        char          *name;    /* e.g. "myservice.service" */
        char          *path;    /* full path to unit file */
        UnitSection    unit;
        ServiceSection service;
        InstallSection install;
} UnitFileConfig;

/* Parse a unit file at the given path */
int  parse_unit_file(const char *path, UnitFileConfig **ret);
void unit_file_config_free(UnitFileConfig *c);

/* Type helpers */
UnitType    unit_type_from_name(const char *name);    /* detect from suffix */
const char *unit_type_suffix(UnitType t);             /* ".service" etc. */
const char *unit_type_to_string(UnitType t);          /* "service" etc. */

const char   *service_type_to_string(ServiceType t);
ServiceType   service_type_from_string(const char *s);

const char    *service_restart_to_string(ServiceRestart r);
ServiceRestart service_restart_from_string(const char *s);

const char *unit_active_state_to_string(UnitActiveState s);
