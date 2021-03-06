#define HAVE_STRUCT_TIMESPEC
#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>

#include <common.h>
#include <logger.h>
#include <ditm.h>
#include <networking.h>
#include <fileops.h>

#include "test_utils.h"

#define NOSSL2  SSL_OP_NO_SSLv2
#define NOSSL3  SSL_OP_NO_SSLv3
#define NOTLS1  SSL_OP_NO_TLSv1
#define NOTLS11 SSL_OP_NO_TLSv1_1
#define NOTLS12 SSL_OP_NO_TLSv1_2
#define NOTLS13 SSL_OP_NO_TLSv1_3

// Openssl 1.1.1 supports TLS1.3
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    // Openssl >= 1.1.1
    #define NOPROTOCOLS (NOSSL2|NOSSL3|NOTLS1|NOTLS11|NOTLS12|NOTLS13)
#else
    // Openssl < 1.1.1
    #define NOPROTOCOLS (NOSSL2|NOSSL3|NOTLS1|NOTLS11|NOTLS12)
#endif


// path to where the test files can be found
static const char* g_test_files;

static void log_errors(void){
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        LOG_ERROR("OpenSSL error: %x\n", FS(buffer));
    }
}

static derr_t print_protocol_and_cipher(connection_t* conn){

    // get the SSL object, will not need to be freed
    SSL* ssl = NULL;
    BIO_get_ssl(conn->bio, &ssl);
    if(!ssl){
        log_errors();
        ORIG(E_SSL, "error getting ssl from bio");
    }

    // print the protocol
    const char* protocol = SSL_get_version(ssl);
    if(!protocol){
        log_errors();
        ORIG(E_SSL, "error getting protocol from ssl");
    }
    LOG_INFO("protocol: %x\n", FS(protocol));

    // print the cipher
    const char* cipher = SSL_get_cipher(ssl);
    if(!cipher){
        log_errors();
        ORIG(E_SSL, "error getting cipher from ssl");
    }
    LOG_INFO("cipher: %x\n", FS(cipher));

    return E_OK;
}

typedef struct {
    // which files to use (can be NULL)
    const char* keypair;
    // which port to run on?
    unsigned int port;
    // use the networking.c implementation or a customizable one?
    bool vanilla;
    // these options only apply to non-vanilla servers:
    unsigned long ssl_types;
    // automatically handled values
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    // return values
    derr_t error;
} server_spec_t;

typedef struct {
    // hostname (can be NULL)
    const char* hostname;
    // use the networking.c implementation or a customizable one?
    bool vanilla;
    // these options only apply to non-vanilla clients:
    unsigned long ssl_types;
    const char* castorefile;
} client_spec_t;

