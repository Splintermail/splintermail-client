#include <string.h>
#include <fcntl.h>
#define HAVE_STRUCT_TIMESPEC
#include <pthread.h>

#include <common.h>
#include <logger.h>
#include <pop_server.h>
#include <networking.h>
#include <pop_client.h>

#include "fake_pop_server.h"

#include <win_compat.h>

char g_username[] = "b@splintermail.com";
char g_password[] = "b";
unsigned int fps_pop_port = 2000;
unsigned int fps_ver_maj = 0;
unsigned int fps_ver_min = 2;
unsigned int fps_ver_bld = 0;

// for communicating when the fake_pop_server is ready
static pthread_mutex_t fps_mutex;
static pthread_cond_t  fps_cond;

// easy way to return value from a thread
static derr_t thread_error;
static derr_t test_error;

static bool keep_going = true;

static bool test_start = false;
void fps_start_test(void){
    pthread_mutex_lock(&fps_mutex);
    test_start = true;
    pthread_cond_signal(&fps_cond);
    pthread_mutex_unlock(&fps_mutex);
}

void fps_done(void){
    pthread_mutex_lock(&fps_mutex);
    test_start = true;
    keep_going = false;
    pthread_cond_signal(&fps_cond);
    pthread_mutex_unlock(&fps_mutex);
}

static bool test_end = false;
derr_t fps_end_test(void){
    pthread_mutex_lock(&fps_mutex);
    if(!test_end){
        // wait for fps thread to finish with its connection
        pthread_cond_wait(&fps_cond, &fps_mutex);
    }
    test_end = false;
    pthread_mutex_unlock(&fps_mutex);
    return test_error;
}

// common responses
DSTR_STATIC(multi_line_end, ".\r\n");
DSTR_STATIC(msg_dne, "-ERR no such message\r\n");
DSTR_STATIC(msg_ok, "+OK\r\n");

