/*
 * ngx_http_scheme_module.cpp -- run Scheme (the dummyscheme VM, see vm.h of
 * the host project) inside nginx.
 *
 * A generic NGX_HTTP_MODULE with location-scoped config:
 *   - three directives taking scheme source inline:
 *       `content_by_scheme` -- the response content handler
 *       `access_by_scheme`  -- an access-phase handler for auth / filtering
 *       `log_by_scheme`     -- a log-phase handler for request logging
 *   - each handler creates a short-lived per-request handle over the
 *     worker's shared VM and destroys it before returning
 *     (see ngx_scheme_runtime.h)
 *   - the classic content-module layout (module struct + commands array +
 *     create/merge loc_conf + handlers) is kept as file-scope statics; only
 *     `ngx_http_scheme_module` itself has external linkage because nginx's
 *     generated module list references that symbol by name.
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
 *
 *   location /logged {
 *       content_by_scheme '(ngx-say "ok")';
 *       log_by_scheme '(ngx-log ngx-info "status=" (ngx-var-ref "status"))';
 *   }
 */

/* nginx headers are C: keep their declarations unmangled so the objects
 * produced from nginx's own C sources link against this C++ translation
 * unit. */
extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

#include "ngx_scheme_runtime.h"

typedef struct {
  ngx_str_t   code;          /* inline content-phase scheme source */
  ngx_flag_t  enabled;

  ngx_str_t   access_code;   /* inline access-phase scheme source */
  ngx_flag_t  access_enabled;

  ngx_str_t   log_code;      /* inline log-phase scheme source */
  ngx_flag_t  log_enabled;
} ngx_http_scheme_loc_conf_t;

static ngx_int_t ngx_http_scheme_content_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_scheme_access_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_scheme_log_handler(ngx_http_request_t *r);

static ngx_int_t ngx_http_scheme_init(ngx_conf_t *cf);
static void *ngx_http_scheme_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_scheme_merge_loc_conf(ngx_conf_t *cf, void *parent,
  void *child);
static char *ngx_http_scheme_content_by_scheme(ngx_conf_t *cf,
  ngx_command_t *cmd, void *conf);
static char *ngx_http_scheme_access_by_scheme(ngx_conf_t *cf,
  ngx_command_t *cmd, void *conf);
static char *ngx_http_scheme_log_by_scheme(ngx_conf_t *cf,
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

  { ngx_string("log_by_scheme"),
    NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
    ngx_http_scheme_log_by_scheme,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL },

  ngx_null_command
};

static ngx_http_module_t  ngx_http_scheme_module_ctx = {
  NULL,                                  /* preconfiguration */
  ngx_http_scheme_init, /* postconfiguration */

  NULL,                                  /* create main configuration */
  NULL,                                  /* init main configuration */

  NULL,                                  /* create server configuration */
  NULL,                                  /* merge server configuration */

  ngx_http_scheme_create_loc_conf,
                                         /* create location configuration */
  ngx_http_scheme_merge_loc_conf
                                         /* merge location configuration */
};

/* nginx looks this symbol up from its generated module list, so it keeps C
 * linkage even though the implementation is C++. */
