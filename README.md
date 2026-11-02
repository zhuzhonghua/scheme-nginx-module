# ngx_http_scheme_module

Run Scheme inside nginx like lua-nginx-module.  The module embeds the [dummyscheme](https://github.com/zhuzhonghua/dummyscheme)
and exposes `content_by_scheme`, `access_by_scheme` and `log_by_scheme`
directives that evaluate inline Scheme source in nginx's content, access and
log phases.  The content script sends the response body via the host
function `ngx-say`.
A second host function, `ngx-log`, writes log messages to nginx's error log;
`ngx-status` / `ngx-exit` control the response status,
`ngx-header-ref` / `ngx-header-set!` read and write response headers, and
`ngx-var-ref` reads nginx variables such as `remote_addr`.

The module follows the classic nginx content-module layout (module struct +
commands array + create/merge loc_conf + content handler).  It is a generic
third-party nginx module; the host scheme interpreter is a separate project
and is linked in as a dependency.

Three directives are provided: `content_by_scheme` (content phase),
`access_by_scheme` (access phase, for auth) and `log_by_scheme` (log phase,
for request logging).  All three take inline Scheme source and all are
compiled once per location.

### Build

```sh
SCHEME_CORE_DIR=/path/to/dummy \
    ./configure --add-module=/path/to/nginx-module
make
```

## How To Use

The module adds a `content_by_scheme` directive, valid inside
`location` blocks.  It takes a single inline Scheme expression as an
argument and is used as the content handler of that location.  The scheme
code produces the body by calling `(ngx-say ...)`, which converts each
argument to a string (write semantics) and appends it to the response:

```nginx
server {
    listen 8080;

    location /hello {
        content_by_scheme '(ngx-say "hello " (+ 20 22))';
    }

    location /sum {
        content_by_scheme '(ngx-say "sum=" (+ 40 2))';
    }

    location /fib {
        content_by_scheme '(ngx-say "fib(24)="
                                    (let fib ((n 24))
                                      (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))';
    }
}
```

```sh
curl http://127.0.0.1:8080/hello   # -> hello 42
curl http://127.0.0.1:8080/sum     # -> sum=42
curl http://127.0.0.1:8080/fib     # -> fib(24)=46368
```

Note that `display` / `write` go to the VM's default output (stderr), not
into the body; use `ngx-say` for response output.

## access_by_scheme

**syntax:** `access_by_scheme '<scheme>';`

Runs the scheme source in nginx's access phase, before the content handler.
It is meant for authentication / authorization and is normally used together
with a content handler or a static file in the same location:

```nginx
location /private {
    access_by_scheme '(if (equal? (ngx-var-ref "arg_token") "ok")
                          #f
                          (ngx-exit ngx-http-unauthorized))';
    content_by_scheme '(ngx-say "secret")';
}
```

```sh
curl -i 'http://127.0.0.1:8080/private?token=ok'  # -> 200 secret
curl -i 'http://127.0.0.1:8080/private'           # -> 401 Unauthorized
```

An `(ngx-exit N)` with `N >= 300` ends the request with that status (nginx's
error page and `error_page` overrides apply) and the content handler does not
run.  `(ngx-exit 0)` and 2xx statuses just finish the access script and let
the request continue.  A scheme error becomes 500.  Response headers set with
`ngx-header-set!` (for example a redirect `Location`) are kept.

Body written with `ngx-say` is discarded, because the access phase produces
no response body.  Like `content_by_scheme`, the source is compiled once per
location and reused across requests; the environment is separate from the
content script's, so values should be passed through `ngx-var-ref` or
request headers rather than top-level variables.

## log_by_scheme

**syntax:** `log_by_scheme '<scheme>';`

Runs the scheme source in nginx's log phase, after the response has been
sent.  It is meant for request logging and cannot change the response:

```nginx
location /logged {
    content_by_scheme '(ngx-say "ok")';
    log_by_scheme '(ngx-log ngx-info "status=" (ngx-var-ref "status")
                             " uri=" (ngx-var-ref "request_uri")
                             " bytes=" (ngx-var-ref "bytes_sent"))';
}
```

```sh
curl http://127.0.0.1:8080/logged   # -> ok
tail -n 1 /path/to/error.log        # ... [scheme]: status=200 uri=/logged bytes=2
```

At this point request/response variables such as `status`, `request_time`,
`bytes_sent` and `sent_http_*` have their final values, so a log line can be
assembled entirely in Scheme.  A scheme error is written to error_log and
otherwise ignored (the response has already been sent); `(ngx-exit N)` only
stops the script, and `ngx-say` output is discarded.

## ngx-log

**syntax:** `(ngx-log log_level message ...)`

Concatenates the message arguments and writes them to error.log at the
given log level.  Message arguments are converted with the same semantics
as `ngx-say` (strings verbatim, anything else via write semantics).

`log_level` is one of the log-level constants:

```scheme
ngx-stderr   ; NGX_LOG_STDERR (0)
ngx-emerg    ; NGX_LOG_EMERG  (1)
ngx-alert    ; NGX_LOG_ALERT  (2)
ngx-crit     ; NGX_LOG_CRIT   (3)
ngx-err      ; NGX_LOG_ERR    (4)
ngx-warn     ; NGX_LOG_WARN   (5)
ngx-notice   ; NGX_LOG_NOTICE (6)
ngx-info     ; NGX_LOG_INFO   (7)
ngx-debug    ; NGX_LOG_DEBUG  (8)
```

or a raw integer matching the `NGX_LOG_*` values above.  These constants
are usually used by the `ngx-log` method.

Messages at a level below the one configured by the `error_log` directive
are dropped, exactly like nginx's own logging.  Log messages are subject to
nginx's `NGX_MAX_ERROR_STR` (2048 bytes) truncation.

```nginx
server {
    listen 8080;

    location /log {
        content_by_scheme '(ngx-log ngx-err "request logged, 20+22=" (+ 20 22))
                           (ngx-say "log request ok")';
    }
}
```

```sh
curl http://127.0.0.1:8080/log     # -> log request ok
tail -n 1 /path/to/error.log       # -> ... [error] ... [scheme]: request logged, 20+22=42
```

## ngx-status

**syntax:** `ngx-status` — read the current response status (0 when untouched)

**syntax:** `(set! ngx-status status)` — set the response status and keep running

`ngx-status` is a virtual global variable backed by the request: read it like
any other global, and write it with `set!`.  Assigning applies the value to
the response status line immediately; it does not stop the script, and the
body accumulated via `ngx-say` is sent with it.  `status` must be an integer
in 100..999 (the status line is 3 digits per RFC 7230); numeric strings are
coerced.  A value outside the range is logged to error_log and ignored; a
non-numeric value raises a scheme error.

```nginx
location /created {
    content_by_scheme '(set! ngx-status ngx-http-created)
                       (ngx-say "created, status=" ngx-status)';
}
```

```sh
curl -i http://127.0.0.1:8080/created   # -> HTTP/1.1 201 Created
                                        #    created, status=201
```

## ngx-header

**syntax:** `(ngx-header-ref name)` — read a response header (`#f` when unset)

**syntax:** `(ngx-header-set! name value)` — set, replace or remove a response header

Names are strings or symbols and are matched case-insensitively; `_` is
treated as `-`, so `"X_Custom"` and `"x-custom"` address the same header.
Setting a name that already exists replaces its value in place, so a header
is never duplicated.  `value` is converted with the same semantics as
`ngx-say` (strings verbatim, other values via write semantics); `#f` removes
the header instead of setting it.

`Content-Type` and `Content-Length` are written to the request fields nginx
emits them from, so they take precedence over the module defaults
(`text/plain` and the accumulated body length).  `Location` also updates
nginx's redirect bookkeeping, so it works together with `ngx-exit`:

```nginx
location /json {
    content_by_scheme '(ngx-header-set! "Content-Type" "application/json")
                       (ngx-say "{}")';
}

location /go {
    content_by_scheme '(ngx-header-set! "Location" "/hello")
                       (ngx-exit ngx-http-moved-temporarily)';
}
```

```sh
curl -i http://127.0.0.1:8080/json   # -> Content-Type: application/json
curl -i http://127.0.0.1:8080/go     # -> HTTP/1.1 302, Location: http://.../hello
```

A header that has multiple values reads back as its first value.  The
module's default `Content-Type` is applied after the script finishes, so a
script that did not set it reads `#f` while it runs.

## ngx-var

**syntax:** `(ngx-var-ref name)` — read an nginx variable (`#f` when not found)

`name` is a string or a symbol; it is lowercased before lookup, so
`"REQUEST_METHOD"` and `'request_method` are equivalent.  Underscores are
kept (this is not the header-name rule), so all of nginx's usual variables
work:

```scheme
(ngx-var-ref "request_method")   ; GET
(ngx-var-ref "request_uri")      ; /path?query
(ngx-var-ref "remote_addr")      ; 203.0.113.7
(ngx-var-ref "arg_q")            ; ?q=... value
(ngx-var-ref "http_user_agent")  ; request header User-Agent
(ngx-var-ref "cookie_sid")       ; request cookie sid
(ngx-var-ref "sent_http_content_type")  ; response header
```

The value is copied into a scheme string, so it remains valid after the
request ends.  A variable that does not exist, has no value yet, or is not
valid at this point of the request reads back as `#f`.  A leading `$` is not
accepted: write `"remote_addr"`, not `"$remote_addr"`.

```nginx
location /info {
    content_by_scheme '(ngx-say (ngx-var-ref "request_method") " "
                                (ngx-var-ref "request_uri") " from "
                                (ngx-var-ref "remote_addr"))';
}
```

```sh
curl http://127.0.0.1:8080/info   # -> GET /info from 127.0.0.1
```

Only reading is supported for now; assigning nginx variables needs an
indexed variable configured at startup plus its validator, and is a separate
step.

## ngx-exit

**syntax:** `(ngx-exit status)`

Finishes the request immediately: the rest of the scheme code does **not**
run.  For `status >= 300`, nginx handles the request as an error/redirect
(the body accumulated via `ngx-say` is discarded and nginx's standard error
page, including any `error_page` override, is used).  For `status` in
200..299 the accumulated body is still sent with that status, and
`(ngx-exit 0)` stops the script while keeping the default 200 status and the
body collected so far.

```nginx
location /private {
    content_by_scheme '(if (not (authorized?))
                           (ngx-exit ngx-http-unauthorized))
                       (ngx-say "secret")';
}
```

```sh
curl -i http://127.0.0.1:8080/private   # -> HTTP/1.1 401 Unauthorized
```

## HTTP status constants

The common `NGX_HTTP_*` codes are predefined as top-level constants, named
`ngx-http-*` (dashes instead of underscores), for use with `ngx-exit` and
assignments to `ngx-status`:

```scheme
ngx-http-ok                      ; 200      ngx-http-no-content              ; 204
ngx-http-created                 ; 201      ngx-http-moved-permanently       ; 301
ngx-http-moved-temporarily       ; 302      ngx-http-not-modified            ; 304
ngx-http-bad-request             ; 400      ngx-http-unauthorized            ; 401
ngx-http-forbidden               ; 403      ngx-http-not-found               ; 404
ngx-http-not-allowed             ; 405      ngx-http-too-many-requests       ; 429
ngx-http-internal-server-error   ; 500      ngx-http-bad-gateway             ; 502
ngx-http-service-unavailable     ; 503      ngx-http-gateway-time-out        ; 504
```

The full list is in `src/ngx_scheme_runtime.cpp` (`ngx_scheme_inject_status_consts`).
