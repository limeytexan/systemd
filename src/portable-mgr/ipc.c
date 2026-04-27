/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - IPC implementation */

#include <sys/socket.h>
#include <sys/un.h>
#include "ipc.h"
#include "manager.h"

/* ---- Socket path ---- */

char *ipc_socket_path(void) {
        _cleanup_free_ char *runtime = psm_runtime_dir();
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/psm/manager.sock", runtime);
        return strdup(buf);
}

/* ---- Minimal JSON parser for IPC messages ---- */

/* Parse {"key":"value",...} into an IpcMessage.
 * Modifies buf in place. All keys/vals point into buf. */
int ipc_msg_parse(IpcMessage *msg, char *buf) {
        memset(msg, 0, sizeof(*msg));
        if (!buf || *buf != '{') return -EINVAL;

        char *p = buf + 1;
        while (*p && *p != '}') {
                /* Skip whitespace and comma */
                while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n') p++;
                if (!*p || *p == '}') break;

                /* Parse key */
                if (*p != '"') return -EINVAL;
                p++; /* skip " */
                char *key = p;
                while (*p && *p != '"') p++;
                if (!*p) return -EINVAL;
                *p++ = '\0'; /* null-terminate key */

                /* Skip : */
                while (*p == ' ') p++;
                if (*p != ':') return -EINVAL;
                p++;
                while (*p == ' ') p++;

                /* Parse value */
                char *val;
                if (*p == '"') {
                        p++;
                        val = p;
                        while (*p && *p != '"') {
                                if (*p == '\\' && *(p+1)) p++; /* skip escape */
                                p++;
                        }
                        if (*p) *p++ = '\0';
                } else if (*p == 't' && strprefix(p, "true")) {
                        val = "true"; p += 4;
                } else if (*p == 'f' && strprefix(p, "false")) {
                        val = "false"; p += 5;
                } else if (*p == 'n' && strprefix(p, "null")) {
                        val = ""; p += 4;
                } else {
                        /* number */
                        val = p;
                        while (*p && *p != ',' && *p != '}' && *p != ' ') p++;
                        char *end = p;
                        /* null-terminate temporarily */
                        char saved = *end;
                        *end = '\0';
                        /* copy into static buffer for number values */
                        /* NOTE: val already points into buf so we're fine */
                        (void)saved;
                }

                if (msg->n < PSM_IPC_MAX_FIELDS) {
                        msg->keys[msg->n] = key;
                        msg->vals[msg->n] = val;
                        msg->n++;
                }
        }
        return 0;
}

const char *ipc_msg_get(const IpcMessage *msg, const char *key) {
        for (int i = 0; i < msg->n; i++)
                if (msg->keys[i] && streq(msg->keys[i], key))
                        return msg->vals[i];
        return NULL;
}

int ipc_build_ok(char *buf, size_t sz) {
        snprintf(buf, sz, "{\"ok\":true}\n");
        return 0;
}

int ipc_build_error(char *buf, size_t sz, const char *error, const char *message) {
        /* Escape message for JSON */
        char msg_esc[1024] = "";
        if (message) {
                size_t i = 0;
                for (const char *p = message; *p && i + 3 < sizeof(msg_esc); p++) {
                        if (*p == '"') { msg_esc[i++] = '\\'; msg_esc[i++] = '"'; }
                        else msg_esc[i++] = *p;
                }
                msg_esc[i] = '\0';
        }
        snprintf(buf, sz, "{\"ok\":false,\"error\":\"%s\",\"message\":\"%s\"}\n",
                error ? error : "Error", msg_esc);
        return 0;
}

/* ---- Dispatch a single IPC request → response ---- */

