/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable Service Manager - IPC protocol over Unix domain sockets
 *
 * Protocol: newline-delimited JSON over a Unix domain socket.
 * Client sends one request line; server replies with one response line.
 *
 * Request:  {"method":"StartUnit","name":"foo.service","mode":"replace"}
 * Response: {"ok":true} or {"ok":false,"error":"NoSuchUnit","message":"..."}
 */
#pragma once

#include "psm.h"

/* ---- Wire protocol helpers ---- */

/* A very minimal JSON object: up to PSM_IPC_MAX_FIELDS string fields */
#define PSM_IPC_MAX_FIELDS 32
#define PSM_IPC_MAX_VALUE  4096

typedef struct IpcMessage {
        char *keys[PSM_IPC_MAX_FIELDS];
        char *vals[PSM_IPC_MAX_FIELDS];
        int   n;
} IpcMessage;

/* Parse a JSON object from a line (modifies buf in-place) */
int  ipc_msg_parse(IpcMessage *msg, char *buf);
/* Get field value; returns NULL if not found */
const char *ipc_msg_get(const IpcMessage *msg, const char *key);
/* Build a response JSON line into buf */
int  ipc_build_ok(char *buf, size_t sz);
int  ipc_build_error(char *buf, size_t sz, const char *error, const char *message);

/* ---- Server ---- */

typedef struct IpcServer IpcServer;
typedef struct Manager Manager;  /* forward decl */

int  ipc_server_new(IpcServer **ret, const char *sock_path, Manager *mgr);
void ipc_server_free(IpcServer *s);

/* Returns the listen fd for registering with the event loop */
int  ipc_server_fd(const IpcServer *s);
/* Called when the listen fd becomes readable */
void ipc_server_accept(IpcServer *s);

/* Socket path used by both server and client */
char *ipc_socket_path(void);  /* caller must free */

/* ---- Client ---- */

/* Send a request line and read a response; both null-terminated strings.
 * Returns 0 on success (response filled), <0 on error. */
int ipc_client_call(const char *sock_path, const char *request,
                    char *response, size_t resp_sz);