static void* ssl_server_thread(void* arg){
    // here we define how the server will be configured
    server_spec_t* spec = arg;
    spec->error = E_OK;

    derr_t error = spec->error;

    DSTR_VAR(certfile, 4096);
    DSTR_VAR(keyfile, 4096);
    DSTR_VAR(dhfile, 4096);
    PROP_GO( FMT(&certfile, "%x/ssl/%x-cert.pem", FS(g_test_files),
                 FS(spec->keypair ? spec->keypair : "good")), exit);
    PROP_GO( FMT(&keyfile, "%x/ssl/%x-key.pem", FS(g_test_files),
                 FS(spec->keypair ? spec->keypair : "good")), exit);
    PROP_GO( FMT(&dhfile, "%x/ssl/dh_4096.pem", FS(g_test_files)), exit);
    if(!file_r_access(certfile.data)){
        ORIG_GO(E_FS, "unable to access certfile", exit);
    }
    if(!file_r_access(keyfile.data)){
        ORIG_GO(E_FS, "unable to access keyfile", exit);
    }
    if(!file_r_access(dhfile.data)){
        ORIG_GO(E_FS, "unable to access dhfile", exit);
    }

    // create the context, either a vanilla one or a customizable
    ssl_context_t ctx;
    ctx.ctx = NULL;

    // vanilla server context for testing the server implementation
    if(spec->vanilla == true){

        // prepare ssl context
        PROP_GO(ssl_context_new_server(&ctx, certfile.data, keyfile.data,
                                       dhfile.data),
                signal_client);
    }
    // custom server context for testing the client implementation
    else{
        // using the openssl pre-1.1.0 API because it seems less "automatic"

        // allow server to start talking to anybody
        const SSL_METHOD* meth = SSLv23_server_method();
        ctx.ctx = NULL;
        ctx.ctx = SSL_CTX_new(meth);
        if(!ctx.ctx){
            log_errors();
            ORIG_GO(E_NOMEM, "failed to create SSL context", signal_client);
        }

        // set default ssl protocols to be TLS (if not specified)
        unsigned long ssl_types = spec->ssl_types ? spec->ssl_types : NOSSL2 | NOSSL3;
        unsigned long ulret = SSL_CTX_set_options(ctx.ctx, ssl_types);
        if(!(ulret & spec->ssl_types)){
            log_errors();
            ORIG_GO(E_SSL, "failed to limit SSL methods", ctx_fail);
        }

        // set key and cert
        int ret = SSL_CTX_use_certificate_chain_file(ctx.ctx, certfile.data);
        if(ret != 1){
            log_errors();
            ORIG_GO(E_SSL, "could not set certificate", ctx_fail);
        }
        ret = SSL_CTX_use_PrivateKey_file(ctx.ctx, keyfile.data, SSL_FILETYPE_PEM);
        if(ret != 1){
            log_errors();
            ORIG_GO(E_SSL, "could not set private key", ctx_fail);
        }
        // make sure the key matches the certificate
        ret = SSL_CTX_check_private_key(ctx.ctx);
        if(ret != 1){
            log_errors();
            ORIG_GO(E_SSL, "private key does not match certificate", ctx_fail);
        }

        // set diffie-helman perameters
        DH* dh = NULL;
        FILE* fp = fopen(dhfile.data, "r");
        if(!fp){
            // already checked read access above, so highly likely E_NOMEM
            ORIG_GO(E_NOMEM, "unable to open dhfile", ctx_fail);
        }
        dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
        fclose(fp);
        if(!dh){
            log_errors();
            ORIG_GO(E_SSL, "failed to read dh params", ctx_fail);
        }
        long lret = SSL_CTX_set_tmp_dh(ctx.ctx, dh);
        if(lret != 1){
            DH_free(dh);
            log_errors();
            ORIG_GO(E_SSL, "failed to set dh params", ctx_fail);
        }
        DH_free(dh);

        // make sure server sets cipher preference
        ulret = SSL_CTX_set_options(ctx.ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
        if( !(ulret & SSL_OP_CIPHER_SERVER_PREFERENCE) ){
            log_errors();
            ORIG_GO(E_SSL, "failed to set server cipher preference ", ctx_fail);
        }

        ret = SSL_CTX_set_cipher_list(ctx.ctx, PREFERRED_CIPHERS);
        if(ret != 1){
            log_errors();
            ORIG_GO(E_SSL, "could not set ciphers", ctx_fail);
        }

        // read/write operations should only return after handshake completed
        lret = SSL_CTX_set_mode(ctx.ctx, SSL_MODE_AUTO_RETRY);
        if(!(lret & SSL_MODE_AUTO_RETRY)){
            log_errors();
            ORIG_GO(E_SSL, "error setting SSL mode", ctx_fail);
        }

        if(error != E_OK){
ctx_fail:
            SSL_CTX_free(ctx.ctx);
            ctx.ctx = NULL;
            goto signal_client;
        }
    }

    // create the listener with our context
    listener_t listener;
    error = listener_new_ssl(&listener, &ctx, "127.0.0.1", spec->port);

signal_client:
    // before we signal the client, let it know if we are about to puke
    spec->error = error;

    // no matter what, signal the main thread
    pthread_mutex_lock(&spec->mutex);
    pthread_cond_signal(&spec->cond);
    pthread_mutex_unlock(&spec->mutex);
    // return error if necessary
    PROP_GO(error, c_ctx);

    connection_t conn;
    // accept a connection
    PROP_GO( listener_accept(&listener, &conn), c_listener);

    // read the message off the wire
    DSTR_VAR(message, 256);
    PROP_GO( connection_read(&conn, &message, NULL), c_conn);

    // echo the message back
    PROP_GO( connection_write(&conn, &message), c_conn);

    // cleanup
c_conn:
    connection_close(&conn);
c_listener:
    listener_close(&listener);
c_ctx:
    ssl_context_free(&ctx);
exit:
    spec->error = error;
    return NULL;
}

static derr_t ssl_server_join(server_spec_t* spec){
    // not planning on having a loop anymore
    // if(spec->in_loop){
    // join the thread
    pthread_join(spec->thread, NULL);
    // cleanup the mutex and conditional variable
    pthread_mutex_destroy(&spec->mutex);
    pthread_cond_destroy(&spec->cond);
    return spec->error;
}

static derr_t ssl_server_start(server_spec_t* spec){
    // prepare for the cond_wait
    pthread_cond_init(&spec->cond, NULL);
    pthread_mutex_init(&spec->mutex, NULL);
    // lock so the server doesn't signal before we are waiting
    pthread_mutex_lock(&spec->mutex);
    // start the server
    pthread_create(&spec->thread, NULL, ssl_server_thread, (void*)spec);
    // now wait the server to be ready
    pthread_cond_wait(&spec->cond, &spec->mutex);
    // unlock mutex
    pthread_mutex_unlock(&spec->mutex);

    // if the server is about to puke, just join() it right now
    if(spec->error){
        return ssl_server_join(spec);
    }

    return E_OK;
}

typedef struct {
    derr_t client;
    derr_t server;
} derr_pair_t;

static derr_pair_t do_ssl_test(server_spec_t* srv_spec, client_spec_t* cli_spec){
    derr_t error = E_OK;
    derr_t server_error = E_OK;

    // start the server
    server_error = ssl_server_start(srv_spec);
    if(server_error) goto exit;

    // setup the client context
    ssl_context_t ctx;
    // vanilla client context for testing the client implementation
    if(cli_spec->vanilla == true){
        PROP_GO( ssl_context_new_client(&ctx), c_server);
    }
    // custom client context for testing the server implementation
    else{
        // using the openssl pre-1.1.0 API because it seems less "automatic"

        // allow client to start talking to anybody
        const SSL_METHOD* meth = SSLv23_client_method();
        ctx.ctx = NULL;
        ctx.ctx = SSL_CTX_new(meth);
        if(!ctx.ctx){
            log_errors();
            ORIG_GO(E_NOMEM, "failed to create SSL context", ctx_fail);
        }
        // set protocol limits based on spec
        unsigned long ulret = SSL_CTX_set_options(ctx.ctx, cli_spec->ssl_types);
        if(!(ulret & cli_spec->ssl_types)){
            log_errors();
            ORIG_GO(E_SSL, "failed to limit SSL methods", ctx_fail);
        }

        // load SSL certificate location
        if(cli_spec->castorefile == NULL){
            // load the actual system store
            PROP_GO( ssl_context_load_from_os(&ctx), ctx_fail);
        }else{
            // load the ca store given in the client spec
            const char* location = cli_spec->castorefile;
            int ret = SSL_CTX_load_verify_locations(ctx.ctx, location, NULL);
            if(ret != 1){
                log_errors();
                DSTR_VAR(msg, 1024);
                PROP_GO( FMT(&msg, "failed to load verify_location: %x\n",
                                   FS(location)),
                         ctx_fail);
                ORIG_GO(E_OS, msg.data, ctx_fail);
            }
        }

        int ret = SSL_CTX_set_cipher_list(ctx.ctx, PREFERRED_CIPHERS);
        if(ret != 1){
            log_errors();
            ORIG_GO(E_SSL, "could not set ciphers", ctx_fail);
        }

        // read/write operations should only return after handshake completed
        long lret = SSL_CTX_set_mode(ctx.ctx, SSL_MODE_AUTO_RETRY);
        if(!(lret & SSL_MODE_AUTO_RETRY)){
            log_errors();
            ORIG_GO(E_SSL, "error setting SSL mode", ctx_fail);
        }

ctx_fail:
        if(error){
            SSL_CTX_free(ctx.ctx);
            ctx.ctx = NULL;
            goto c_server;
        }
    }

    // setup a connection
    connection_t conn;
    const char* hostname = cli_spec->hostname ? cli_spec->hostname : "127.0.0.1";
    PROP_GO( connection_new_ssl(&conn, &ctx, hostname, srv_spec->port), c_ctx);

    // if we made a connection, print the protocol and cipher
    print_protocol_and_cipher(&conn);

    // write something
    DSTR_STATIC(msg_out, "this is a test\n");
    PROP_GO( connection_write(&conn, &msg_out), c_conn);

    // read something
    DSTR_VAR(msg_in, 256);
    PROP_GO( connection_read(&conn, &msg_in, NULL), c_conn);

    // they should match (server is echoing)
    if(dstr_cmp(&msg_in, &msg_out) != 0){
        ORIG_GO(E_VALUE, "msg_in did not match msg_out", c_conn);
    }

c_conn:
    connection_close(&conn);
c_ctx:
    ssl_context_free(&ctx);
c_server:
    server_error = ssl_server_join(srv_spec);
exit:
    CATCH(server_error){}
    CATCH(error){}
    derr_pair_t ret = {.client=error, .server=server_error};
    return ret;
}

#define EXPECT_ERRORS(eclient, eserver) \
    { \
        bool keep_going = true; \
        if(eclient != errors.client){ \
            LOG_ERROR("expected client to return %x but got %x\n", \
                      FD(error_to_dstr(eclient)), \
                      FD(error_to_dstr(errors.client))); \
            keep_going = false; \
        } \
        if(eserver != errors.server){ \
            LOG_ERROR("expected server to return %x but got %x\n", \
                      FD(error_to_dstr(eserver)), \
                      FD(error_to_dstr(errors.server))); \
            keep_going = false; \
        } \
        if(keep_going == false){ \
            ORIG(E_VALUE, "test failed: returned wrong values"); \
        } \
        LOG_INFO("-- test passed\n"); \
    }

static derr_t test_ssl_client(void){
    // basic test, vanilla client and server
    {
        LOG_INFO("\n-- testing vanilla client vs vanilla server\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
    // client tests, vanilla client and custom servers
    {
        LOG_INFO("\n-- testing vanilla client vs SSLv2 server\n");
        unsigned long ssl_types = NOPROTOCOLS ^ NOSSL2;
        server_spec_t srv_spec = {.vanilla=false, .port=2010, .ssl_types=ssl_types};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_CONN, E_CONN);
    }
    {
        LOG_INFO("\n-- testing vanilla client vs SSLv3 server\n");
        unsigned long ssl_types = NOPROTOCOLS ^ NOSSL3;
        server_spec_t srv_spec = {.vanilla=false, .port=2010, .ssl_types=ssl_types};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_CONN, E_CONN);
    }
    {
        LOG_INFO("\n-- testing vanilla client vs TLSv1-only server\n");
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS1;
        server_spec_t srv_spec = {.vanilla=false, .port=2010, .ssl_types=ssl_types};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
    {
        LOG_INFO("\n-- testing vanilla client vs TLSv1.1-only server\n");
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS11;
        server_spec_t srv_spec = {.vanilla=false, .port=2010, .ssl_types=ssl_types};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
    {
        LOG_INFO("\n-- testing vanilla client vs TLSv1.2-only server\n");
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS12;
        server_spec_t srv_spec = {.vanilla=false, .port=2010, .ssl_types=ssl_types};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
    // OpenSSL 1.1.1 has TLSv1.3
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    {
        LOG_INFO("\n-- testing vanilla client vs TLSv1.3-only server\n");
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS13;
        server_spec_t srv_spec = {.vanilla=false, .port=2010, .ssl_types=ssl_types};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
#endif
    // server tests, custom clients and vanilla server
    /* with Openssl 1.1.0, SSLv2/3 clients are completely ignored by server,
       meaning the first two tests just hang.  I'm not going to fight to make
       the new api work with ancient encryption for the sake of the tests,  so
       we will skip them.  But anything using the old api should run these
       tests, so we won't erase them until OpenSSL 1.1.0 is ubiquitous. */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    {
        LOG_INFO("\n-- testing SSLv2 client vs vanilla server\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010};
        unsigned long ssl_types = NOPROTOCOLS ^ NOSSL2;
        client_spec_t cli_spec = {.vanilla=false, .ssl_types=ssl_types};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_CONN, E_CONN);
    }
    {
        LOG_INFO("\n-- testing SSLv3 client vs vanilla server\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010};
        unsigned long ssl_types = NOPROTOCOLS ^ NOSSL3;
        client_spec_t cli_spec = {.vanilla=false, .ssl_types=ssl_types};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_CONN, E_CONN);
    }
#endif
    {
        LOG_INFO("\n-- testing TLSv1 client vs vanilla server\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010};
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS1;
        client_spec_t cli_spec = {.vanilla=false, .ssl_types=ssl_types};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
    {
        LOG_INFO("\n-- testing TLSv1.1 client vs vanilla server\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010};
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS11;
        client_spec_t cli_spec = {.vanilla=false, .ssl_types=ssl_types};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
    {
        LOG_INFO("\n-- testing TLSv1.2 client vs vanilla server\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010};
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS12;
        client_spec_t cli_spec = {.vanilla=false, .ssl_types=ssl_types};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
    // OpenSSL 1.1.1 has TLSv1.3
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    {
        LOG_INFO("\n-- testing TLSv1.3 client vs vanilla server\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010};
        unsigned long ssl_types = NOPROTOCOLS ^ NOTLS13;
        client_spec_t cli_spec = {.vanilla=false, .ssl_types=ssl_types};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_OK, E_OK);
    }
#endif
    // more client testing: make sure bad server certs are properly rejected
    {
        LOG_INFO("\n-- testing vanilla client vs expired cert\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010, .keypair="expired"};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_SSL, E_CONN);
    }
    {
        LOG_INFO("\n-- testing vanilla client vs wronghost cert\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010, .keypair="wronghost"};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_SSL, E_CONN);
    }
    {
        LOG_INFO("\n-- testing vanilla client vs unknown cert\n");
        server_spec_t srv_spec = {.vanilla=true, .port=2010, .keypair="unknown"};
        client_spec_t cli_spec = {.vanilla=true};
        derr_pair_t errors = do_ssl_test(&srv_spec, &cli_spec);
        EXPECT_ERRORS(E_SSL, E_CONN);
    }

    return E_OK;
}

/*static derr_t test_ssl_server(void){
    derr_t error;
    // setup the context (context-specific step)
    ssl_context_t ctx;
    PROP( ssl_context_new_server(&ctx, "test/files/cert.pem",
                                       "test/files/key.pem",
                                       "test/files/dh_4096.pem") );

    // open an ssl listener
    listener_t listener;
    PROP_GO( listener_new_ssl(&listener, &ctx, "0.0.0.0", 12345), cleanup_1);

    // accept a connection
    connection_t conn;
    PROP_GO( listener_accept(&listener, &conn), cleanup_2);

    DSTR_STATIC(writeme, "hello world!\n");
    PROP_GO( connection_write(&conn, &writeme), cleanup_3 );

    DSTR_VAR(buffer, 4096);
    PROP_GO( connection_read(&conn, &buffer, NULL), cleanup_3 );
    PFMT("read: %x", FD(&buffer));

cleanup_3:
    connection_close(&conn);
cleanup_2:
    listener_close(&listener);
cleanup_1:
    ssl_context_free(&ctx);
    return error;
}

static derr_t test_server_repeatedly(void){
    derr_t error;
    // setup the context (context-specific step)
    ssl_context_t ctx;
    PROP( ssl_context_new_server(&ctx, "test/files/cert.pem",
                                       "test/files/key.pem",
                                       "test/files/dh_4096.pem") );

    // open an ssl listener
    listener_t listener;
    PROP_GO( listener_new_ssl(&listener, &ctx, "0.0.0.0", 12345), cleanup_1);

    while(1){
        // accept a connection
        connection_t conn;
        error = listener_accept(&listener, &conn);
        if(error){
            continue;
        }

        DSTR_VAR(buffer, 4096);
        error = connection_read(&conn, &buffer, NULL);
        if(error){
            connection_close(&conn);
            continue;
        }
        LOG_INFO("read: %x", FD(&buffer));

        connection_close(&conn);
    }

// TODO: currently you have to kill the program to get to exit the loop
//     listener_close(&listener);
cleanup_1:
    ssl_context_free(&ctx);
    return error;
} */


int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    // setup the library (application-wide step)
    PROP_GO( ssl_library_init(), test_fail);

    PROP_GO( test_ssl_client(), test_fail);
    // PROP_GO( test_ssl_server(), test_fail);
    // PROP_GO( test_server_repeatedly(), test_fail);

    LOG_ERROR("PASS\n");
    ssl_library_close();
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    ssl_library_close();
    return 1;
}
