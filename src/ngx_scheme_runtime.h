/*
 * ngx_scheme_runtime.h -- C++ bridge over the dummyscheme VM.
 *
 * The scheme interpreter itself is C++ (namespace Scheme).  This bridge
 * talks to the interpreter through the public API only (vm.h, which includes
 * scmapi.h -- never the VM internals).  The nginx module
 * (ngx_http_scheme_module.cpp) is C++ too, so the two translation units use
 * the class directly.
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
 * touching only that function, not the handle or the public API.
 *
 * Lifetime model: one VM per worker process, created lazily on the first
 * request (after fork) and never destroyed (worker exit reclaims it).  Every
 * handler declares one NgxSchemeVMHandle on the stack; its constructor
 * acquires the VM and registers itself in the VM's request-context slot,
 * its destructor tears both down.  The VM is not thread-safe, but nginx
 * runs a worker single-threaded, so a per-worker VM is isolated by the
 * event loop.  No malloc/free per-handle.
 *
 * All VM host stubs, buffer helpers and utility functions live as file-scope
 * statics in ngx_scheme_runtime.cpp.
 */
#ifndef NGX_SCHEME_RUNTIME_H
#define NGX_SCHEME_RUNTIME_H

#include "vm.h"

/* nginx headers are C: keep their declarations unmangled so the objects
 * produced from nginx's own C sources link against this C++ translation
 * unit. */
extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

/* growing byte buffer; used for the response body and for assembling
 * ngx-log messages; ctor zeroes fields, dtor frees data */
struct ngx_scheme_buf_s {
  char *data;
  int   len;
  int   cap;

  ngx_scheme_buf_s() : data(NULL), len(0), cap(0) {}
  ~ngx_scheme_buf_s()
  {
    ::free(data);
    data = NULL;
    len = 0;
    cap = 0;
  }

  ngx_scheme_buf_s(const ngx_scheme_buf_s&) = delete;
  ngx_scheme_buf_s& operator=(const ngx_scheme_buf_s&) = delete;
};
typedef struct ngx_scheme_buf_s ngx_scheme_buf_t;

/*
 * One per-request handle.  Declare on the stack inside each phase handler;
 * the constructor acquires the worker-shared VM and registers this handle
 * as the VM's request context, the destructor unregisters and frees the
 * output buffer.  No heap allocation per handle.  Copying is deleted (a
 * handle must refer to exactly one request + one stack frame).
 */
class NgxSchemeVMHandle
{
public:
  /*
   * Construct a handle bound to request `r`.  Acquires the worker-shared
   * VM and points its request-context registry slot at `this`.  After
   * construction, ok() tells you whether setup succeeded.
   */
  NgxSchemeVMHandle(ngx_http_request_t *r);
  ~NgxSchemeVMHandle();

  NgxSchemeVMHandle(const NgxSchemeVMHandle&) = delete;
  NgxSchemeVMHandle& operator=(const NgxSchemeVMHandle&) = delete;

  bool ok() const { return vm_ != NULL; }
  Scheme::VM *vm() const { return vm_; }
  ngx_http_request_t *request() const { return r_; }

  /*
   * Evaluate `code` (len bytes; need not be NUL-terminated).
   *
   * The scheme code is expected to call (ngx-say ...) to produce the
   * response body; the value of the last expression is ignored.  Call
   * output() to fetch the accumulated body.
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
  int eval(const char *code, int len, char *err, int err_cap);

  /*
   * After a successful eval(): pointer + length of the body accumulated via
   * ngx-say.  The pointer is owned by this handle and stays valid until
   * destruction.  *data may be NULL / *len 0 when the body is empty.
   */
  void output(const char **data, int *len) const;

  /* the three fields are accessed directly by file-scope VM stub functions
     (ngx-say, ngx-log, header stubs, …) which retrieve the handle via
     ngx_scheme_get_ctx() */
  Scheme::VM          *vm_;
  ngx_http_request_t  *r_;
  ngx_scheme_buf_t     out_;
};

/* legacy alias kept for registry lightuserdata casts inside the
 * implementation file only (the public type is NgxSchemeVMHandle) */
typedef NgxSchemeVMHandle ngx_scheme_vm_t;

#endif /* NGX_SCHEME_RUNTIME_H */
