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
  VM         *vm;   /* the interpreter (created via scm_create_vm) */
  ngx_log_t  *log;  /* nginx error log ngx-log writes to */
  ngx_scheme_buf_t out;  /* response body accumulated via ngx-say */
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
  if ((ngx_uint_t) level > h->log->log_level) {
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
  ngx_log_error((ngx_uint_t) level, h->log, 0, "[scheme]: %s",
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

/* library table, same style as the builtin ones (see SCMMath::init) */
static const RegCProc ngx_scheme_lib[] = {
  RegCProc("ngx-say", ngx_scheme_stub_say, true),  /* variadic */
  RegCProc("ngx-log", ngx_scheme_stub_log, true),  /* variadic: level + messages */
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

ngx_scheme_vm_t *
ngx_scheme_create(ngx_log_t *log)
{
  ngx_scheme_vm_t *h = (ngx_scheme_vm_t *) malloc(sizeof(*h));
  if (h == NULL) {
    return NULL;
  }

  h->vm = NULL;
  h->log = log;
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
