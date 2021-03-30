#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libdstr/libdstr.h"
#include "server/badbadbad_alert.h"
#include "server/libsmsql.h"

static derr_t _gc_sessions(int server_id, const dstr_t *sock){
    derr_t e = E_OK;

    time_t now;
    PROP(&e, dtime(&now) );

    // get sql server ready
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        ORIG(&e, E_SQL, "unable to init mysql library");
    }

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG_GO(&e, E_SQL, "unable to init mysql object", cu_sql_lib);
    }

    PROP_GO(&e, sql_connect_unix(&sql, NULL, NULL, sock), cu_sql);

    PROP_GO(&e, gc_sessions_and_csrf(&sql, server_id, now), cu_sql);

cu_sql:
    mysql_close(&sql);

cu_sql_lib:
    mysql_library_end();

    return e;
}

static dstr_t get_arg(char **argv, size_t idx){
    dstr_t out;
    DSTR_WRAP(out, argv[idx], strlen(argv[idx]), true);
    return out;
}

static void print_help(FILE *f){
    fprintf(f,
        "gc_sessions: delete sessions older than a certain age\n"
        "\n"
        "usage: gc_sessions SERVER_ID [OPTIONS]\n"
        "\n"
        "and where OPTIONS are one of:\n"
        "  -d --debug\n"
        "  -s --socket PATH     default: /var/run/mysqld/mysqld.sock\n"
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;

    // specify command line options
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t o_sock = {'s', "socket", true, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {
        &o_debug,
        &o_sock,
    };

    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    // parse command line options
    IF_PROP(&e, opt_parse(argc, argv, spec, speclen, &newargc) ){
        print_help(stderr);
        DROP_CMD( logger_add_fileptr(LOG_LVL_ERROR, stderr) );
        goto fail;
    }

    if(newargc != 2){
        print_help(stderr);
        ORIG_GO(&e, E_INTERNAL, "bad usage", fail);
    }

    // SERVER_ID
    dstr_t server_id_arg = get_arg(argv, 1);
    int server_id;
    PROP_GO(&e,
        dstr_toi(&server_id_arg, &server_id, 10),
    fail);

    // --debug
    PROP_GO(&e,
        logger_add_fileptr(
            o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr
        ),
    fail);

    // --socket
    dstr_t *sock = o_sock.found ? &o_sock.val : NULL;

    PROP_GO(&e, _gc_sessions(server_id, sock), fail);

    return 0;

fail:
    badbadbad_alert(&DSTR_LIT("unknown error in gc_sessions"), &e.msg);
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
