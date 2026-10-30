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
 * Request model: the handle is bound to the nginx request it serves, and the
 * host functions read/write nginx's own request state directly (e.g. the
 * response status lives in r->headers_out.status).  The handle therefore
 * carries no per-feature fields: adding a new ngx-* function normally means
 * touching only that function, not the handle or the C API.
 *
 * Lifetime model (MVP): one VM is created per request and destroyed when
 * the request finishes.  A worker may run many requests concurrently and
 * the VM is not thread-safe, hence the per-request isolation.  (Future:
 * a per-worker VM with coroutine multiplexing.)
 */
#ifndef NGX_SCHEME_RUNTIME_H
#define NGX_SCHEME_RUNTIME_H

/* nginx types/macros (ngx_http_request_t, ngx_log_t, NGX_LOG_*); the
 * extern "C" wrapper only applies in C++ so the declarations get C linkage
 * there (plain C needs no wrapper) */
#ifdef __cplusplus
extern "C" {
#endif
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* opaque per-request scheme VM handle */
typedef struct ngx_scheme_vm_s ngx_scheme_vm_t;

/*
 * Create a fresh scheme VM bound to request `r`.  The handle keeps only the
 * request pointer: ngx-log writes to r->connection->log, and response state
 * (status, headers, ...) is read/written through the request itself, so new
 * scheme functions do not add fields here.  Returns NULL on failure.
 */
ngx_scheme_vm_t *ngx_scheme_create(ngx_http_request_t *r);

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
 * Return value:
 *   NGX_OK (0)          success; r->headers_out.status (0 = default 200),
 *                       headers and body are already set up
 *   NGX_ERROR (-1)      scheme error; err/err_cap holds the message
 *   >= 300              (ngx-exit N) asked to end the request with status N;
 *                       the caller returns N from the content handler so
 *                       nginx emits its error page
 * Exit statuses below 300 (including (ngx-exit 0)) return 0: the script was
 * aborted, but the response is sent normally by the caller.
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
