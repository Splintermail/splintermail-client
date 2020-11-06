#include <stdio.h>
#include <stdlib.h>

#include "uv_util.h"
#include "libdstr/libdstr.h"

#include "server/badbadbad_alert.h"
#include "server/mysql_util.h"

typedef struct {
    const dstr_t *sock;
    const dstr_t *user;
    const dstr_t *pass;
} config_t;

typedef struct {
    uv_loop_t loop;
    uv_async_t closer;
    uv_timer_t timer;
    uv_udp_t udp;
    const config_t config;
} globals_t;

static void close_loop(globals_t *g){
    uv_udp_close(&g->udp, NULL);
    uv_timer_close(&g->timer, NULL);
    uv_async_close(&g->closer, NULL);
    uv_stop(&g->loop);
}

static void closer_cb(uv_async_t *handle){
    globals_t *g = handle->loop->data;
    close_loop(g);
}

static bool hard_exit = false;
static uv_async_t *close_async = NULL;
static void stop_loop_on_signal(int signum){
    (void) signum;
    LOG_ERROR("caught signal\n");
    if(hard_exit) exit(1);
    hard_exit = true;
    int ret = uv_async_send(close_async);
    if(ret < 0){
        exit(2);
    }
}


typedef struct {
    dstr_t fpr;
    bool add;
    dstr_t pub;
    link_t link;
} op_t;
DEF_CONTAINER_OF(op_t, link, link_t);


static derr_t op_new(
    const dstr_t *fpr, bool add, const dstr_t *pub, op_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    op_t *op = malloc(sizeof(*op));
    if(!op) ORIG(&e, E_NOMEM, "nomem");
    *op = (op_t){ .add = add };
    link_init(&op->link);

    PROP_GO(&e, dstr_copy(fpr, &op->fpr), fail_malloc);
    PROP_GO(&e, dstr_copy(pub, &op->pub), fail_fpr);

    *out = op;
    return e;

fail_fpr:
    dstr_free(&op->fpr);
fail_malloc:
    free(op);
    return e;
}


static void op_free(op_t *op){
    dstr_free(&op->pub);
    dstr_free(&op->fpr);
    free(op);
}


static derr_t get_unfinished_ops(MYSQL *sql, link_t *ops){
    derr_t e = E_OK;

    // build request
    DSTR_VAR(stmt, 256);
    PROP(&e,
        FMT(&stmt, "select fingerprint, add_op, public_key from keysync_ops")
    );
    LOG_DEBUG("executing: %x\n", FD(&stmt));

    // do request
    int ret = mysql_real_query(sql, stmt.data, stmt.len);
    if(ret != 0){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG(&e, E_SQL, "mysql_real_query failed");
    }

    // get result
    MYSQL_RES* res = mysql_use_result(sql);
    if(!res){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG(&e, E_SQL, "mysql_use_result failed");
    }

    // loop through results
    MYSQL_ROW row;
    while((row = mysql_fetch_row(res))){
        unsigned int nfields = mysql_num_fields(res);
        if(nfields != 3) ORIG_GO(&e, E_INTERNAL, "wrong nfields", fail_loop);
        unsigned long *lens = mysql_fetch_lengths(res);
        // wrap results in dstr_t's
        dstr_t fpr, addstr, pub;
        DSTR_WRAP(fpr, row[0], lens[0], 0);
        DSTR_WRAP(addstr, row[1], lens[1], 0);
        DSTR_WRAP(pub, row[2], lens[2], 0);

        bool add;
        if(dstr_cmp(&addstr, &DSTR_LIT("0")) == 0){
            add = true;
        }else if(dstr_cmp(&addstr, &DSTR_LIT("1")) == 0){
            add = false;
        }else{
            ORIG_GO(&e, E_INTERNAL, "invalid add field", fail_loop);
        }

        op_t *op;
        PROP_GO(&e, op_new(&fpr, add, &pub, &op), fail_loop);

        link_list_append(ops, &op->link);
    }

    mysql_free_result(res);

    // make sure we exited the loop without an error
    if(*mysql_error(sql)){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG_GO(&e, E_SQL, "error fetching rows", fail);
    }

    return e;

fail_loop:
    while(row) row = mysql_fetch_row(res);
    mysql_free_result(res);

    link_t *link;
fail:
    while((link = link_list_pop_first(ops))){
        op_t *op = CONTAINER_OF(link, op_t, link);
        op_free(op);
    }
    return e;
};


