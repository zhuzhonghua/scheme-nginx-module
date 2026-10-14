/*
 * ngx_scheme_runtime.h -- C bridge over the dummyscheme VM (see vm.h).
 *
 * The scheme interpreter itself is C++ (namespace Scheme).  This header
 * exposes the tiny C API that the pure-C nginx module
 * (ngx_http_scheme_module.c) uses, so the nginx-facing code never has to
 * touch C++.
 *
 * Lifetime model (MVP): one VM is created per request and destroyed when
 * the request finishes.  A worker may run many requests concurrently and
 * the VM is not thread-safe, hence the per-request isolation.  (Future:
 * a per-worker VM with coroutine multiplexing.)
 */
#ifndef NGX_SCHEME_RUNTIME_H
#define NGX_SCHEME_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* opaque per-request scheme VM handle */
typedef struct ngx_scheme_vm_s ngx_scheme_vm_t;

/* create a fresh scheme VM; returns NULL on failure */
ngx_scheme_vm_t *ngx_scheme_create(void);

/* destroy the VM and release every resource it owns */
void ngx_scheme_destroy(ngx_scheme_vm_t *vm);

/*
 * Evaluate `code` (len bytes; need not be NUL-terminated).
 *
 * The bytes written by `display`/`write`/`newline` during evaluation are
 * captured, and if the value of the last expression is a string it is
 * appended after them.  Call ngx_scheme_output() to fetch the result.
 *
 * Returns 0 on success.  On a scheme error returns -1 and writes a
 * NUL-terminated message into err/err_cap (if err_cap > 0).
 */
int ngx_scheme_eval(ngx_scheme_vm_t *vm, const char *code, int len,
                    char *err, int err_cap);

/*
 * After a successful ngx_scheme_eval(): pointer + length of the captured
 * body.  The pointer is owned by the handle and stays valid until
 * ngx_scheme_destroy().  *data may be NULL / *len 0 when the body is empty.
 */
void ngx_scheme_output(ngx_scheme_vm_t *vm, const char **data, int *len);

#ifdef __cplusplus
}
#endif

#endif /* NGX_SCHEME_RUNTIME_H */
