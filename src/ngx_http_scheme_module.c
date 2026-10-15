/*
 * ngx_http_scheme_module -- run Scheme (the dummyscheme VM, see vm.h of the
 * host project) inside nginx.
 *
 * A generic NGX_HTTP_MODULE with location-scoped config:
 *   - one directive, `content_by_scheme`, taking the scheme source inline
 *   - the content handler creates a per-request VM (see ngx_scheme_runtime)
 *   - the classic content-module layout (module struct + commands array +
 *     create/merge loc_conf + content handler)
 *
 * Example:
 *
 *   location /hello {
 *       content_by_scheme '(ngx-say "hello " (+ 20 22))';
 *   }
 *   # GET /hello -> "hello 42"
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_scheme_runtime.h"

typedef struct {
    ngx_str_t   code;      /* inline scheme source */
    ngx_flag_t  enabled;
} ngx_http_scheme_loc_conf_t;

static ngx_int_t ngx_http_scheme_handler(ngx_http_request_t *r);

static void *ngx_http_scheme_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_scheme_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_scheme_content_by_scheme(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static ngx_command_t  ngx_http_scheme_commands[] = {

    { ngx_string("content_by_scheme"),
      NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_http_scheme_content_by_scheme,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_scheme_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_scheme_create_loc_conf,       /* create location configuration */
    ngx_http_scheme_merge_loc_conf         /* merge location configuration */
};

ngx_module_t  ngx_http_scheme_module = {
    NGX_MODULE_V1,
    &ngx_http_scheme_module_ctx,           /* module context */
    ngx_http_scheme_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_http_scheme_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_scheme_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_scheme_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    ngx_str_null(&conf->code);
    conf->enabled = NGX_CONF_UNSET;

    return conf;
}

static char *
ngx_http_scheme_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_scheme_loc_conf_t  *prev = parent;
    ngx_http_scheme_loc_conf_t  *conf = child;

    ngx_conf_merge_str_value(conf->code, prev->code, "");
    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

    return NGX_CONF_OK;
}

static char *
ngx_http_scheme_content_by_scheme(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_core_loc_conf_t   *clcf;
    ngx_http_scheme_loc_conf_t *slcf = conf;
    ngx_str_t                  *value;

    if (slcf->code.len != 0) {
        return "is duplicate";
    }

    value = cf->args->elts;

    slcf->code = value[1];
    slcf->enabled = 1;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_scheme_handler;

    return NGX_CONF_OK;
}

/*
 * Destroy the per-request VM and log teardown failures here, in the nginx
 * layer: the C++ bridge never logs (it has no nginx dependency), it only
 * reports errors through its return code and error buffer.
 */
static void
ngx_http_scheme_destroy(ngx_scheme_vm_t *svm, ngx_http_request_t *r)
{
  char err[128];

  if (ngx_scheme_destroy(svm, err, sizeof(err)) != 0) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "scheme: %s", err);
  }
}

static ngx_int_t
ngx_http_scheme_handler(ngx_http_request_t *r)
{
    ngx_http_scheme_loc_conf_t  *slcf;
    ngx_scheme_vm_t             *svm;
    ngx_buf_t                   *b;
    ngx_chain_t                 *cl;
    const char                  *body;
    int                          body_len;
    char                         err[256];
    ngx_int_t                    rc;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_scheme_module);

    if (!slcf->enabled) {
        return NGX_DECLINED;
    }

    svm = ngx_scheme_create();
    if (svm == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "scheme: failed to create VM");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_scheme_eval(svm, (const char *) slcf->code.data, slcf->code.len,
                        err, sizeof(err)) != 0)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "scheme: %s", err);
    ngx_http_scheme_destroy(svm, r);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_scheme_output(svm, &body, &body_len);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "scheme: body %d bytes", body_len);

    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = body_len;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
    ngx_http_scheme_destroy(svm, r);
        return rc;
    }

    b = ngx_create_temp_buf(r->pool, (size_t) body_len);
    if (b == NULL) {
    ngx_http_scheme_destroy(svm, r);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    if (body_len > 0) {
        b->last = ngx_cpymem(b->pos, body, (size_t) body_len);
    }

    /*
     * this is the final buffer of the response: mark it so the write
     * filter sends immediately, otherwise postpone_output buffers the
     * chain and the client never receives anything
     */
    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
    ngx_http_scheme_destroy(svm, r);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

  ngx_http_scheme_destroy(svm, r);

    return ngx_http_output_filter(r, cl);
}
