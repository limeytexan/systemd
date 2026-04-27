/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - utility implementations */

#include <stdarg.h>
#include <sys/stat.h>
#include "psm.h"

int psm_log_level = LOG_INFO;

void psm_logv(int priority, const char *unit, const char *fmt, va_list ap) {
        if (priority > psm_log_level)
                return;

        const char *prefix;
        switch (priority) {
        case LOG_EMERG:   prefix = "EMERG";   break;
        case LOG_ALERT:   prefix = "ALERT";   break;
        case LOG_CRIT:    prefix = "CRIT";    break;
        case LOG_ERR:     prefix = "ERR";     break;
        case LOG_WARNING: prefix = "WARNING"; break;
        case LOG_NOTICE:  prefix = "NOTICE";  break;
        case LOG_INFO:    prefix = "INFO";    break;
        case LOG_DEBUG:   prefix = "DEBUG";   break;
        default:          prefix = "?";       break;
        }

        FILE *f = priority <= LOG_WARNING ? stderr : stdout;

        if (unit)
                fprintf(f, "[psm] %s %s: ", prefix, unit);
        else
                fprintf(f, "[psm] %s: ", prefix);

        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fflush(f);
}

void psm_log(int priority, const char *unit, const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        psm_logv(priority, unit, fmt, ap);
        va_end(ap);
}

char *psm_runtime_dir(void) {
        const char *e = getenv("XDG_RUNTIME_DIR");
        if (e)
                return strdup(e);

        /* Fallback for macOS and systems without XDG_RUNTIME_DIR */
        char buf[256];
        snprintf(buf, sizeof(buf), "/tmp/psm-runtime-%d", (int)getuid());
        return strdup(buf);
}

char *psm_config_dir(void) {
        const char *e = getenv("XDG_CONFIG_HOME");
        if (e)
                return strdup(e);

        const char *home = getenv("HOME");
        if (!home) home = "/tmp";

        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/.config", home);
        return strdup(buf);
}

char *psm_data_dir(void) {
        const char *e = getenv("XDG_DATA_HOME");
        if (e)
                return strdup(e);

        const char *home = getenv("HOME");
        if (!home) home = "/tmp";

        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/.local/share", home);
        return strdup(buf);
}

int psm_mkdir_p(const char *path, mode_t mode) {
        char tmp[4096];
        size_t len = strlen(path);
        if (len >= sizeof(tmp)) return -ENAMETOOLONG;

        memcpy(tmp, path, len + 1);

        for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') {
                        *p = '\0';
                        if (mkdir(tmp, mode) < 0) {
                                if (errno != EEXIST) return -errno;
                                /* EEXIST: verify the existing path is a directory */
                                struct stat st;
                                if (stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode))
                                        return -ENOTDIR;
                        }
                        *p = '/';
                }
        }
        if (mkdir(tmp, mode) < 0) {
                if (errno != EEXIST) return -errno;
                struct stat st;
                if (stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode))
                        return -ENOTDIR;
        }
        return 0;
}
