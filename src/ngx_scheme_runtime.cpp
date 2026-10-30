/*
 * ngx_scheme_runtime.cpp -- C bridge over the dummyscheme VM.
 *
 * Owns one VM per request.  Output follows the model :
 * the scheme code calls the host function `ngx-say` to hand strings to
 * nginx, which appends them to a per-request buffer and sends that buffer as
 * the response body.  The VM's own IO streams (display/write -> stderr) are
 * never touched.
 *
 * The request handle is stored in the VM's registry as a lightuserdata
 * (scm_registry_set + scm_make_lightuserdata), a host function finds its
 * context via scm_registry_get + scm_to_lightuserdata. The response buffer
 * is owned by this handle (plain growing buffer); the final body is copied into the nginx
 * request pool by the content handler.
 *
 * This file talks to the interpreter through the public API (vm.h, which
 * includes scmapi.h) only.
 */
#include "vm.h"
#include "ngx_scheme_runtime.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

using namespace Scheme;

/* registry key holding this request's handle (a lightuserdata) */
#define NGX_SCHEME_CTX_KEY "~ngx-scheme-ctx"

/* growing byte buffer; used for the response body and for assembling
 * ngx-log messages */
struct ngx_scheme_buf_s {
  char *data;
  int   len;
  int   cap;
};
typedef struct ngx_scheme_buf_s ngx_scheme_buf_t;

/* make sure `add` more bytes fit, growing the buffer if needed */
static void
ngx_scheme_buf_reserve(ngx_scheme_buf_s *b, int add)
{
  if (b->len + add <= b->cap) {
    return;
  }

  int ncap = b->cap ? b->cap * 2 : 256;
  while (ncap < b->len + add) {
    ncap *= 2;
  }

  char *nb = (char *) realloc(b->data, (size_t) ncap);
  if (nb == NULL) {
    throw "OutOfMemory";
  }
  b->data = nb;
  b->cap = ncap;
}

static void
ngx_scheme_buf_append(ngx_scheme_buf_s *b, const char *s, int n)
{
  if (n <= 0) {
    return;
  }
  ngx_scheme_buf_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
}

/* append one value with ngx-say semantics: strings verbatim, anything else
 * via write semantics */
static void
ngx_scheme_buf_append_value(ngx_scheme_buf_s *b, VM *vm, ValueT *p)
{
  int n = 0;
  const char *s = scm_is_string(vm, p) ? scm_get_string(vm, p, &n)
                                       : scm_tostring(vm, p, &n);
  ngx_scheme_buf_append(b, s, n);
}