static derr_t loginhook(void* arg, const dstr_t* username,
                        const dstr_t* password, bool* login_ok){
    // suppress unused parameter warning
    (void) arg;
    *login_ok = true;
    // check username
    int result = strncmp(username->data, g_username, username->len);
    if(result != 0){
        *login_ok = false;
        return E_OK;
    }
    // check password
    result = strncmp(password->data, g_password, password->len);
    if(result != 0){
        *login_ok = false;
        return E_OK;
    }
    // we don't need to download anything
    return E_OK;
}
static derr_t stathook(void* arg){
    // suppress unused parameter warning
    (void) arg;
    ORIG(E_VALUE, "STAT hook not implemented in fake server");
}
static derr_t listhook(void* arg, int index){
    // suppress unused parameter warning
    (void) arg;
    (void) index;
    ORIG(E_VALUE, "LIST hook not implemented in fake server");
}
static derr_t retrhook(void* arg, unsigned int index){
    // cast arg to fake_pop_server_t*
    fake_pop_server_t* fps = (fake_pop_server_t*)arg;

    derr_t error;

    // get the internal index
    size_t internal_index = index - 1;
    // make sure the index isn't too high
    if(internal_index >= fps->deletions.len){
        PROP( pop_server_send_dstr(&fps->ps, &msg_dne) );
        return E_OK;
    }
    // make sure the email isn't deleted
    bool deleted = fps->deletions.data[internal_index];
    if(deleted == true){
        PROP( pop_server_send_dstr(&fps->ps, &msg_dne) );
        return E_OK;
    }

    /* get a copy of message, because pop3_encode() consumes the buffer but
       that's fine for the real DITM and I'm not changing the api for the
       fake server*/
    dstr_t* msg_orig = &fps->messages.data[internal_index];
    dstr_t msg;
    PROP( dstr_new(&msg, msg_orig->len) );
    PROP_GO( dstr_copy(msg_orig, &msg), cleanup_1);

    // now build the response
    dstr_t response;
    PROP_GO( dstr_new(&response, msg.len + 64), cleanup_1);

    // now build a response
    size_t length = fps->lengths.data[internal_index];
    // get the first line of the response
    PROP_GO( FMT(&response, "+OK %x octets\r\n", FU(length)), cleanup_2);

    // add the encoded message
    PROP_GO( pop3_encode(&msg, &response, true), cleanup_2);

    // now add the end of the multi-line response
    PROP_GO( dstr_append(&response, &multi_line_end), cleanup_2);

    // send the response
    PROP_GO( pop_server_send_dstr(&fps->ps, &response), cleanup_2);

cleanup_2:
    dstr_free(&msg);
cleanup_1:
    dstr_free(&response);
    return error;
}
static derr_t delehook(void* arg, unsigned int index){
    // cast arg to fake_pop_server_t*
    fake_pop_server_t* fps = (fake_pop_server_t*)arg;
    DSTR_VAR(response, 64);

    // get the internal index
    size_t internal_index = index - 1;
    // make sure the index isn't too high
    if(internal_index >= fps->deletions.len){
        PROP( pop_server_send_dstr(&fps->ps, &msg_dne) );
        return E_OK;
    }
    // make sure the email isn't deleted
    bool deleted = fps->deletions.data[internal_index];
    if(deleted == true){
        PROP( pop_server_send_dstr(&fps->ps, &msg_dne) );
        return E_OK;
    }
    // mark message for deletion
    fps->deletions.data[internal_index] = true;
    // now build a response
    PROP( FMT(&response, "+OK %x deleted\r\n", FI(index)) );
    PROP( pop_server_send_dstr(&fps->ps, &response) );
    return E_OK;
}
static derr_t rsethook(void* arg){
    // cast arg to fake_pop_server_t*
    fake_pop_server_t* fps = (fake_pop_server_t*)arg;
    size_t num_messages = fps->deletions.len;
    for(size_t i = 0; i < num_messages; i++){
        fps->deletions.data[i] = false;
    }
    PROP( pop_server_send_dstr(&fps->ps, &msg_ok) );
    return E_OK;
}
static derr_t tophook(void* arg, unsigned int index, unsigned int lines){
    // suppress unused parameter warning
    (void) arg;
    (void) index;
    (void) lines;
    ORIG(E_VALUE, "TOP hook not implemented in fake server");
}
static derr_t uidlhook(void* arg, int index){
    // cast arg to fake_pop_server_t*
    fake_pop_server_t* fps = (fake_pop_server_t*)arg;
    DSTR_VAR(response, 4096);

    // pop_server enforces that indicies are 1 or greater, or -1
    // so, first handle the case where we are given a specific index
    if(index > 0 ){
        ORIG(E_VALUE, "UIDL hook with index not implemented in fake server");

    //now we can handle the case where we need to list all messages
    }else if(index == -1){
        // send first line
        PROP( dstr_append(&response, &msg_ok) );
        // now get all the other lines of the response
        size_t num_messages = fps->uids.len;
        for(size_t i = 0; i < num_messages; i++){
            fflush(stdout);
            bool deleted = fps->deletions.data[i];
            if(deleted == false){
                // add one line to the response
                size_t index_show = i+1;
                dstr_t* uid = &fps->uids.data[i];
                PROP( FMT(&response, "%x %x\r\n", FU(index_show), FD(uid)) );
            }
        }
        // now add the end of the multi-line response
        PROP( dstr_append(&response, &multi_line_end) );
        PROP( pop_server_send_dstr(&fps->ps, &response) );
    }
    return E_OK;
}
static derr_t quithook(void* arg, bool* update_ok){
    // cast arg to fake_pop_server_t*
    fake_pop_server_t* fps = (fake_pop_server_t*)arg;
    // only do "update" state actions we entered the transaction state
    if(fps->logged_in == true){
        size_t num_messages = fps->deletions.len;
        for(size_t i = 0; i < num_messages; i++){
            size_t idx = num_messages - 1 - i;
            bool deleted = fps->deletions.data[idx];
            if(deleted){
                dstr_free(&fps->messages.data[idx]);
                dstr_free(&fps->uids.data[idx]);
                LIST_DELETE(dstr_t, &fps->messages, idx);
                LIST_DELETE(dstr_t, &fps->uids, idx);
                LIST_DELETE(bool, &fps->deletions, idx);
                LIST_DELETE(size_t, &fps->lengths, idx);
            }
        }
    }
    *update_ok = true;
    return E_OK;
}

