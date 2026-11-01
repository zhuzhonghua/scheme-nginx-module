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

/* registry key buffer for per-location cached values ("~code-%p" / "~env-%p") */
#define NGX_SCHEME_CACHE_KEY_LEN 64

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

/* fetch an integer argument: an integer, or a string that parses as one
 * (numeric strings are accepted); anything else raises a scheme error */
static scm_int
ngx_scheme_int_arg(VM *vm, ValueT *p, const char *who, const char *what)
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

  scm_error(vm, "%s: expected integer %s, got %s",
      who, what, scm_typename(vm, p));
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
  scm_int status = ngx_scheme_int_arg(vm, val, "ngx-status", "HTTP status");

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

/*
 * Response header helpers.  Names are matched case-insensitively and '_' is
 * normalised to '-', so "X_Custom" and "x-custom" address the same header.
 * A leading stack buffer keeps the common case allocation-free; longer names
 * are rejected instead of truncated.
 */
#define NGX_SCHEME_HEADER_NAME_MAX 128

/* stack buffer for variable names; longer names are rejected, not truncated */
#define NGX_SCHEME_VAR_NAME_MAX 128

/* lower-case a scheme header name into dst; returns its length */
static int
ngx_scheme_header_name(VM *vm, ValueT *p, u_char *dst, size_t cap,
    const char *who)
{
  const char *s;
  int n;

  if (scm_is_string(vm, p)) {
    s = scm_get_string(vm, p, &n);
  } else if (issym(p)) {
    s = Ssstr(symref(p));
    n = (int) strlen(s);
  } else {
    scm_error(vm, "%s: expected header name, got %s",
        who, scm_typename(vm, p));
    return 0;  /* not reached */
  }

  if (n <= 0 || (size_t) n >= cap) {
    scm_error(vm, "%s: invalid header name", who);
  }

  for (int i = 0; i < n; i++) {
    u_char c = (u_char) s[i];
    if (c == '_') {
      c = '-';
    }
    dst[i] = (u_char) ngx_tolower(c);
  }
  dst[n] = '\0';

  return n;
}

/* look up a live response header by its already-lowercased name */
static ngx_table_elt_t *
ngx_scheme_header_find(ngx_http_request_t *r, const u_char *name, size_t len)
{
  ngx_list_part_t *part;
  ngx_table_elt_t *h;
  ngx_uint_t        i;

  part = &r->headers_out.headers.part;
  h = (ngx_table_elt_t *) part->elts;

  for (i = 0; /* void */; i++) {
    if (i >= part->nelts) {
      if (part->next == NULL) {
        break;
      }
      part = part->next;
      h = (ngx_table_elt_t *) part->elts;
      i = 0;
    }

    /* hash == 0 marks a deleted entry, the nginx convention */
    if (h[i].hash == 0) {
      continue;
    }

    if (h[i].key.len == len
        && ngx_strncasecmp(h[i].key.data, (u_char *) name, len) == 0)
    {
      return &h[i];
    }
  }

  return NULL;
}

/* copy a scheme value into the request pool as an nginx string */
static void
ngx_scheme_header_copy_value(VM *vm, ValueT *p, ngx_pool_t *pool,
    ngx_str_t *out)
{
  int n = 0;
  const char *s = scm_is_string(vm, p) ? scm_get_string(vm, p, &n)
                                       : scm_tostring(vm, p, &n);

  if (n < 0) {
    n = 0;
  }

  u_char *data = (u_char *) ngx_pnalloc(pool, (size_t) n + 1);
  if (data == NULL) {
    throw "OutOfMemory";
  }

  if (n > 0) {
    ngx_memcpy(data, s, (size_t) n);
  }
  data[n] = '\0';

  out->data = data;
  out->len = (size_t) n;
}

/* append a new header entry; the key is copied into the request pool */
static ngx_table_elt_t *
ngx_scheme_header_add(ngx_http_request_t *r, const u_char *name, size_t len,
    ngx_str_t *value)
{
  u_char *key;
  ngx_table_elt_t *h;

  h = (ngx_table_elt_t *) ngx_list_push(&r->headers_out.headers);
  if (h == NULL) {
    throw "OutOfMemory";
  }

  key = (u_char *) ngx_pnalloc(r->pool, len);
  if (key == NULL) {
    throw "OutOfMemory";
  }
  ngx_memcpy(key, name, len);

  h->hash = 1;
  h->key.data = key;
  h->key.len = len;
  h->value = *value;

  return h;
}

