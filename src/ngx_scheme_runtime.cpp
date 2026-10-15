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

struct ngx_scheme_vm_s {
  VM    *vm;   /* the interpreter (created via scm_create_vm) */
  char  *buf;  /* growing response buffer (owned by this handle) */
  int    len;  /* used bytes */
  int    cap;  /* allocated bytes */
};

static void ngx_scheme_append(ngx_scheme_vm_s *h, const char *s, int n);

static void
ngx_scheme_append(ngx_scheme_vm_s *h, const char *s, int n)
{
  if (n <= 0) {
    return;
  }

  if (h->len + n > h->cap) {
    int ncap = h->cap ? h->cap * 2 : 256;
    while (ncap < h->len + n) {
      ncap *= 2;
    }
    char *nb = (char *) realloc(h->buf, ncap);
    if (nb == NULL) {
      throw "OutOfMemory";
    }
    h->buf = nb;
    h->cap = ncap;
  }

  memcpy(h->buf + h->len, s, n);
  h->len += n;
}

/*
 * The ngx-say host function: converts every argument to a string and appends
 * it to the request's output buffer.  Strings are appended verbatim (display
 * semantics, like Lua's tostring); any other value is printed with
 * write semantics.  Accepts any number of arguments.
 */
static ValueT
ngx_scheme_say(VM* vm, ValueT* rest)
{
  ValueT ctx = scm_registry_get(vm, NGX_SCHEME_CTX_KEY);
  if (!scm_is_lightuserdata(vm, &ctx)) {
    throw "no request context";
  }
  ngx_scheme_vm_s *h = (ngx_scheme_vm_s *) scm_to_lightuserdata(vm, &ctx);

  while (!scm_is_null(vm, rest)) {
    ValueT *p = scm_car(vm, rest);
    if (scm_is_string(vm, p)) {
      int n = 0;
      const char *s = scm_get_string(vm, p, &n);
      ngx_scheme_append(h, s, n);
    }
    else {
      int n = 0;
      const char *s = scm_tostring(vm, p, &n);
      ngx_scheme_append(h, s, n);
    }
    rest = scm_cdr(vm, rest);
  }

  return_void();
}

/* library table, same style as the builtin ones (see SCMMath::init) */
static const RegCProc ngx_scheme_lib[] = {
  RegCProc("ngx-say", ngx_scheme_say, true),  /* variadic */
  RegCProc(NULL, -1)
};

ngx_scheme_vm_t *
ngx_scheme_create(void)
{
    ngx_scheme_vm_t *h = (ngx_scheme_vm_t *) malloc(sizeof(*h));
    if (h == NULL) {
        return NULL;
    }

    h->vm = NULL;
  h->buf = NULL;
  h->len = 0;
  h->cap = 0;

    try {
        h->vm = scm_create_vm();
        if (h->vm == NULL) {
            free(h);
            return NULL;
        }

    /* register this handle as a lightuserdata in the VM registry, like
        lua-nginx-module's request context in the Lua registry */
    scm_registry_set(h->vm, NGX_SCHEME_CTX_KEY,
        scm_make_lightuserdata(h->vm, h));
    scm_register_lib(h->vm, ngx_scheme_lib);
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
          is reported to the caller, which logs it (this bridge never logs) */
      if (err_cap > 0) {
        snprintf(err, err_cap, "scheme teardown error: %s",
            e != NULL ? e : "unknown");
      }
      free(h->buf);
      free(h);
      return -1;
    }
        catch (...) {
      if (err_cap > 0) {
        snprintf(err, err_cap, "scheme teardown error: unknown exception");
      }
      free(h->buf);
      free(h);
      return -1;
        }
    }

  free(h->buf);
    free(h);
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
    *data = h->buf;
    }
    if (len != NULL) {
    *len = h->len;
    }
}