static derr_t sync_keys(const config_t config){
    derr_t e = E_OK;

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG(&e, E_SQL, "unable to init mysql object");
    }

    // make a connection with mysqld
    PROP_GO(&e,
        sql_connect_unix(&sql, config.user, config.pass, config.sock),
    cu_sql);

    // get unfinished operations
    link_t ops;
    link_init(&ops);
    PROP_GO(&e, get_unfinished_ops(&sql, &ops), cu_sql);

    link_t *link;
//cu_ops:
    while((link = link_list_pop_first(&ops))){
        op_t *op = CONTAINER_OF(link, op_t, link);
        op_free(op);
    }

cu_sql:
    mysql_close(&sql);

    return e;
}

static void do_work_timer(uv_timer_t *handle);

static void do_work(globals_t *g){
    derr_t e = E_OK;

    IF_PROP(&e, sync_keys(g->config) ){
        DUMP(e);
        DROP_VAR(&e);
    }

    int ret = uv_timer_stop(&g->timer);
    if(ret < 0){
        TRACE(&e, "uv_timer_stop: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error stopping timer", fail);
    }

    ret = uv_timer_start(&g->timer, do_work_timer, 5000, 0);
    if(ret < 0){
        TRACE(&e, "uv_timer_start: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error starting timer", fail);
    }

    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    // failed to set up timer!  Close ourselves and get restarted
    DSTR_STATIC(summary, "failed to reset timer!");
    badbadbad_alert(&summary, NULL);
    close_loop(g);
}

static void do_work_timer(uv_timer_t *handle){
    globals_t *g = handle->loop->data;
    do_work(g);
}

static void do_work_udp(
    uv_udp_t *handle,
    ssize_t nread,
    const uv_buf_t* buf,
    const struct sockaddr* addr,
    unsigned flags
){
    (void) flags;
    (void) buf;
    globals_t *g = handle->loop->data;

    // check for socket errors
    if(nread < 0){
        close_loop(g);
        return;
    }

    // ignore spurious messages, these indicate "no more stuff to read"
    if(nread == 0 && addr == NULL){
        return;
    }

    do_work(g);
}

static void udp_alloc(uv_handle_t *handle, size_t suggest, uv_buf_t *buf){
    (void)suggest;
    (void)handle;

    // give out the same buffer over and over, we never read it.
    static char dummy_buffer[1024];

    buf->base = dummy_buffer;
    buf->len = sizeof(dummy_buffer);

    return;
}

static derr_t keysync_service(const config_t config){
    derr_t e = E_OK;

    globals_t g = { .config = config };

    // init mysql
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        ORIG(&e, E_SQL, "unable to init mysql library");
    }

    ret = uv_loop_init(&g.loop);
    if(ret < 0){
        TRACE(&e, "uv_loop_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing loop", fail_sql);
    }
    g.loop.data = &g;

    // init the closer async
    ret = uv_async_init(&g.loop, &g.closer, closer_cb);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing async", fail_loop);
    }

    // init the timer
    ret = uv_timer_init(&g.loop, &g.timer);
    if(ret < 0){
        TRACE(&e, "uv_timer_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing timer", fail_async);
    }

    // start the timer
    ret = uv_timer_start(&g.timer, do_work_timer, 5000, 0);
    if(ret < 0){
        TRACE(&e, "uv_timer_start: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error starting timer", fail_timer);
    }

    // init the udp handle
    ret = uv_udp_init(&g.loop, &g.udp);
    if(ret < 0){
        TRACE(&e, "uv_udp_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing udp", fail_timer);
    }

    // bind to 127.0.0.1:8764
    const char *addr = "127.0.0.1";
    unsigned short port = 8764;
    struct sockaddr_in sai = {0};
    sai.sin_family = AF_INET;
    sai.sin_addr.s_addr = inet_addr(addr);
    sai.sin_port = htons(port);
    ret = uv_udp_bind_sockaddr_in(&g.udp, &sai, UV_UDP_REUSEADDR);
    if(ret < 0){
        TRACE(&e, "uv_udp_bind: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error binding udp", fail_udp);
    }

    ret = uv_udp_recv_start(&g.udp, udp_alloc, do_work_udp);
    if(ret < 0){
        TRACE(&e, "uv_udp_bind: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error binding udp", fail_udp);
    }

    // Enable signal handling
    close_async = &g.closer;
    signal(SIGINT, stop_loop_on_signal);
    signal(SIGTERM, stop_loop_on_signal);

