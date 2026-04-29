/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - service manager core */
#pragma once

#include "psm.h"
#include "event.h"
#include "parse-unit.h"
#include "journal.h"

/* ---- Unit (runtime state) ---- */

typedef struct Unit Unit;
typedef struct Manager Manager;

struct Unit {
        char           *name;             /* e.g. "myservice.service" */
        UnitFileConfig *config;           /* Parsed unit file; may be NULL for synthetic units */
        UnitType        type;
        UnitActiveState state;
        char           *state_msg;        /* Human-readable state description */

        /* Process state (for UNIT_SERVICE) */
        pid_t           main_pid;         /* Main process PID; 0 if not running */
        pid_t           control_pid;      /* Control process (ExecStartPre, etc.) */
        uint64_t        active_enter_usec;/* When we entered ACTIVE state */
        uint64_t        inactive_enter_usec;
        int             last_exit_status; /* Last process exit code */
        int             restart_count;    /* How many times restarted */
        uint64_t        restart_timer_id; /* Event loop timer for restart delay */

        /* Timer state (for UNIT_TIMER) */
        uint64_t        timer_event_id;   /* Event loop timer ID (0 = not armed) */
        uint64_t        last_trigger_usec;/* Last trigger realtime usec (0 = never) */
        uint64_t        activate_usec;    /* When this timer unit was activated */
        char           *timer_service;    /* Associated service name (heap-allocated) */

        /* Dependency tracking */
        Unit          **deps_after;       /* Wait for these before starting */
        int             n_deps_after;
        Unit          **deps_before;
        int             n_deps_before;
        Unit          **deps_requires;
        int             n_deps_requires;
        Unit          **deps_wants;
        int             n_deps_wants;

        /* Journal capture pipes (read ends; -1 when not active) */
        int             stdout_fd;
        int             stderr_fd;

        /* Flags */
        bool            enabled;          /* Symlink exists in .wants */
        bool            pinned;           /* Explicitly started; don't auto-stop */
        bool            stopping;         /* We initiated stop; don't restart */

        Manager        *manager;          /* Back-reference */
};

/* ---- Manager ---- */

struct Manager {
        Unit         **units;
        int            n_units;
        int            units_cap;

        EventLoop     *event;
        Journal       *journal;
        char          *unit_search_path;  /* Colon-separated list */
        char          *wants_dir;         /* Where enabled .wants symlinks live */
        bool           running;
};

/* ---- Lifecycle ---- */
int  manager_new(Manager **ret);
void manager_free(Manager *m);
int  manager_run(Manager *m);  /* Enters event loop; returns when done */

/* ---- Unit loading ---- */
int  manager_load_unit(Manager *m, const char *name, Unit **ret);
int  manager_load_all(Manager *m);
void manager_reload(Manager *m);

/* ---- Unit control ---- */
int  manager_start_unit(Manager *m, const char *name, char *errbuf, size_t errsz);
int  manager_stop_unit(Manager *m, const char *name, char *errbuf, size_t errsz);
int  manager_restart_unit(Manager *m, const char *name, char *errbuf, size_t errsz);
int  manager_reload_unit(Manager *m, const char *name, char *errbuf, size_t errsz);

/* ---- Enable/disable (symlink management) ---- */
int  manager_enable_unit(Manager *m, const char *name, bool *changed, char *errbuf, size_t errsz);
int  manager_disable_unit(Manager *m, const char *name, bool *changed, char *errbuf, size_t errsz);
bool manager_unit_is_enabled(Manager *m, const char *name);

/* ---- Status ---- */
Unit *manager_find_unit(Manager *m, const char *name);

/* Build a JSON status string for a unit */
int  manager_unit_status_json(Manager *m, const char *name, char *buf, size_t sz);
/* Build a JSON list of all units */
int  manager_list_units_json(Manager *m, char *buf, size_t sz);
/* Build a JSON list of all unit files in the search path */
int  manager_list_unit_files_json(Manager *m, char *buf, size_t sz);
