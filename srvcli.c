#include <string.h>

#include "common.h"
#include "logger.h"
#include "networking.h"

#include "win_compat.h"

static derr_t connect_tls(const char* addr, char* port){
    derr_t error;
    unsigned int uport;
    dstr_t dport;
    DSTR_WRAP(dport, port, strlen(port), true);
    PROP( dstr_tou(&dport, &uport, 10) );

    // create ssl context
    ssl_context_t ctx;
    PROP( ssl_context_new_client(&ctx) );

    // make a connection
    connection_t conn;
    PROP_GO( connection_new_ssl(&conn, &ctx, addr, uport), cu_ctx);

    PFMT("writing\n");

    // write something
    DSTR_STATIC(req, "GET / HTTP/1.0\r\n\r\n");
    PROP_GO( connection_write(&conn, &req), cu_conn);

    PFMT("reading\n");

    // read something
    DSTR_VAR(resp, 4096);
    PROP_GO( connection_read(&conn, &resp, NULL), cu_conn);

    PFMT("read:\n-------\n%x-------\n", FD(&resp));

cu_conn:
    connection_close(&conn);
cu_ctx:
    ssl_context_free(&ctx);
    return error;
}

static derr_t host_tls(const char* addr, char* port,
                       char* certfile, char* keyfile, char* dhfile){
    derr_t error;
    unsigned int uport;
    dstr_t dport;
    DSTR_WRAP(dport, port, strlen(port), true);
    PROP( dstr_tou(&dport, &uport, 10) );

    // create ssl context
    ssl_context_t ctx;
    //const char* certfile = "test/files/cert.pem";
    //const char* keyfile  = "test/files/key.pem";
    //const char* dhfile   = "test/files/dh_4096.pem";
    PROP( ssl_context_new_server(&ctx, certfile, keyfile, dhfile) );

    // create a listener
    listener_t listener;
    PROP_GO( listener_new_ssl(&listener, &ctx, addr, uport), cu_ctx);

    // accept a connection
    connection_t conn;
    PROP_GO( listener_accept(&listener, &conn), cu_listener);

    PFMT("reading\n");

    // read something
    DSTR_VAR(resp, 4096);
    PROP_GO( connection_read(&conn, &resp, NULL), cu_conn);

    PFMT("read:\n-------\n%x-------\n", FD(&resp));

    PFMT("writing\n");

    // write something
    DSTR_STATIC(req, "Well, hi there, buddy!\n");
    PROP_GO( connection_write(&conn, &req), cu_conn);

cu_conn:
    connection_close(&conn);
cu_listener:
    listener_close(&listener);
cu_ctx:
    ssl_context_free(&ctx);
    return error;
}


int main(int argc, char** argv){
    logger_add_fileptr(LOG_LVL_DEBUG, stdout);

    ssl_library_init();

    if(argc > 1){
        if(strcmp("client", argv[1]) == 0){
            if(argc == 4)
                return connect_tls(argv[2], argv[3]);
        }
        if(strcmp("server", argv[1]) == 0){
            if(argc == 6)
                return host_tls(argv[2], argv[3], argv[4], argv[5], NULL);
            if(argc == 7)
                return host_tls(argv[2], argv[3], argv[4], argv[5], argv[6]);
        }
    }
    printf("usage %s client <addr> <port>\n", argv[0]);
    printf("usage %s server <addr> <port> <certificate> <key> [dhfile]\n", argv[0]);
    printf("   or %s print_system_certs\n", argv[0]);
    return 255;
}
