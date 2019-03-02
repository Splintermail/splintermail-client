#include "fake_imap_client.h"
#include <logger.h>

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}

derr_t fic_create(fic_t *fic, const dstr_t *url, unsigned short port){
    derr_t error;
    // mutex
    int ret = uv_mutex_init(&fic->resp_mutex);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "failed in uv_mutex_init");
    }
    // cond
    ret = uv_cond_init(&fic->resp_cond);
    if(ret < 0){
        uv_perror("uv_cond_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "failed in uv_cond_init", fail_mutex);
    }
    // allocate read buffers
    PROP_GO( dstr_new(&fic->resp, 4096), fail_cond);
    PROP_GO( dstr_new(&fic->resp_chunk, 4096), fail_resp);

    // command queue
    PROP_GO( llist_init(&fic->cmd_q, NULL, NULL), fail_resp_chunk);

    // SSL context
    PROP_GO( ssl_context_new_client(&fic->ssl), fail_q);

    // zero connection struct
    fic->conn = (connection_t){0};

    // threads get created after connection is made
    fic->threads_running = false;

    // misc stuff
    fic->url = url;
    fic->port = port;
    fic->reader_error = E_OK;
    fic->writer_error = E_OK;

    return E_OK;

fail_q:
    llist_free(&fic->cmd_q);
fail_resp_chunk:
    dstr_free(&fic->resp_chunk);
fail_resp:
    dstr_free(&fic->resp);
fail_cond:
    uv_cond_destroy(&fic->resp_cond);
fail_mutex:
    uv_mutex_destroy(&fic->resp_mutex);
    return error;
}

derr_t fic_destroy(fic_t *fic){
    derr_t error;
    derr_t any_error = E_OK;
    if(fic->threads_running){
        // pass a NULL pointer command to the writer thread
        llist_elem_t n = {.data = NULL};
        llist_append(&fic->cmd_q, &n);
        // join writer
        int ret = uv_thread_join(&fic->writer);
        if(ret < 0){
            error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
            CATCH(E_ANY){
                uv_perror("uv_thread_join", ret);
                LOG_ERROR("failed joining writer, continuing\n");
                any_error = error;
            }
        }
        // catch error from writer
        error = fic->writer_error;
        CATCH(E_ANY){
            LOG_ERROR("writer threw error\n");
            if(!any_error) any_error = error;
        }
        // indicate to the reader thread that we are done
        fic->threads_continue = false;
        // close connection to force reader awake
        connection_close(&fic->conn);
        ret = uv_thread_join(&fic->reader);
        if(ret < 0){
            derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
            CATCH(E_ANY){
                uv_perror("uv_thread_join", ret);
                LOG_ERROR("failed joining reader, continuing\n");
                if(!any_error) any_error = error;
            }
        }
        // catch error from reader
        error = fic->reader_error;
        CATCH(E_ANY){
            LOG_ERROR("reader threw error\n");
            if(!any_error) any_error = error;
        }
    }
    ssl_context_free(&fic->ssl);
    llist_free(&fic->cmd_q);
    dstr_free(&fic->resp_chunk);
    dstr_free(&fic->resp);
    uv_cond_destroy(&fic->resp_cond);
    uv_mutex_destroy(&fic->resp_mutex);
    return any_error;
}

static void *reader_thread(){
}

// commands that can be given:
derr_t fic_connect(fic_t *fic){
    derr_t error;

    /* fuck.  This is not going to work because you can't have different
       threads reading and writing the same socket with openssl. */

}
// derr_t fic_disconnect(fic_t *fic);
// derr_t fic_command(fic_t *fic, dstr_t command);
// derr_t fic_get_resp(fic_t *fic, const dstr_t *tag, dstr_t *response);
/* note that to get unilateral server messages you'd have send a command (maybe
   NOOP) and you would see the uniltaeral server message above the response */