static void dispatch_request(Manager *mgr, const char *req_line,
                             char *resp, size_t resp_sz) {
        char req_copy[16384];
        size_t rlen = strlen(req_line);
        if (rlen >= sizeof(req_copy)) rlen = sizeof(req_copy) - 1;
        memcpy(req_copy, req_line, rlen);
        req_copy[rlen] = '\0';

        IpcMessage msg;
        if (ipc_msg_parse(&msg, req_copy) < 0) {
                ipc_build_error(resp, resp_sz, "ParseError", "Invalid JSON request");
                return;
        }

        const char *method = ipc_msg_get(&msg, "method");
        if (!method) {
                ipc_build_error(resp, resp_sz, "NoMethod", "Missing 'method' field");
                return;
        }

        char errbuf[512] = "";

        if (streq(method, "StartUnit")) {
                const char *name = ipc_msg_get(&msg, "name");
                if (!name) { ipc_build_error(resp, resp_sz, "MissingParam", "Missing 'name'"); return; }
                int r = manager_start_unit(mgr, name, errbuf, sizeof(errbuf));
                if (r < 0) ipc_build_error(resp, resp_sz, "Failed", errbuf);
                else ipc_build_ok(resp, resp_sz);

        } else if (streq(method, "StopUnit")) {
                const char *name = ipc_msg_get(&msg, "name");
                if (!name) { ipc_build_error(resp, resp_sz, "MissingParam", "Missing 'name'"); return; }
                int r = manager_stop_unit(mgr, name, errbuf, sizeof(errbuf));
                if (r < 0) ipc_build_error(resp, resp_sz, "Failed", errbuf);
                else ipc_build_ok(resp, resp_sz);

        } else if (streq(method, "RestartUnit")) {
                const char *name = ipc_msg_get(&msg, "name");
                if (!name) { ipc_build_error(resp, resp_sz, "MissingParam", "Missing 'name'"); return; }
                int r = manager_restart_unit(mgr, name, errbuf, sizeof(errbuf));
                if (r < 0) ipc_build_error(resp, resp_sz, "Failed", errbuf);
                else ipc_build_ok(resp, resp_sz);

        } else if (streq(method, "ReloadUnit")) {
                const char *name = ipc_msg_get(&msg, "name");
                if (!name) { ipc_build_error(resp, resp_sz, "MissingParam", "Missing 'name'"); return; }
                int r = manager_reload_unit(mgr, name, errbuf, sizeof(errbuf));
                if (r < 0) ipc_build_error(resp, resp_sz, "Failed", errbuf);
                else ipc_build_ok(resp, resp_sz);

        } else if (streq(method, "GetUnitStatus")) {
                const char *name = ipc_msg_get(&msg, "name");
                if (!name) { ipc_build_error(resp, resp_sz, "MissingParam", "Missing 'name'"); return; }
                manager_unit_status_json(mgr, name, resp, resp_sz);
                /* Append newline if missing */
                size_t n = strlen(resp);
                if (n + 2 < resp_sz && resp[n-1] != '\n') { resp[n] = '\n'; resp[n+1] = '\0'; }

        } else if (streq(method, "ListUnits")) {
                manager_list_units_json(mgr, resp, resp_sz);
                size_t n = strlen(resp);
                if (n + 2 < resp_sz && resp[n-1] != '\n') { resp[n] = '\n'; resp[n+1] = '\0'; }

        } else if (streq(method, "ListUnitFiles")) {
                manager_list_unit_files_json(mgr, resp, resp_sz);
                size_t n = strlen(resp);
                if (n + 2 < resp_sz && resp[n-1] != '\n') { resp[n] = '\n'; resp[n+1] = '\0'; }

        } else if (streq(method, "EnableUnit")) {
                const char *name = ipc_msg_get(&msg, "name");
                if (!name) { ipc_build_error(resp, resp_sz, "MissingParam", "Missing 'name'"); return; }
                bool changed = false;
                int r = manager_enable_unit(mgr, name, &changed, errbuf, sizeof(errbuf));
                if (r < 0) ipc_build_error(resp, resp_sz, "Failed", errbuf);
                else snprintf(resp, resp_sz, "{\"ok\":true,\"changed\":%s}\n",
                        changed ? "true" : "false");

        } else if (streq(method, "DisableUnit")) {
                const char *name = ipc_msg_get(&msg, "name");
                if (!name) { ipc_build_error(resp, resp_sz, "MissingParam", "Missing 'name'"); return; }
                bool changed = false;
                int r = manager_disable_unit(mgr, name, &changed, errbuf, sizeof(errbuf));
                if (r < 0) ipc_build_error(resp, resp_sz, "Failed", errbuf);
                else snprintf(resp, resp_sz, "{\"ok\":true,\"changed\":%s}\n",
                        changed ? "true" : "false");

        } else if (streq(method, "Reload")) {
                manager_reload(mgr);
                ipc_build_ok(resp, resp_sz);

        } else if (streq(method, "Ping")) {
                snprintf(resp, resp_sz, "{\"ok\":true,\"version\":\"%s\"}\n", PSM_VERSION);

        } else {
                ipc_build_error(resp, resp_sz, "UnknownMethod",
                        "Unknown method (try StartUnit, StopUnit, etc.)");
        }
}

