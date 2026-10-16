# ngx_http_scheme_module

Run Scheme inside nginx like lua-nginx-module.  The module embeds the [dummyscheme](https://github.com/zhuzhonghua/dummyscheme)
and exposes a `content_by_scheme` directive that evaluates
inline Scheme source and sends the response body via the host function `ngx-say`.
A second host function, `ngx-log`, writes log messages to nginx's error log.

The module follows the classic nginx content-module layout (module struct +
commands array + create/merge loc_conf + content handler).  It is a generic
third-party nginx module; the host scheme interpreter is a separate project
and is linked in as a dependency.

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