extern "C" ngx_module_t  ngx_http_scheme_module = {
  NGX_MODULE_V1,
  &ngx_http_scheme_module_ctx,
                                         /* module context */
  ngx_http_scheme_commands,
                                         /* module directives */
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

void *
ngx_http_scheme_create_loc_conf(ngx_conf_t *cf)
{
  ngx_http_scheme_loc_conf_t  *conf;

  conf = (ngx_http_scheme_loc_conf_t *)
      ngx_pcalloc(cf->pool, sizeof(ngx_http_scheme_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  ngx_str_null(&conf->code);
  conf->enabled = NGX_CONF_UNSET;

  ngx_str_null(&conf->access_code);
  conf->access_enabled = NGX_CONF_UNSET;

  ngx_str_null(&conf->log_code);
  conf->log_enabled = NGX_CONF_UNSET;

  return conf;
}

char *
ngx_http_scheme_merge_loc_conf(ngx_conf_t *cf, void *parent,
  void *child)
{
  ngx_http_scheme_loc_conf_t  *prev = (ngx_http_scheme_loc_conf_t *) parent;
  ngx_http_scheme_loc_conf_t  *conf = (ngx_http_scheme_loc_conf_t *) child;

  ngx_conf_merge_str_value(conf->code, prev->code, "");
  ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

  ngx_conf_merge_str_value(conf->access_code, prev->access_code, "");
  ngx_conf_merge_value(conf->access_enabled, prev->access_enabled, 0);

  ngx_conf_merge_str_value(conf->log_code, prev->log_code, "");
  ngx_conf_merge_value(conf->log_enabled, prev->log_enabled, 0);

  return NGX_CONF_OK;
}

char *
ngx_http_scheme_content_by_scheme(ngx_conf_t *cf,
  ngx_command_t *cmd, void *conf)
{
  ngx_http_core_loc_conf_t   *clcf;
  ngx_http_scheme_loc_conf_t *slcf = (ngx_http_scheme_loc_conf_t *) conf;
  ngx_str_t                  *value;

  if (slcf->code.len != 0) {
    return "is duplicate";
  }

  value = (ngx_str_t *) cf->args->elts;

  slcf->code = value[1];
  slcf->enabled = 1;

  clcf = (ngx_http_core_loc_conf_t *)
      ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
  clcf->handler = ngx_http_scheme_content_handler;

  return NGX_CONF_OK;
}

char *
ngx_http_scheme_access_by_scheme(ngx_conf_t *cf,
  ngx_command_t *cmd, void *conf)
{
  ngx_http_scheme_loc_conf_t *slcf = (ngx_http_scheme_loc_conf_t *) conf;
  ngx_str_t                  *value;

  if (slcf->access_code.len != 0) {
    return "is duplicate";
  }

  value = (ngx_str_t *) cf->args->elts;

  slcf->access_code = value[1];
  slcf->access_enabled = 1;

  return NGX_CONF_OK;
}

char *
ngx_http_scheme_log_by_scheme(ngx_conf_t *cf, ngx_command_t *cmd,
  void *conf)
{
  ngx_http_scheme_loc_conf_t *slcf = (ngx_http_scheme_loc_conf_t *) conf;
  ngx_str_t                  *value;

  if (slcf->log_code.len != 0) {
    return "is duplicate";
  }

  value = (ngx_str_t *) cf->args->elts;

  slcf->log_code = value[1];
  slcf->log_enabled = 1;

  return NGX_CONF_OK;
}

/*
 * Register the access- and log-phase handlers.  They are appended to their
 * phases like any other module; locations without the matching directive
 * leave the chain alone.
 */
ngx_int_t
ngx_http_scheme_init(ngx_conf_t *cf)
{
  ngx_http_handler_pt        *h;
  ngx_http_core_main_conf_t  *cmcf;

  cmcf = (ngx_http_core_main_conf_t *)
      ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

  h = (ngx_http_handler_pt *)
      ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
  if (h == NULL) {
    return NGX_ERROR;
  }
  *h = ngx_http_scheme_access_handler;

  h = (ngx_http_handler_pt *)
      ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
  if (h == NULL) {
    return NGX_ERROR;
  }
  *h = ngx_http_scheme_log_handler;

  return NGX_OK;
}

/*
 * Content-phase handler: runs content_by_scheme for the request body,
 * then hands the output buffer to nginx's write chain.  Handles with
 * fewer boilerplate: a single NgxSchemeVMHandle tears the per-request
 * context and VM registry slot down on every exit path.
 */
ngx_int_t
ngx_http_scheme_content_handler(ngx_http_request_t *r)
{
  ngx_http_scheme_loc_conf_t  *slcf;
  ngx_buf_t                   *b;
  ngx_chain_t                 *cl;
  const char                  *body;
  int                          body_len;
  char                         err[256];
  ngx_int_t                    rc;

  slcf = (ngx_http_scheme_loc_conf_t *)
      ngx_http_get_module_loc_conf(r, ngx_http_scheme_module);

  if (!slcf->enabled) {
    return NGX_DECLINED;
  }

  NgxSchemeVMHandle handle(r);
  if (!handle.ok()) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "[scheme]: failed to create VM");
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  rc = handle.eval((const char *) slcf->code.data, slcf->code.len,
      err, sizeof(err));
  if (rc == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[scheme]: %s", err);
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
    return rc;
  }

  handle.output(&body, &body_len);

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

    return ngx_http_send_header(r);
  }

  rc = ngx_http_send_header(r);
  if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {

    return rc;
  }

  b = ngx_create_temp_buf(r->pool, (size_t) body_len);
  if (b == NULL) {

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

    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  cl->buf = b;
  cl->next = NULL;

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
ngx_int_t
ngx_http_scheme_access_handler(ngx_http_request_t *r)
{
  ngx_http_scheme_loc_conf_t  *slcf;
  char                         err[256];
  ngx_int_t                    rc;

  slcf = (ngx_http_scheme_loc_conf_t *)
      ngx_http_get_module_loc_conf(r, ngx_http_scheme_module);

  if (!slcf->access_enabled) {
    return NGX_DECLINED;
  }

  NgxSchemeVMHandle handle(r);
  if (!handle.ok()) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "[scheme]: failed to create VM");
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  rc = handle.eval((const char *) slcf->access_code.data, slcf->access_code.len,
      err, sizeof(err));

  if (rc == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[scheme]: %s", err);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "[scheme]: access eval rc %i", (int) rc);

  if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
    return rc;
  }

  return NGX_OK;
}

/*
 * Log-phase handler: runs log_by_scheme after the response has been sent.
 * It cannot change the response, so every outcome returns NGX_OK; scheme
 * errors are written to error_log and otherwise ignored.  (ngx-exit N) only
 * stops the script.  Body written with ngx-say is discarded.
 */
ngx_int_t
ngx_http_scheme_log_handler(ngx_http_request_t *r)
{
  ngx_http_scheme_loc_conf_t  *slcf;
  char                         err[256];
  ngx_int_t                    rc;

  slcf = (ngx_http_scheme_loc_conf_t *)
      ngx_http_get_module_loc_conf(r, ngx_http_scheme_module);

  if (!slcf->log_enabled) {
    return NGX_OK;
  }

  NgxSchemeVMHandle handle(r);
  if (!handle.ok()) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "[scheme]: failed to create VM");
    return NGX_OK;
  }

  rc = handle.eval((const char *) slcf->log_code.data, slcf->log_code.len,
      err, sizeof(err));

  if (rc == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "[scheme]: %s", err);
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "[scheme]: log eval rc %i", (int) rc);

  return NGX_OK;
}