static derr_t fake_pop_server_add_email(fake_pop_server_t* fps,
                                        const char* path, const char* uid){
    derr_t error;
    dstr_t message;
    PROP( dstr_new(&message, 4096) );
    // open the file
    DSTR_VAR(realpath, 4096);
    PROP( FMT(&realpath, "%x/%x", FS(g_test_files), FS(path)) );
    int fd = open(realpath.data, O_RDONLY);
    if(fd < 0){
        LOG_ERROR("%x: %x\n", FD(&realpath), FE(&errno));
        ORIG_GO(E_OS, "unable to open email file", cleanup_1);
    }
    // read the file into dstr_t message
    size_t amnt_read = 1;
    while(amnt_read){
        error = dstr_read(fd, &message, 4096, &amnt_read);
        if(error != E_OK){
            close(fd);
            goto cleanup_1;
        }
    }
    close(fd);
    // create the uid dstring
    dstr_t uid_d;
    PROP_GO( dstr_new(&uid_d, 32), cleanup_1);
    PROP_GO( FMT(&uid_d, "%x", FS(uid)), cleanup_2);

    // register the uid
    PROP_GO( LIST_APPEND(dstr_t, &fps->uids, uid_d), cleanup_2);
    // register the message
    PROP_GO( LIST_APPEND(dstr_t, &fps->messages, message), cleanup_1);
    // register the length
    PROP( LIST_APPEND(size_t, &fps->lengths, message.len) );
    // register the deletion status
    PROP( LIST_APPEND(bool, &fps->deletions, false) );

    return E_OK;

cleanup_2:
    dstr_free(&uid_d);
cleanup_1:
    dstr_free(&message);
    return error;
}

derr_t fake_pop_server_new(fake_pop_server_t* fps, const char** files,
                           size_t nfiles, const char** uids){
    derr_t error;
    PROP( LIST_NEW(dstr_t, &fps->uids, nfiles) );
    PROP_GO( LIST_NEW(dstr_t, &fps->messages, nfiles), cleanup_1);
    PROP_GO( LIST_NEW(size_t, &fps->lengths, nfiles), cleanup_2);
    PROP_GO( LIST_NEW(bool, &fps->deletions, nfiles), cleanup_3);

    // zero everything
    memset(fps->uids.data, 0, sizeof(fps->uids.data[0]) * fps->uids.len);
    memset(fps->messages.data, 0, sizeof(fps->messages.data[0]) * fps->messages.len);
    memset(fps->lengths.data, 0, sizeof(fps->lengths.data[0]) * fps->lengths.len);
    memset(fps->deletions.data, 0, sizeof(fps->deletions.data[0]) * fps->deletions.len);

    // now read all the files into the system
    for(size_t i = 0; i < nfiles; i++){
        PROP_GO( fake_pop_server_add_email(fps, files[i], uids[i]), cleanup_5);
    }
    LOG_INFO("FPS populated with %x messages\n", FU(fps->messages.len));

    return E_OK;

cleanup_5:
    for(size_t i = 0; i < fps->messages.len; i++){
        dstr_free(&fps->messages.data[i]);
    }
    for(size_t i = 0; i < fps->uids.len; i++){
        dstr_free(&fps->uids.data[i]);
    }
    LIST_FREE(bool, &fps->deletions);
cleanup_3:
    LIST_FREE(size_t, &fps->lengths);
cleanup_2:
    LIST_FREE(dstr_t, &fps->messages);
cleanup_1:
    LIST_FREE(dstr_t, &fps->uids);
    return error;
}

void fake_pop_server_free(fake_pop_server_t* fps){
    size_t num_messages = fps->uids.len;
    for(size_t i = 0; i < num_messages; i++){
        dstr_free(&fps->messages.data[i]);
        dstr_free(&fps->uids.data[i]);
    }

    LIST_FREE(bool, &fps->deletions);
    LIST_FREE(size_t, &fps->lengths);
    LIST_FREE(dstr_t, &fps->messages);
    LIST_FREE(dstr_t, &fps->uids);
    LOG_INFO("Fake pop server cleaned up\n");
}

