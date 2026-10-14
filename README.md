# ngx_http_scheme_module

Run Scheme inside nginx like lua-nginx-module.  The module embeds the [dummyscheme](https://github.com/zhuzhonghua/dummyscheme)
and exposes a `content_by_scheme` directive that evaluates
inline Scheme source and sends the `display` output as the response body.

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

The module adds one directive, `content_by_scheme`, valid inside
`location` blocks.  It takes a single inline Scheme expression as an
argument and is used as the content handler of that location:

```nginx
server {
    listen 8080;

    location /hello {
        content_by_scheme '(begin (display "hello ") (display (+ 20 22)))';
    }

    location /sum {
        content_by_scheme '(begin (display "sum=") (number->string (+ 40 2)))';
    }

    location /fib {
        content_by_scheme '(begin (display "fib(24)=")
                                  (display
                                    (let fib ((n 24))
                                      (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))))';
    }
}
```

```sh
curl http://127.0.0.1:8080/hello   # -> hello 42
curl http://127.0.0.1:8080/sum     # -> sum=42
curl http://127.0.0.1:8080/fib     # -> fib(24)=46368
```
