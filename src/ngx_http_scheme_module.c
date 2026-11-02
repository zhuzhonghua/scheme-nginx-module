/*
 * ngx_http_scheme_module -- run Scheme (the dummyscheme VM, see vm.h of the
 * host project) inside nginx.
 *
 * A generic NGX_HTTP_MODULE with location-scoped config:
 *   - two directives taking scheme source inline:
 *       `content_by_scheme` -- the response content handler
 *       `access_by_scheme`  -- an access-phase handler for auth / filtering
 *   - both handlers create a per-request VM (see ngx_scheme_runtime)
 *   - the classic content-module layout (module struct + commands array +
 *     create/merge loc_conf + content handler)
 *
 * Example:
 *
 *   location /hello {
 *       content_by_scheme '(ngx-say "hello " (+ 20 22))';
 *   }
 *   # GET /hello -> "hello 42"
 *
 *   location /private {
 *       access_by_scheme '(if (not (equal? (ngx-var-ref "arg_token") "ok"))
 *                              (ngx-exit ngx-http-unauthorized))';
 *       content_by_scheme '(ngx-say "secret")';
 *   }
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_scheme_runtime.h"

typedef struct {
  ngx_str_t   code;          /* inline content-phase scheme source */
  ngx_flag_t  enabled;

  ngx_str_t   access_code;   /* inline access-phase scheme source */
  ngx_flag_t  access_enabled;
} ngx_http_scheme_loc_conf_t;

static ngx_int_t ngx_http_scheme_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_scheme_access_handler(ngx_http_request_t *r);

static ngx_int_t ngx_http_scheme_init(ngx_conf_t *cf);
static void *ngx_http_scheme_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_scheme_merge_loc_conf(ngx_conf_t *cf, void *parent,
  void *child);
static char *ngx_http_scheme_content_by_scheme(ngx_conf_t *cf,
  ngx_command_t *cmd, void *conf);
static char *ngx_http_scheme_access_by_scheme(ngx_conf_t *cf,
  ngx_command_t *cmd, void *conf);

static ngx_command_t  ngx_http_scheme_commands[] = {

  { ngx_string("content_by_scheme"),
    NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
    ngx_http_scheme_content_by_scheme,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL },

  { ngx_string("access_by_scheme"),
    NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
    ngx_http_scheme_access_by_scheme,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL },

  ngx_null_command
};

static ngx_http_module_t  ngx_http_scheme_module_ctx = {
  NULL,                                  /* preconfiguration */
  ngx_http_scheme_init,                  /* postconfiguration */

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

  ngx_str_null(&conf->access_code);
  conf->access_enabled = NGX_CONF_UNSET;

  return conf;
}

static char *
ngx_http_scheme_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
  ngx_http_scheme_loc_conf_t  *prev = parent;
  ngx_http_scheme_loc_conf_t  *conf = child;

  ngx_conf_merge_str_value(conf->code, prev->code, "");
  ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

  ngx_conf_merge_str_value(conf->access_code, prev->access_code, "");
  ngx_conf_merge_value(conf->access_enabled, prev->access_enabled, 0);

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

static char *
ngx_http_scheme_access_by_scheme(ngx_conf_t *cf, ngx_command_t *cmd,
  void *conf)
{
  ngx_http_scheme_loc_conf_t *slcf = conf;
  ngx_str_t                  *value;

  if (slcf->access_code.len != 0) {
    return "is duplicate";
  }

  value = cf->args->elts;

  slcf->access_code = value[1];
  slcf->access_enabled = 1;

  return NGX_CONF_OK;
}

/*
 * Register the access-phase handler.  It is appended to the access phase
 * like any other access module; locations without access_by_scheme return
 * NGX_DECLINED so the rest of the chain keeps running.
 */
static ngx_int_t
ngx_http_scheme_init(ngx_conf_t *cf)
{
  ngx_http_handler_pt        *h;
  ngx_http_core_main_conf_t  *cmcf;

  cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

  h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
  if (h == NULL) {
    return NGX_ERROR;
  }

  *h = ngx_http_scheme_access_handler;

  return NGX_OK;
}

/*
 * Destroy the per-request VM and log teardown failures here, in the nginx
 * layer: the bridge reports teardown errors through its return code and
 * error buffer.
 */
static void
ngx_http_scheme_destroy(ngx_scheme_vm_t *svm, ngx_http_request_t *r)
{
  char err[128];

  if (ngx_scheme_destroy(svm, err, sizeof(err)) != 0) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[scheme]: %s", err);
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

  svm = ngx_scheme_create(r);
  if (svm == NULL) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "[scheme]: failed to create VM");
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  rc = ngx_scheme_eval(svm, (const char *) slcf->code.data, slcf->code.len,
          err, sizeof(err));
  if (rc == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[scheme]: %s", err);
    ngx_http_scheme_destroy(svm, r);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "[scheme]: eval rc %i", (int) rc);

  if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
    /*
     * (ngx-exit N) with N >= 300: hand the request back to nginx so its
     * standard error page (and error_page overrides) applies.  The body
     * accumulated via ngx-say is discarded in favour of nginx's standard
     * error page.
     */
    ngx_http_scheme_destroy(svm, r);
    return rc;
  }

  ngx_scheme_output(svm, &body, &body_len);

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "[scheme]: body %d bytes", body_len);

  /* a Content-Type / Content-Length set by the scheme code wins; fill in the
     module defaults only when it left them untouched */
  if (r->headers_out.content_type.len == 0) {
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_len = sizeof("text/plain") - 1;
  }
  if (r->headers_out.status == 0) {
    r->headers_out.status = NGX_HTTP_OK;
  }
  if (body_len == 0 || r->headers_out.content_length_n < 0) {
    r->headers_out.content_length_n = body_len;
  }

  if (body_len == 0) {
    /*
     * empty body (e.g. bare (ngx-say)): mark the response header-only so
     * no zero-size buf reaches the write filter -- nginx treats a buf
     * with pos == last as an error ("zero size buf in writer").
     */
    r->header_only = 1;
    ngx_http_scheme_destroy(svm, r);
    return ngx_http_send_header(r);
  }

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
  b->last = ngx_cpymem(b->pos, body, (size_t) body_len);

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

/*
 * Access-phase handler: runs access_by_scheme when the location configures
 * it.  A (ngx-exit N) with N >= 300 ends the request with that status (the
 * access phase finalizes it, error_page included); 0 and 2xx just finish the
 * script and let the request continue to the later phases.  A scheme error
 * becomes 500.  Body written with ngx-say is discarded: this phase has no
 * response body.
 */
static ngx_int_t
ngx_http_scheme_access_handler(ngx_http_request_t *r)
{
  ngx_http_scheme_loc_conf_t  *slcf;
  ngx_scheme_vm_t             *svm;
  char                         err[256];
  ngx_int_t                    rc;

  slcf = ngx_http_get_module_loc_conf(r, ngx_http_scheme_module);

  if (!slcf->access_enabled) {
    return NGX_DECLINED;
  }

  svm = ngx_scheme_create(r);
  if (svm == NULL) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "[scheme]: failed to create VM");
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  rc = ngx_scheme_eval(svm, (const char *) slcf->access_code.data,
          slcf->access_code.len, err, sizeof(err));

  if (rc == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[scheme]: %s", err);
    ngx_http_scheme_destroy(svm, r);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "[scheme]: access eval rc %i", (int) rc);

  ngx_http_scheme_destroy(svm, r);

  if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
    return rc;
  }

  return NGX_OK;
}