static void
ngx_scheme_buf_free(ngx_scheme_buf_s *b)
{
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

/* RAII guard for a local buffer: frees it on every exit path (normal return
 * or throw), so callers never need try/catch for temporary buffers */
struct ngx_scheme_buf_guard_s {
  ngx_scheme_buf_t b;

  ngx_scheme_buf_guard_s() {
    b.data = NULL;
    b.len = 0;
    b.cap = 0;
  }
  ~ngx_scheme_buf_guard_s() {
    ngx_scheme_buf_free(&b);
  }
};
typedef struct ngx_scheme_buf_guard_s ngx_scheme_buf_guard_t;

struct ngx_scheme_vm_s {
  VM                 *vm;   /* the interpreter (created via scm_create_vm) */
  ngx_http_request_t *r;    /* request this VM serves; also holds the log */
  ngx_scheme_buf_t    out;  /* response body accumulated via ngx-say */
};

/*
 * Control-flow-only exception thrown by (ngx-exit ...).  It carries the exit
 * status and unwinds out of the interpreter's eval loop; ngx_scheme_eval()
 * catches it and turns it into its return value, where it means "success,
 * script aborted on purpose" rather than an error.
 */
struct ngx_scheme_exit_signal {
  int status;

  ngx_scheme_exit_signal(int s) : status(s) {}
};

/* resolve the request handle stored in the VM registry */
static ngx_scheme_vm_s *
ngx_scheme_get_ctx(VM *vm)
{
  ValueT ctx = scm_registry_get(vm, NGX_SCHEME_CTX_KEY);
  if (!scm_is_lightuserdata(vm, &ctx)) {
    throw "no request context";
  }
  return (ngx_scheme_vm_s *) scm_to_lightuserdata(vm, &ctx);
}

/*
 * The ngx-log host function: (ngx-log LEVEL MESSAGE ...).  LEVEL is an
 * integer matching nginx's NGX_LOG_* values; the remaining arguments are
 * concatenated with ngx-say semantics and written straight to the request's
 * error log via ngx_log_error.
 */
static ValueT
ngx_scheme_stub_log(VM* vm, ValueT* rest)
{
  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);

  if (scm_is_null(vm, rest)) {
    scm_error(vm, "ngx-log: expected integer log level, got no argument");
  }

  ValueT *levelp = scm_car(vm, rest);
  if (!scm_is_int(vm, levelp)) {
    scm_error(vm, "ngx-log: expected integer log level, got %s",
        scm_typename(vm, levelp));
  }
  int level = (int) scm_get_int(vm, levelp);
  if (level < NGX_LOG_STDERR || level > NGX_LOG_DEBUG) {
    scm_error(vm, "ngx-log: bad log level: %d", level);
  }

  /* skip assembling the message when the configured level filters it out */
  if ((ngx_uint_t) level > h->r->connection->log->log_level) {
    return_void();
  }

  /* msg is a local temporary buffer: the RAII guard frees it on every exit
     path (normal or throw), so no try/catch is needed */
  ngx_scheme_buf_guard_t msg;

  rest = scm_cdr(vm, rest);
  while (!scm_is_null(vm, rest)) {
    ngx_scheme_buf_append_value(&msg.b, vm, scm_car(vm, rest));
    rest = scm_cdr(vm, rest);
  }
  ngx_log_error((ngx_uint_t) level, h->r->connection->log, 0, "[scheme]: %s",
      msg.b.data != NULL ? msg.b.data : "");
  return_void();
}

/*
 * The ngx-say host function: converts every argument to a string and appends
 * it to the request's output buffer.  Accepts any number of arguments.  No
 * try/catch here: values go straight into h->out, which the handle owns and
 * destroy() releases even if appending throws.
 */
static ValueT
ngx_scheme_stub_say(VM* vm, ValueT* rest)
{
  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);

  while (!scm_is_null(vm, rest)) {
    ngx_scheme_buf_append_value(&h->out, vm, scm_car(vm, rest));
    rest = scm_cdr(vm, rest);
  }

  return_void();
}

/* fetch an HTTP status argument: an integer, or a string that parses as one
 * (numeric strings are accepted); anything else raises a scheme error */
static scm_int
ngx_scheme_status_arg(VM *vm, ValueT *p, const char *who)
{
  if (scm_is_int(vm, p)) {
    return scm_get_int(vm, p);
  }

  if (scm_is_string(vm, p)) {
    int n = 0;
    const char *s = scm_get_string(vm, p, &n);
    char *end = NULL;
    long v = strtol(s, &end, 10);
    while (end != NULL && (*end == ' ' || *end == '\t')) {
      end++;
    }
    if (end != NULL && *end == '\0' && end != s) {
      return (scm_int) v;
    }
  }

  scm_error(vm, "%s: expected integer HTTP status, got %s",
      who, scm_typename(vm, p));
  return 0;  /* not reached */
}

/* whether a metatable callback key is the symbol ngx-status */
static bool
ngx_scheme_is_status_key(ValueT *key)
{
  return issym(key) && strcmp(Ssstr(symref(key)), "ngx-status") == 0;
}

/*
 * The genv metatable __index callback: ngx-status is never stored in the
 * global table, so every read lands here and is answered from the request
 * state (err_status first, then headers_out.status; 0 when neither is set).
 * Any other name stays absent (the undefined value), so scripts keep the
 * normal "undefined global var" error.
 */
static ValueT
ngx_scheme_stub_status_index(VM* vm, ValueT* tbl, ValueT* key)
{
  (void) tbl;

  if (!ngx_scheme_is_status_key(key)) {
    return *Sundefined;
  }

  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);
  ngx_uint_t st = h->r->err_status != 0 ? h->r->err_status
                                        : h->r->headers_out.status;
  return scm_make_int(vm, (scm_int) st);
}