/*
 * (ngx-header-ref NAME) -- read a response header.  NAME is a string or a
 * symbol; matching ignores case and treats '_' like '-'.  Returns the first
 * value as a string, or #f when the header is not set.  Content-Type and
 * Content-Length are answered from the request fields nginx uses for them,
 * so they read back what was set with ngx-header-set!.
 */
static ValueT
ngx_scheme_stub_header_ref(VM *vm, ValueT *namep)
{
  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);
  u_char name[NGX_SCHEME_HEADER_NAME_MAX] = {0};
  int nlen = ngx_scheme_header_name(vm, namep, name, sizeof(name),
      "ngx-header-ref");

  if (nlen == (int) (sizeof("content-type") - 1)
      && ngx_strncmp(name, "content-type", nlen) == 0)
  {
    if (h->r->headers_out.content_type.len == 0) {
      return scm_make_bool(vm, false);
    }
    return scm_make_string(vm,
        (const char *) h->r->headers_out.content_type.data,
        (int) h->r->headers_out.content_type.len);
  }

  if (nlen == (int) (sizeof("content-length") - 1)
      && ngx_strncmp(name, "content-length", nlen) == 0)
  {
    if (h->r->headers_out.content_length_n < 0) {
      return scm_make_bool(vm, false);
    }
    char buf[NGX_INT_T_LEN];
    int n = snprintf(buf, sizeof(buf), "%lld",
        (long long) h->r->headers_out.content_length_n);
    return scm_make_string(vm, buf, n);
  }

  ngx_table_elt_t *hdr = ngx_scheme_header_find(h->r, name, (size_t) nlen);
  if (hdr == NULL) {
    return scm_make_bool(vm, false);
  }

  return scm_make_string(vm, (const char *) hdr->value.data,
      (int) hdr->value.len);
}

/*
 * (ngx-header-set! NAME VALUE) -- set, replace or remove a response header.
 * VALUE #f removes the header; any other value is converted to a string (same
 * semantics as ngx-say).  Setting an existing name replaces its value in
 * place, so a header is never duplicated.  Content-Type and Content-Length
 * are written to the request fields nginx emits them from; Location also
 * updates r->headers_out.location so nginx's absolute-redirect handling sees
 * it.
 */
static ValueT
ngx_scheme_stub_header_set(VM *vm, ValueT *namep, ValueT *valp)
{
  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);
  ngx_http_request_t *r = h->r;
  u_char name[NGX_SCHEME_HEADER_NAME_MAX] = {0};
  int nlen = ngx_scheme_header_name(vm, namep, name, sizeof(name),
      "ngx-header-set!");
  bool remove = scm_is_false(vm, valp);

  if (nlen == (int) (sizeof("content-type") - 1)
      && ngx_strncmp(name, "content-type", nlen) == 0)
  {
    ngx_table_elt_t *old = ngx_scheme_header_find(r, name, (size_t) nlen);
    if (old != NULL) {
      old->hash = 0;
    }

    if (remove) {
      ngx_str_null(&r->headers_out.content_type);
      r->headers_out.content_type_len = 0;
      r->headers_out.content_type_lowcase = NULL;
    } else {
      ngx_scheme_header_copy_value(vm, valp, r->pool,
          &r->headers_out.content_type);
      r->headers_out.content_type_len = r->headers_out.content_type.len;
      r->headers_out.content_type_lowcase = NULL;
    }
    return_void();
  }

  if (nlen == (int) (sizeof("content-length") - 1)
      && ngx_strncmp(name, "content-length", nlen) == 0)
  {
    if (remove) {
      ngx_http_clear_content_length(r);
      return_void();
    }

    scm_int v = ngx_scheme_int_arg(vm, valp, "ngx-header-set!",
        "content length");
    if (v < 0) {
      scm_error(vm, "ngx-header-set!: bad content length: %d", (int) v);
    }
    ngx_http_clear_content_length(r);
    r->headers_out.content_length_n = (off_t) v;
    return_void();
  }

  ngx_table_elt_t *old = ngx_scheme_header_find(r, name, (size_t) nlen);

  if (remove) {
    if (old != NULL) {
      old->hash = 0;
      if (r->headers_out.location == old) {
        r->headers_out.location = NULL;
      }
    }
    return_void();
  }

  ngx_str_t value;
  ngx_scheme_header_copy_value(vm, valp, r->pool, &value);

  if (old != NULL) {
    old->value = value;
    return_void();
  }

  ngx_table_elt_t *hdr = ngx_scheme_header_add(r, name, (size_t) nlen, &value);
  if (nlen == (int) (sizeof("location") - 1)
      && ngx_strncmp(name, "location", nlen) == 0)
  {
    r->headers_out.location = hdr;
  }

  return_void();
}

