/*
 * ngx_scheme_runtime.cpp -- C bridge over the dummyscheme VM.
 *
 * Owns one VM per request.  The default output port of the VM is replaced
 * by an OutputPortStrObj so that everything the scheme code prints via
 * `display` / `write` / `newline` is accumulated into an in-memory buffer
 * instead of going to stderr, and can be handed back to nginx as the
 * response body.
 *
 * The default output port normally points at stderr (see GlobalVM::init
 * in vm.cpp); we swap in a string port right after VM creation.
 */
#include "vm.h"
#include "ngx_scheme_runtime.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>
#include <string.h>

using namespace Scheme;

struct ngx_scheme_vm_s {
    VM                *vm;     /* the interpreter */
    OutputPortStrObj  *oport;  /* string output port; rooted via G(vm)->oport */
};

ngx_scheme_vm_t *
ngx_scheme_create(void)
{
    ngx_scheme_vm_t *h = (ngx_scheme_vm_t *) malloc(sizeof(*h));
    if (h == NULL) {
        return NULL;
    }

    h->vm = NULL;
    h->oport = NULL;

    try {
        h->vm = scm_create_vm();
        if (h->vm == NULL) {
            free(h);
            return NULL;
        }

        /* redirect display/write output into a string buffer */
        h->oport = Sr1(h->vm, OutputPortStrObj, h->vm);
        G(h->vm)->oport = h->oport;
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

void
ngx_scheme_destroy(ngx_scheme_vm_t *h)
{
    if (h == NULL) {
        return;
    }

    if (h->vm != NULL) {
        try {
            scm_destroy_vm(h->vm);
        }
        catch (...) {
            /* teardown must not throw out of the request path */
        }
    }

    free(h);
}

int
ngx_scheme_eval(ngx_scheme_vm_t *h, const char *code, int len,
                char *err, int err_cap)
{
    char *src;
    ValueT res;

    if (h == NULL || h->vm == NULL) {
        return -1;
    }

    if (err_cap > 0) {
        err[0] = '\0';
    }

    /* evalstr() needs a NUL-terminated source string */
    src = (char *) malloc((size_t) len + 1);
    if (src == NULL) {
        if (err_cap > 0) {
            snprintf(err, err_cap, "out of memory");
        }
        return -1;
    }
    memcpy(src, code, (size_t) len);
    src[len] = '\0';

    h->oport->strbuf.reset();

    try {
        res = h->vm->evalstr(src);

        /* a string as the value of the last expression is appended too */
        if (isstr(&res)) {
            StrPtr sp = strref(&res);
            h->oport->writestr(Ssstr(sp), Sslen(sp));
        }
    }
    catch (const char *e) {
        if (err_cap > 0) {
            snprintf(err, err_cap, "scheme error: %s",
                     e != NULL ? e : "unknown");
        }
        free(src);
        return -1;
    }
    catch (...) {
        if (err_cap > 0) {
            snprintf(err, err_cap, "scheme error: unknown exception");
        }
        free(src);
        return -1;
    }

    free(src);
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

    if (h == NULL || h->oport == NULL) {
        return;
    }

    if (data != NULL) {
        *data = h->oport->strbuf.buf;
    }
    if (len != NULL) {
        *len = h->oport->strbuf.count;
    }
}
