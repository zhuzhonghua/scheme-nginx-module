/*
 * ngx_scheme_runtime.h -- C bridge over the dummyscheme VM.
 *
 * The scheme interpreter itself is C++ (namespace Scheme).  This bridge is
 * the only C++ in the module: it talks to the interpreter through the public
 * API only (vm.h, which includes scmapi.h -- never the VM internals), and
 * exposes a tiny C API that the pure-C nginx module
 * (ngx_http_scheme_module.c) uses, so the nginx-facing code never has to
 * touch C++.
 *
 * Output model: the scheme code sends the response body by calling the host
 * function `ngx-say` (registered into the VM at creation), which converts
 * each argument to a string and appends it to an output buffer owned by this
 * handle.  The VM's own IO streams (display/write -> stderr) are never
 * touched.  The handle is stored in the VM's registry as a lightuserdata,
 * and the response buffer is a plain growing buffer owned by this handle.
 *
 * Lifetime model (MVP): one VM is created per request and destroyed when
 * the request finishes.  A worker may run many requests concurrently and
 * the VM is not thread-safe, hence the per-request isolation.  (Future:
 * a per-worker VM with coroutine multiplexing.)
 */
#ifndef NGX_SCHEME_RUNTIME_H
#define NGX_SCHEME_RUNTIME_H

/* nginx types/macros (ngx_log_t, NGX_LOG_*); the extern "C" wrapper only
 * applies in C++ so the declarations get C linkage there (plain C needs no
 * wrapper) */
#ifdef __cplusplus
extern "C" {
#endif
#include <ngx_config.h>
#include <ngx_core.h>
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* opaque per-request scheme VM handle */
typedef struct ngx_scheme_vm_s ngx_scheme_vm_t;

/*
 * Create a fresh scheme VM.  `log` is the nginx error log that the ngx-log
 * host function writes to; returns NULL on failure.
 */
ngx_scheme_vm_t *ngx_scheme_create(ngx_log_t *log);

/*
 * Destroy the VM and release every resource it owns.
 *
 * Returns 0 on success.  On a teardown failure returns -1 and writes a
 * NUL-terminated message into err/err_cap (if err_cap > 0), so the caller
 * can log it.
 */
int ngx_scheme_destroy(ngx_scheme_vm_t *vm, char *err, int err_cap);

/*
 * Evaluate `code` (len bytes; need not be NUL-terminated).
 *
 * The scheme code is expected to call (ngx-say ...) to produce the
 * response body; the value of the last expression is ignored.  Call
 * ngx_scheme_output() to fetch the accumulated body.
 *
 * Returns 0 on success.  On a scheme error returns -1 and writes a
 * NUL-terminated message into err/err_cap (if err_cap > 0).
 */
int ngx_scheme_eval(ngx_scheme_vm_t *vm, const char *code, int len,
          char *err, int err_cap);

/*
 * After a successful ngx_scheme_eval(): pointer + length of the body
 * accumulated via ngx-say.  The pointer is owned by the handle and stays
 * valid until ngx_scheme_destroy().  *data may be NULL / *len 0 when the
 * body is empty.
 */
void ngx_scheme_output(ngx_scheme_vm_t *vm, const char **data, int *len);

#ifdef __cplusplus
}
#endif

#endif /* NGX_SCHEME_RUNTIME_H */