fail_udp:
    if(is_error(e))  uv_udp_close(&g.udp, NULL);
fail_timer:
    if(is_error(e))  uv_timer_close(&g.timer, NULL);
fail_async:
    if(is_error(e))  uv_async_close(&g.closer, NULL);
fail_loop:
    if(is_error(e)) uv_stop(&g.loop);

    ret = uv_run(&g.loop, UV_RUN_DEFAULT);
    if(ret < 0){
        TRACE(&e, "uv_run: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_run error");
    }

    ret = uv_loop_close(&g.loop);
    if(ret != 0){
        LOG_ERROR("uv_loop_close: %x\n", FUV(&ret));
    }

fail_sql:
    mysql_library_end();

    return e;
}


static void print_help(FILE *file){
    fprintf(file,
        "keysync: a daemon to synchronize mailbox-accessible keys with the "
        "in the database\n"
        "\n"
        "usage: keysync OPTIONS\n"
        "\n"
        "where OPTIONS are one of:\n"
        "  -h --help\n"
        "  -d --debug\n"
        "  -s --socket PATH     default /var/run/mysqld/mysqld.sock\n"
        "     --user\n"
        "     --pass\n"
        "     --host            (not yet supported)\n"
        "     --port            (not yet supported)\n"
    );
}


int main(int argc, char **argv){
    derr_t e = E_OK;

    // specify command line options
    opt_spec_t o_help = {'h', "help", false, OPT_RETURN_INIT};
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t o_sock = {'\0', "socket", true, OPT_RETURN_INIT};
    opt_spec_t o_user = {'\0', "user", true, OPT_RETURN_INIT};
    opt_spec_t o_pass = {'\0', "pass", true, OPT_RETURN_INIT};
    opt_spec_t o_host = {'\0', "host", true, OPT_RETURN_INIT};
    opt_spec_t o_port = {'\0', "port", true, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {
        &o_help,
        &o_debug,
        &o_sock,
        &o_user,
        &o_pass,
        &o_host,
        &o_port,
    };

    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    // parse command line options
    e = opt_parse(argc, argv, spec, speclen, &newargc);
    if(is_error(e)){
        logger_add_fileptr(LOG_LVL_ERROR, stderr);
        DUMP(e);
        DROP_VAR(&e);
        return 2;
    }

    // print help?
    if(o_help.found){
        print_help(stdout);
        exit(0);
    }

    if(newargc > 0){
        print_help(stderr);
        exit(1);
    }

    // --debug
    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_WARN, stderr);

    // --host
    if(o_host.found){
        LOG_ERROR("--host not supported\n");
        exit(1);
    }

    // --port
    if(o_port.found){
        LOG_ERROR("--host not supported\n");
        exit(1);
    }

    // --sock, --user, and --pass
    config_t config = {
        .sock = o_sock.found ? &o_sock.val : NULL,
        .user = o_user.found ? &o_user.val : NULL,
        .pass = o_pass.found ? &o_pass.val : NULL,
    };

    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO(&e, keysync_service(config), fail);
    return 0;

fail:
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