/*
 * The genv metatable __newindex callback: (set! ngx-status n) lands here
 * (the name is never stored) and is written into the request at once.
 * Numeric strings are coerced; a value that is not a number at all raises a
 * scheme error; an out-of-range number (not 100..999) is logged and ignored
 * without raising.  #f declines any other name, so its write takes the
 * normal insertion path.
 */
static ValueT
ngx_scheme_stub_status_newindex(VM* vm, ValueT* tbl, ValueT* key, ValueT* val)
{
  (void) tbl;

  if (!ngx_scheme_is_status_key(key)) {
    return scm_make_bool(vm, false);
  }

  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);
  scm_int status = ngx_scheme_status_arg(vm, val, "ngx-status");

  /* the status line must be 3 digits (RFC 7230 sec 3.1.2) */
  if (status < 100 || status > 999) {
    ngx_log_error(NGX_LOG_ERR, h->r->connection->log, 0,
        "invalid HTTP status code %d", (int) status);
    return_void();
  }

  h->r->headers_out.status = (ngx_uint_t) status;
  h->r->err_status = 0;
  h->r->headers_out.status_line.len = 0;
  return_void();
}

/*
 * (ngx-exit STATUS) -- write the response status into the request and abort
 * the script.  A status of 0 means "stop now, keep the default 200"; any
 * other integer is used as-is (no range check, so nginx's special codes
 * like 444 work).  Aborting is done by throwing a control-flow exception
 * caught in ngx_scheme_eval(), so the rest of the scheme code (including
 * later ngx-say/ngx-log calls) does not run.
 */
static ValueT
ngx_scheme_stub_exit(VM* vm, ValueT* statusp)
{
  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);

  if (!scm_is_int(vm, statusp)) {
    scm_error(vm, "ngx-exit: expected integer HTTP status, got %s",
        scm_typename(vm, statusp));
  }

  scm_int status = scm_get_int(vm, statusp);
  int exit_status = 0;

  if (status != 0) {
    exit_status = (int) status;
    h->r->headers_out.status = (ngx_uint_t) exit_status;
  }

  throw ngx_scheme_exit_signal(exit_status);
}

/* library table, same style as the builtin ones (see SCMMath::init) */
static const RegCProc ngx_scheme_lib[] = {
  RegCProc("ngx-say", ngx_scheme_stub_say, true),  /* variadic */
  RegCProc("ngx-log", ngx_scheme_stub_log, true),  /* variadic: level + messages */
  RegCProc("ngx-exit", ngx_scheme_stub_exit),      /* (status) */
  RegCProc(NULL, -1)
};

/* Register the log-level constants as top-level variables (ngx-err etc.). */
static void
ngx_scheme_inject_log_consts(VM *vm)
{
  static const struct {
    const char *name;
    ngx_uint_t  level;
  } log_levels[] = {
    { "ngx-stderr", NGX_LOG_STDERR },
    { "ngx-emerg",  NGX_LOG_EMERG  },
    { "ngx-alert",  NGX_LOG_ALERT  },
    { "ngx-crit",   NGX_LOG_CRIT   },
    { "ngx-err",    NGX_LOG_ERR    },
    { "ngx-warn",   NGX_LOG_WARN   },
    { "ngx-notice", NGX_LOG_NOTICE },
    { "ngx-info",   NGX_LOG_INFO   },
    { "ngx-debug",  NGX_LOG_DEBUG  },
  };

  for (int i = 0; i < (int) (sizeof(log_levels) / sizeof(log_levels[0])); i++) {
    scm_set_global(vm, log_levels[i].name,
        scm_make_int(vm, (scm_int) log_levels[i].level));
  }
}

/* Register the HTTP status constants (ngx-http-*) as top-level variables.
 * They mirror nginx's NGX_HTTP_* codes so scheme code can write
 * (ngx-exit ngx-http-not-found) instead of a bare number. */