/*
 * (ngx-var-ref NAME) -- value of an nginx variable as a string, or #f when
 * the variable does not exist or is not found at this point of the request.
 * NAME is a string or a symbol and is lowercased before lookup (nginx
 * variable names are lowercase); '_' is kept, so http_user_agent,
 * arg_name, cookie_x and friends all work.  No leading '$' is expected.
 * The value is copied into a scheme string before returning, so it stays
 * valid after the request pool is recycled.
 */
static ValueT
ngx_scheme_stub_var_ref(VM *vm, ValueT *namep)
{
  ngx_scheme_vm_s *h = ngx_scheme_get_ctx(vm);
  const char *s;
  int n;

  if (scm_is_string(vm, namep)) {
    s = scm_get_string(vm, namep, &n);
  } else if (issym(namep)) {
    s = Ssstr(symref(namep));
    n = (int) strlen(s);
  } else {
    scm_error(vm, "ngx-var-ref: expected variable name, got %s",
        scm_typename(vm, namep));
    return scm_make_bool(vm, false);  /* not reached */
  }

  if (n <= 0 || (size_t) n >= NGX_SCHEME_VAR_NAME_MAX) {
    scm_error(vm, "ngx-var-ref: invalid variable name");
  }

  u_char name[NGX_SCHEME_VAR_NAME_MAX] = {0};
  for (int i = 0; i < n; i++) {
    name[i] = (u_char) ngx_tolower((u_char) s[i]);
  }
  name[n] = '\0';

  ngx_str_t vname;
  vname.data = name;
  vname.len = (size_t) n;

  ngx_http_variable_value_t *vv = ngx_http_get_variable(h->r, &vname,
      ngx_hash_key_lc(name, (size_t) n));

  if (vv == NULL || vv->not_found) {
    return scm_make_bool(vm, false);
  }

  return scm_make_string(vm, (const char *) vv->data, (int) vv->len);
}

