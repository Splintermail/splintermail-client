#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <unistd.h>

// the global loop
static uv_loop_t loop;

// something to kick
static uv_async_t loop_kicker;

// for now, just one connection is allowed
static uv_tcp_t conn;

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


static void allocator(uv_handle_t* handle, size_t suggest, uv_buf_t* buf) {
    (void)handle;
    (void)suggest;
    buf->base = malloc(256);
    buf->len = 256;
}


static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf){
    // check for EOF
    if(nread == UV_EOF){
        fprintf(stderr, "EOF\n");
        uv_read_stop(stream);
        uv_close((uv_handle_t*)stream, NULL);
        return;
    }

    // check for error
    if(nread < 0){
        fprintf(stderr, "error from read\n");
        uv_read_stop(stream);
        uv_close((uv_handle_t*)stream, NULL);
        return;
    }

    char buffer[128];
    int len = sprintf(buffer, "read (%zd/%zu): ", nread, buf->len);
    // echo what we read to stdout
    ssize_t written = write(1, buffer, (size_t)len);
    written = write(1, buf->base, (size_t)nread);
    written = write(1, "\n", 1);
    (void) written;
    // free the buffer
    if(buf->base) free(buf->base);
}


static int bind_via_gai(uv_tcp_t *srv, const char *addr, const char *svc){
    int error = 0;

    // prepare for getaddrinfo
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // get address of host
    struct addrinfo* ai;
    int ret = getaddrinfo(addr, svc, &hints, &ai);
    if(ret != 0){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    // bind to something
    struct addrinfo* p;
    for(p = ai; p != NULL; p = p->ai_next){
        struct sockaddr_in* sin = (struct sockaddr_in*)p->ai_addr;
        printf("binding to ip addr %s\n", inet_ntoa(sin->sin_addr));

        ret = uv_tcp_bind(srv, p->ai_addr, 0);
        if(ret < 0){
            uv_perror("uv_tcp_bind", ret);
            continue;
        }

        // if we made it here, we connected successfully
        break;
    }
    // make sure we found something
    if(p == NULL){
        fprintf(stderr, "failed to bind\n");
        error = -2;
        goto cu_ai;
    }

cu_ai:
    freeaddrinfo(ai);
    return error;
}


static void connection_cb(uv_stream_t* listener, int status){
    if(status < 0){
        uv_perror("uv_listen", status);
        // close loop and exit
        fprintf(stderr, "shutting down\n");
        uv_stop(&loop);
        goto cu_listener;
    }

    // prepare new connection
    int ret = uv_tcp_init(&loop, &conn);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        goto cu_listener;
    }

    // accept the connection
    ret = uv_accept(listener, (uv_stream_t*)&conn);
    if(ret < 0){
        uv_perror("uv_accept", ret);
        goto cu_conn;
    }

    // set up the read callback
    ret = uv_read_start((uv_stream_t*)&conn, allocator, read_cb);
    return;

cu_conn:
    uv_close((uv_handle_t*)&conn, NULL);
cu_listener:
    uv_close((uv_handle_t*)listener, NULL);
    return;
}


int main(void){
    // allocate loop
    int ret = uv_loop_init(&loop);
    if(ret < 0){
        uv_perror("uv_loop_init", ret);
        return 1;
    }

    // allocate async object
    uv_async_init(&loop, &loop_kicker, NULL);
    if(ret < 0){
        uv_perror("uv_async_init", ret);
        return 2;
    }

    // allocate TCP listener
    uv_tcp_t listener;
    ret = uv_tcp_init(&loop, &listener);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        return 2;
    }

    // bind TCP listener
    ret = bind_via_gai(&listener, "0.0.0.0", "12345");
    if(ret < 0){
        return 3;
    }

    // listen for connections
    ret = uv_listen((uv_stream_t*)&listener, 10, connection_cb);

    // run loop
    ret = uv_run(&loop, UV_RUN_DEFAULT);
    if(ret < 0){
        uv_perror("uv_run", ret);
        return 5;
    }

    // free loop
    ret = uv_loop_close(&loop);
    if(ret < 0){
        uv_perror("uv_loop_close", ret);
        return 1;
    }
    return 0;
}