static void
ngx_scheme_inject_status_consts(VM *vm)
{
  static const struct {
    const char *name;
    ngx_int_t   status;
  } http_status[] = {
    { "ngx-http-continue",               NGX_HTTP_CONTINUE },
    { "ngx-http-switching-protocols",    NGX_HTTP_SWITCHING_PROTOCOLS },
    { "ngx-http-ok",                     NGX_HTTP_OK },
    { "ngx-http-created",                NGX_HTTP_CREATED },
    { "ngx-http-accepted",               NGX_HTTP_ACCEPTED },
    { "ngx-http-no-content",             NGX_HTTP_NO_CONTENT },
    { "ngx-http-partial-content",        NGX_HTTP_PARTIAL_CONTENT },
    { "ngx-http-moved-permanently",      NGX_HTTP_MOVED_PERMANENTLY },
    { "ngx-http-moved-temporarily",      NGX_HTTP_MOVED_TEMPORARILY },
    { "ngx-http-see-other",              NGX_HTTP_SEE_OTHER },
    { "ngx-http-not-modified",           NGX_HTTP_NOT_MODIFIED },
    { "ngx-http-temporary-redirect",     NGX_HTTP_TEMPORARY_REDIRECT },
    { "ngx-http-permanent-redirect",     NGX_HTTP_PERMANENT_REDIRECT },
    { "ngx-http-bad-request",            NGX_HTTP_BAD_REQUEST },
    { "ngx-http-unauthorized",           NGX_HTTP_UNAUTHORIZED },
    { "ngx-http-forbidden",              NGX_HTTP_FORBIDDEN },
    { "ngx-http-not-found",              NGX_HTTP_NOT_FOUND },
    { "ngx-http-not-allowed",            NGX_HTTP_NOT_ALLOWED },
    { "ngx-http-proxy-auth-required",    NGX_HTTP_PROXY_AUTH_REQUIRED },
    { "ngx-http-request-time-out",       NGX_HTTP_REQUEST_TIME_OUT },
    { "ngx-http-conflict",               NGX_HTTP_CONFLICT },
    { "ngx-http-length-required",        NGX_HTTP_LENGTH_REQUIRED },
    { "ngx-http-precondition-failed",    NGX_HTTP_PRECONDITION_FAILED },
    { "ngx-http-request-entity-too-large", NGX_HTTP_REQUEST_ENTITY_TOO_LARGE },
    { "ngx-http-request-uri-too-large",  NGX_HTTP_REQUEST_URI_TOO_LARGE },
    { "ngx-http-unsupported-media-type", NGX_HTTP_UNSUPPORTED_MEDIA_TYPE },
    { "ngx-http-range-not-satisfiable",  NGX_HTTP_RANGE_NOT_SATISFIABLE },
    { "ngx-http-misdirected-request",    NGX_HTTP_MISDIRECTED_REQUEST },
    { "ngx-http-too-many-requests",      NGX_HTTP_TOO_MANY_REQUESTS },
    { "ngx-http-close",                  NGX_HTTP_CLOSE },
    { "ngx-http-internal-server-error",  NGX_HTTP_INTERNAL_SERVER_ERROR },
    { "ngx-http-not-implemented",        NGX_HTTP_NOT_IMPLEMENTED },
    { "ngx-http-bad-gateway",            NGX_HTTP_BAD_GATEWAY },
    { "ngx-http-service-unavailable",    NGX_HTTP_SERVICE_UNAVAILABLE },
    { "ngx-http-gateway-time-out",       NGX_HTTP_GATEWAY_TIME_OUT },
    { "ngx-http-version-not-supported",  NGX_HTTP_VERSION_NOT_SUPPORTED },
    { "ngx-http-insufficient-storage",   NGX_HTTP_INSUFFICIENT_STORAGE },
  };

  for (int i = 0; i < (int) (sizeof(http_status) / sizeof(http_status[0])); i++) {
    scm_set_global(vm, http_status[i].name,
        scm_make_int(vm, (scm_int) http_status[i].status));
  }
}