/* library table, same style as the builtin ones (see SCMMath::init) */
static const RegCProc ngx_scheme_lib[] = {
  RegCProc("ngx-say", ngx_scheme_stub_say, true),  /* variadic */
  RegCProc("ngx-log", ngx_scheme_stub_log, true),  /* variadic: level + messages */
  RegCProc("ngx-exit", ngx_scheme_stub_exit),      /* (status) */
  RegCProc("ngx-header-ref", ngx_scheme_stub_header_ref),   /* (name) */
  RegCProc("ngx-header-set!", ngx_scheme_stub_header_set),  /* (name value) */
  RegCProc("ngx-var-ref", ngx_scheme_stub_var_ref),         /* (name) */
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

/*
 * Registry keys of the two ngx-status metatable callbacks.  They are created
 * once when the worker's VM is initialized and shared by the genv metatable
 * and every per-location env metatable (native procs are immutable, so one
 * instance serves all call sites).
 */
#define NGX_SCHEME_STATUS_INDEX_PROC   "~ngx-status-index"
#define NGX_SCHEME_STATUS_NEWINDEX_PROC "~ngx-status-newindex"

/*
 * The interpreter is shared by the whole worker process: it is created
 * lazily on the first request, registered once, and never destroyed (the
 * worker's exit reclaims it).  Per-request state lives in the handle and is
 * re-pointed through the registry context on every request.
 */
static VM *
ngx_scheme_acquire_vm()
{
  static VM *shared_vm = NULL;   /* one per worker process */

  if (shared_vm != NULL) {
    return shared_vm;
  }

  VM *vm = NULL;
  try {
    vm = scm_create_vm();
    if (vm == NULL) {
      return NULL;
    }

    scm_register_lib(vm, ngx_scheme_lib);
    ngx_scheme_inject_log_consts(vm);
    ngx_scheme_inject_status_consts(vm);

    /* ngx-status is a virtual global backed by r->headers_out.status: it is
       never stored in genv; reads and (set! ...) writes go through the genv
       metatable callbacks built here (native procs that never reach genv) */
    Sgcvar4(vm, mt, mk, iv, nv);
    *mt = scm_make_table(vm);
    *iv = scm_make_native_proc(vm, RegCProc(NGX_SCHEME_STATUS_INDEX_PROC, ngx_scheme_stub_status_index));
    *nv = scm_make_native_proc(vm, RegCProc(NGX_SCHEME_STATUS_NEWINDEX_PROC, ngx_scheme_stub_status_newindex));
    /* keep one shared instance of each callback in the registry; per-location
       env metatables fetch them from there instead of building new procs */
    scm_registry_set(vm, NGX_SCHEME_STATUS_INDEX_PROC, *iv);
    scm_registry_set(vm, NGX_SCHEME_STATUS_NEWINDEX_PROC, *nv);
    *mk = scm_make_string(vm, "__index");
    scm_table_set(vm, mt, mk, *iv);
    *mk = scm_make_string(vm, "__newindex");
    scm_table_set(vm, mt, mk, *nv);

    scm_set_global_metatable(vm, mt);
  }
  catch (...) {
    if (vm != NULL) {
      scm_destroy_vm(vm);
    }
    return NULL;
  }

  shared_vm = vm;
  return vm;
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
    h->vm = ngx_scheme_acquire_vm();
    if (h->vm == NULL) {
      free(h);
      return NULL;
    }

    /* register this handle as a lightuserdata in the VM registry; the VM is
       shared, so every request re-points the context at its own handle */
    scm_registry_set(h->vm, NGX_SCHEME_CTX_KEY,
        scm_make_lightuserdata(h->vm, h));
  }
  catch (...) {
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

  /* the VM is shared and outlives the request; only the per-request handle
     and its output buffer are released */
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
    /* compile once per location script into a per-location sandbox env and
       cache both in the registry for the VM's lifetime; requests just apply
       the cached closure.  define/set! write into the env (not visible from
       other locations or genv), reads fall back to genv */
    char code_key[NGX_SCHEME_CACHE_KEY_LEN] = {0};
    char env_key[NGX_SCHEME_CACHE_KEY_LEN] = {0};
    snprintf(code_key, sizeof(code_key), "~code-%p", (const void *) code);
    snprintf(env_key, sizeof(env_key), "~env-%p", (const void *) code);

    Sgcvar2(h->vm, fv, envv);
    *fv = scm_registry_get(h->vm, code_key);
    if (scm_is_null(h->vm, fv)) {
      *envv = scm_make_table(h->vm);
      *fv = scm_compile(h->vm, envv, code, len);

      /* scm_compile installs {__index = genv} on the env; add the shared
         ngx-status __newindex callback so (set! ngx-status ...) still reaches
         the request instead of being swallowed by the env */
      Sgcvar2(h->vm, niv, nik);
      *niv = scm_registry_get(h->vm, NGX_SCHEME_STATUS_NEWINDEX_PROC);
      *nik = scm_make_string(h->vm, "__newindex");
      hashtableref(hashtableref(envv)->getmetatable())->set(h->vm, nik, niv);

      scm_registry_set(h->vm, env_key, *envv);
      scm_registry_set(h->vm, code_key, *fv);
    }

    ValueT result;
    if (!scm_apply(h->vm, fv, NULL, 0, &result)) {
      /* scm_apply reports a scheme error as the result string */
      if (err_cap > 0) {
        int n = 0;
        const char *msg = scm_get_string(h->vm, &result, &n);
        snprintf(err, err_cap, "scheme error: %s", msg != NULL ? msg : "unknown");
      }
      return -1;
    }
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
