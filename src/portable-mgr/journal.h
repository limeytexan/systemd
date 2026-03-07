/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - structured journal writer/reader
 *
 * Log format: newline-delimited JSON, one entry per line.
 * Fields mirror the systemd journal field conventions.
 */
#pragma once

#include "psm.h"

/* ---- Writer ---- */

typedef struct Journal Journal;

int  journal_open(Journal **ret, const char *dir);
void journal_close(Journal *j);

/* Write a log entry; all fields are optional (pass NULL to omit) */
int journal_write(
        Journal    *j,
        int         priority,      /* LOG_INFO etc. */
        const char *unit,          /* _SYSTEMD_UNIT */
        pid_t       pid,           /* _PID; 0 = omit */
        const char *identifier,    /* SYSLOG_IDENTIFIER */
        const char *message);      /* MESSAGE */

/* Convenience wrapper that formats message like printf */
int journal_writef(
        Journal    *j,
        int         priority,
        const char *unit,
        pid_t       pid,
        const char *identifier,
        const char *fmt, ...)
        __attribute__((format(printf, 6, 7)));

/* ---- Reader ---- */

typedef struct JournalEntry {
        uint64_t realtime_usec;    /* _REALTIME_TIMESTAMP */
        uint64_t monotonic_usec;   /* _MONOTONIC_TIMESTAMP */
        char    *unit;             /* _SYSTEMD_UNIT */
        char    *message;          /* MESSAGE */
        char    *identifier;       /* SYSLOG_IDENTIFIER */
        int      priority;         /* PRIORITY */
        pid_t    pid;              /* _PID */
} JournalEntry;

typedef struct JournalReader JournalReader;

int  journal_reader_open(JournalReader **ret, const char *dir);
int  journal_reader_open_unit(JournalReader **ret, const char *dir, const char *unit);
void journal_reader_close(JournalReader *r);

/* Read next entry; returns 0 if entry read, 1 if EOF, <0 on error */
int  journal_reader_next(JournalReader *r, JournalEntry *ret);
/* Return underlying fd for polling (-1 if not available) */
int  journal_reader_fileno(JournalReader *r);
/* Seek to entries after this realtime timestamp (usec) */
int  journal_reader_seek_realtime(JournalReader *r, uint64_t usec);
/* Jump to last N entries */
int  journal_reader_seek_tail(JournalReader *r, int n);

void journal_entry_clear(JournalEntry *e);

/* Path where journal files are stored */
char *journal_dir(void);  /* caller must free */