ngx_scheme_vm_t *
ngx_scheme_create(ngx_http_request_t *r)
{
  ngx_scheme_vm_t *h = (ngx_scheme_vm_t *) malloc(sizeof(*h));
  if (h == NULL) {
    return NULL;
  }

  h->vm = NULL;
  h->r = r;
  h->out.data = NULL;
  h->out.len = 0;
  h->out.cap = 0;

  try {
    h->vm = scm_create_vm();
    if (h->vm == NULL) {
      free(h);
      return NULL;
    }

    /* register this handle as a lightuserdata in the VM registry */
    scm_registry_set(h->vm, NGX_SCHEME_CTX_KEY,
        scm_make_lightuserdata(h->vm, h));
    scm_register_lib(h->vm, ngx_scheme_lib);
    ngx_scheme_inject_log_consts(h->vm);
    ngx_scheme_inject_status_consts(h->vm);

    /* ngx-status is a virtual global backed by r->headers_out.status: it is
       never stored in genv; reads and (set! ...) writes go through the genv
       metatable callbacks built here (native procs that never reach genv) */
    Sgcvar3(h->vm, mt, mk, mv);
    *mt = scm_make_table(h->vm);

    ValueT nmsym;
    CProc cf;

    *mk = scm_make_string(h->vm, "__index");
    cf.cp2 = ngx_scheme_stub_status_index;
    setnativeproc(mv, Sr4(h->vm, NativeProcObj,
        G(h->vm)->getintern()->internsym("~ngx-status-index", &nmsym),
        cf, 2, false));
    scm_table_set(h->vm, mt, mk, *mv);

    *mk = scm_make_string(h->vm, "__newindex");
    cf.cp3 = ngx_scheme_stub_status_newindex;
    setnativeproc(mv, Sr4(h->vm, NativeProcObj,
        G(h->vm)->getintern()->internsym("~ngx-status-newindex", &nmsym),
        cf, 3, false));
    scm_table_set(h->vm, mt, mk, *mv);

    scm_set_global_metatable(h->vm, mt);
  }
  catch (...) {
    if (h->vm != NULL) {
      scm_destroy_vm(h->vm);
    }
    free(h);
    return NULL;
  }

  return h;
}

static void
ngx_scheme_free(ngx_scheme_vm_s *h)
{
  if (h == NULL) {
    return;
  }
  ngx_scheme_buf_free(&h->out);
  free(h);
}

int
ngx_scheme_destroy(ngx_scheme_vm_t *h, char *err, int err_cap)
{
  if (err_cap > 0) {
    err[0] = '\0';
  }
  if (h == NULL) {
    return 0;  /* nothing owned, nothing to tear down */
  }

  if (h->vm != NULL) {
    try {
      scm_destroy_vm(h->vm);
    }
    catch (const char *e) {
      /* teardown must not throw out of the request path, but the failure
          is reported to the caller, which logs it */
      if (err_cap > 0) {
        snprintf(err, err_cap, "scheme teardown error: %s",
            e != NULL ? e : "unknown");
      }
      ngx_scheme_free(h);
      return -1;
    }
    catch (...) {
      if (err_cap > 0) {
        snprintf(err, err_cap, "scheme teardown error: unknown exception");
      }
      ngx_scheme_free(h);
      return -1;
    }
  }

  ngx_scheme_free(h);
  return 0;
}

int
ngx_scheme_eval(ngx_scheme_vm_t *h, const char *code, int len,
                char *err, int err_cap)
{
  if (h == NULL || h->vm == NULL) {
    return -1;
  }

  if (err_cap > 0) {
    err[0] = '\0';
  }

  try {
    (void) scm_eval_string(h->vm, code, len);
  }
  catch (const ngx_scheme_exit_signal &e) {
    /* (ngx-exit N) aborted the script; that is not an error.  Statuses >= 300
     * are reported back so the caller can let nginx emit its error page; the
     * rest are a normal response and collapse to NGX_OK. */
    return e.status >= NGX_HTTP_SPECIAL_RESPONSE ? e.status : NGX_OK;
  }
  catch (const char *e) {
    if (err_cap > 0) {
      snprintf(err, err_cap, "scheme error: %s",
          e != NULL ? e : "unknown");
    }
    return -1;
  }
  catch (...) {
    if (err_cap > 0) {
      snprintf(err, err_cap, "scheme error: unknown exception");
    }
    return -1;
  }

  return 0;
}

void
ngx_scheme_output(ngx_scheme_vm_t *h, const char **data, int *len)
{
  if (data != NULL) {
    *data = NULL;
  }
  if (len != NULL) {
    *len = 0;
  }

  if (h == NULL) {
    return;
  }

  if (data != NULL) {
    *data = h->out.data;
  }
  if (len != NULL) {
    *len = h->out.len;
  }
}