/* ---- Server ---- */

struct IpcServer {
        int      fd;        /* Listen socket */
        char    *path;      /* Socket path */
        Manager *manager;
};

int ipc_server_new(IpcServer **ret, const char *sock_path, Manager *mgr) {
        IpcServer *s = NEW0(IpcServer);
        if (!s) return -ENOMEM;

        s->path = strdup(sock_path);
        if (!s->path) { free(s); return -ENOMEM; }
        s->manager = mgr;

        /* Ensure directory exists */
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s", sock_path);
        char *slash = strrchr(dir, '/');
        if (slash) {
                *slash = '\0';
                int r = psm_mkdir_p(dir, 0700);
                if (r < 0) {
                        log_error("mkdir_p(%s): %s", dir, strerror(-r));
                        ipc_server_free(s);
                        return r;
                }
        }

        /* Remove stale socket */
        unlink(sock_path);

        s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s->fd < 0) {
                log_error("socket: %s", strerror(errno));
                ipc_server_free(s);
                return -errno;
        }

        set_cloexec(s->fd);
        set_nonblock(s->fd);

#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(s->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

        if (bind(s->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                log_error("bind(%s): %s", sock_path, strerror(errno));
                ipc_server_free(s);
                return -errno;
        }

        if (listen(s->fd, 16) < 0) {
                log_error("listen: %s", strerror(errno));
                ipc_server_free(s);
                return -errno;
        }

        log_info("IPC socket: %s", sock_path);
        *ret = s;
        return 0;
}

void ipc_server_free(IpcServer *s) {
        if (!s) return;
        if (s->fd >= 0) close(s->fd);
        if (s->path) unlink(s->path);
        free(s->path);
        free(s);
}

int ipc_server_fd(const IpcServer *s) {
        return s->fd;
}

/* Accept and handle one client connection */
void ipc_server_accept(IpcServer *s) {
        int client = accept(s->fd, NULL, NULL);
        if (client < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                        log_warning("accept: %s", strerror(errno));
                return;
        }

        /* Set a read timeout */
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* Read request line (up to 16K) */
        char req[16384];
        ssize_t n = recv(client, req, sizeof(req) - 1, 0);
        if (n <= 0) {
                close(client);
                return;
        }
        req[n] = '\0';
        /* Strip trailing newline */
        while (n > 0 && (req[n-1] == '\n' || req[n-1] == '\r')) req[--n] = '\0';

        /* Dispatch */
        char resp[32768];
        dispatch_request(s->manager, req, resp, sizeof(resp));

        /* Send response */
        size_t resp_len = strlen(resp);
        /* Ensure response ends with newline */
        if (resp_len > 0 && resp[resp_len-1] != '\n') {
                resp[resp_len++] = '\n';
                resp[resp_len]   = '\0';
        }
        send(client, resp, resp_len, MSG_NOSIGNAL);
        close(client);
}

/* ---- Client ---- */

int ipc_client_call(const char *sock_path, const char *request,
                    char *response, size_t resp_sz) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -errno;

#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        /* Set connect timeout */
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                int e = errno;
                close(fd);
                return -e;
        }

        /* Send request (ensure trailing newline) */
        char req_buf[16384];
        snprintf(req_buf, sizeof(req_buf), "%s\n", request);

        if (send(fd, req_buf, strlen(req_buf), MSG_NOSIGNAL) < 0) {
                int e = errno;
                close(fd);
                return -e;
        }

        /* Read response */
        ssize_t n = recv(fd, response, resp_sz - 1, 0);
        close(fd);

        if (n < 0) return -errno;
        if (n == 0) return -ECONNRESET;

        response[n] = '\0';
        /* Strip trailing newline */
        while (n > 0 && (response[n-1] == '\n' || response[n-1] == '\r'))
                response[--n] = '\0';

        return 0;
}