static void* fake_pop_server_thread(void* arg){
    derr_t error;
    // cast arg to fps
    fake_pop_server_t* fps = (fake_pop_server_t*)arg;

    // prepare ssl context
    DSTR_VAR(certfile, 4096);
    DSTR_VAR(keyfile, 4096);
    DSTR_VAR(dhfile, 4096);
    PROP_GO( FMT(&certfile, "%x/%x", FS(g_test_files), FS("ssl/good-cert.pem")), cleanup_1);
    PROP_GO( FMT(&keyfile, "%x/%x", FS(g_test_files), FS("ssl/good-key.pem")), cleanup_1);
    PROP_GO( FMT(&dhfile, "%x/%x", FS(g_test_files), FS("ssl/dh_4096.pem")), cleanup_1);
    ssl_context_t ctx;
    PROP_GO(ssl_context_new_server(&ctx, certfile.data,
                                         keyfile.data,
                                         dhfile.data), cleanup_1);

    listener_t listener;
    error = listener_new_ssl(&listener, &ctx, "127.0.0.1", fps_pop_port);
    if(!error) LOG_INFO("FPS ready for incoming connections\n");
    // no matter what, signal the main thread
    pthread_mutex_lock(&fps_mutex);
    pthread_cond_signal(&fps_cond);
    pthread_mutex_unlock(&fps_mutex);
    // return error if necessary
    PROP_GO(error, cleanup_2);

    while(keep_going){
        // wait for main thread to let us start
        pthread_mutex_lock(&fps_mutex);
        if(!test_start){
            pthread_cond_wait(&fps_cond, &fps_mutex);
        }
        test_start = false;
        pthread_mutex_unlock(&fps_mutex);
        // make sure we haven't been cancelled
        if(!keep_going) break;

        connection_t conn;
        // accept a connection
        LOG_INFO("FPS about to accept()\n");
        PROP_GO( listener_accept(&listener, &conn), cleanup_3);
        LOG_INFO("FPS accepted()\n");

        // set the pop_server to talk over this connection
        pop_server_hooks_t hooks = { loginhook, stathook, listhook,
                                     retrhook, delehook, rsethook,
                                     tophook, uidlhook, quithook};

        fps->ps.conn = conn;
        fps->ps.hooks = hooks;

        // not logged in yet
        fps->logged_in = false;

        // send server greeting
        DSTR_VAR(banner, 128);
        PROP_GO( FMT(&banner, "+OK Fake POP Server Ready DITMv%x.%x.%x\r\n",
                     FU(fps_ver_maj), FU(fps_ver_min), FU(fps_ver_bld)), cleanup_4);
        PROP_GO( pop_server_send_dstr(&fps->ps, &banner), cleanup_4);

        // kick off server loop
        PROP_GO( pop_server_loop(&fps->ps, (void*)fps), cleanup_4);

cleanup_4:
        connection_close(&conn);
        LOG_INFO("FPS connection closed\n");

        // end-of-test
        pthread_mutex_lock(&fps_mutex);
        // save the error from this test
        test_error = error;
        // reset the error
        error = E_OK;
        test_end = true;
        pthread_cond_signal(&fps_cond);
        pthread_mutex_unlock(&fps_mutex);
    }

cleanup_3:
    listener_close(&listener);
cleanup_2:
    ssl_context_free(&ctx);
cleanup_1:
    thread_error = error;
    LOG_INFO("fps exiting normally\n");
    return NULL;
}

static pthread_t fps_thread;

derr_t fake_pop_server_start(fake_pop_server_t* fps){
    // prepare for the cond_wait
    pthread_cond_init(&fps_cond, NULL);
    pthread_mutex_init(&fps_mutex, NULL);

    // lock so the server doesn't signal before we are waiting
    pthread_mutex_lock(&fps_mutex);
    // start the server
    pthread_create(&fps_thread, NULL, fake_pop_server_thread, fps);
    // now wait the server to be ready
    pthread_cond_wait(&fps_cond, &fps_mutex);
    // unlock mutex
    pthread_mutex_unlock(&fps_mutex);

    return E_OK;
}

derr_t fake_pop_server_join(void){
    pthread_join(fps_thread, NULL);
    pthread_mutex_destroy(&fps_mutex);
    pthread_cond_destroy(&fps_cond);
    return thread_error;
}
