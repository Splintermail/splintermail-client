#include <string.h>
#include <stdint.h>

#include "pop_client.h"
#include "logger.h"
#include "networking.h"

#define CHUNK_SIZE 4096

derr_t pop_client_new(pop_client_t* pc){
    memset(pc, 0, sizeof(pop_client_t));
    derr_t error;

    // wrap response around *response_buffer
    pc->response.data = pc->response_buffer;
    pc->response.size = sizeof(pc->response_buffer);
    pc->response.len = 0;
    pc->response.fixed_size = true;

    PROP( LIST_NEW(dstr_t, &pc->uids, 256) );
    PROP_GO( LIST_NEW(size_t, &pc->idxs, 256), cleanup_1);
    PROP_GO( dstr_new(&pc->uids_block, 4096), cleanup_2);
    return E_OK;

cleanup_2:
    LIST_FREE(size_t, &pc->idxs);
cleanup_1:
    LIST_FREE(dstr_t, &pc->uids);
    return error;
}


void pop_client_free(pop_client_t* pc){
    // close connection
    connection_close(&pc->conn);
    // free everything
    dstr_free(&pc->uids_block);
    LIST_FREE(size_t, &pc->idxs);
    LIST_FREE(dstr_t, &pc->uids);
}


derr_t pop_client_connect(pop_client_t* pc, ssl_context_t* ctx,
                          const char* addr, unsigned int port,
                          bool* status_ok, dstr_t* message){
    // Connect to remote server
    PROP( connection_new_ssl(&pc->conn, ctx, addr, port) );

    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    return E_OK;
}


derr_t pop_client_username(pop_client_t* pc, const dstr_t* username,
                           bool* status_ok, dstr_t* message){
    // set default value for outputs
    *status_ok = false;
    message->len = 0;

    // build buffer to send
    DSTR_VAR(buffer, 256);
    PROP( FMT(&buffer, "USER %x\r\n", FD(username)) );
    // send buffer
    PROP( pop_client_send_dstr(pc, &buffer) );
    // get response
    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    return E_OK;
}


derr_t pop_client_password(pop_client_t* pc, const dstr_t* password,
                           bool* status_ok, dstr_t* message){
    // set default value for outputs
    *status_ok = false;
    message->len = 0;

    // build buffer to send
    DSTR_VAR(buffer, 256);
    PROP( FMT(&buffer, "PASS %x\r\n", FD(password)) );
    // send buffer
    PROP( pop_client_send_dstr(pc, &buffer) );
    // get response
    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    return E_OK;
}


derr_t pop_client_uidl(pop_client_t* pc, bool* status_ok, dstr_t* message){
    // set default value for outputs
    *status_ok = false;
    message->len = 0;
    pc->uids.len = 0;
    pc->uids_block.len = 0;
    pc->idxs.len = 0;

    // buffer to send
    DSTR_STATIC(buffer, "UIDL\r\n");
    // send buffer
    PROP( pop_client_send_dstr(pc, &buffer) );
    // get response
    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    // if status is ERR, quit here
    if(*status_ok == false){
        return E_OK;
    }

    // load up the entire body
    bool end;
    PROP( pop_client_get_body(pc, &pc->uids_block, true, &end) );
    while(end == false){
        PROP( pop_client_get_body(pc, &pc->uids_block, false, &end) );
    }

    // prepare working memory
    LIST(dstr_t) lines;
    PROP( LIST_NEW(dstr_t, &lines, 128) );

    // now that we have allocated working memory,
    // prepare for PROP_GO and ORIG_GO macros
    derr_t error = E_OK;

    // patterns for splitting strings
    DSTR_STATIC(line_end, "\r\n");
    DSTR_STATIC(space, " ");

    // split lines
    PROP_GO( dstr_split(&pc->uids_block, &line_end, &lines), cleanup);

    // check for empty-list condition
    if(lines.len == 2 && lines.data[0].len == 0 && lines.data[1].len == 0){
        goto cleanup;
    }

    // now aside from the last line, which should be empty...
    if((lines.data[lines.len - 1]).len != 0){
        ORIG_GO(E_RESPONSE, "Error parsing UIDs", cleanup);
    }

    // ... every line should be of the form: "<index> <uidl>"
    for(size_t i = 0; i < lines.len - 1; i++){
        // we will use a static-sized dstr_list
        LIST_VAR(dstr_t, tokens, 8);

        // now try and split this line
        PROP_GO( dstr_split(&lines.data[i], &space, &tokens), cleanup);

        // verify there are 2 tokens
        if(tokens.len != 2){
            ORIG_GO(E_RESPONSE, "Error parsing UIDs", cleanup);
        }
        dstr_t* index_token = &tokens.data[0];
        dstr_t* uid = &tokens.data[1];

        // get index from token
        unsigned int index;
        PROP_GO( dstr_tou(index_token, &index), cleanup);

        // add index to the list
        PROP_GO( LIST_APPEND(size_t, &pc->idxs, index), cleanup);

        // add the UID token to the list
        PROP_GO( LIST_APPEND(dstr_t, &pc->uids, *uid), cleanup);
    }

cleanup:
    LIST_FREE(dstr_t, &lines);
    return error;
}


derr_t pop_client_retrieve(pop_client_t* pc, unsigned int index,
                           bool* status_ok, dstr_t* message){
    // set default value for outputs
    *status_ok = false;
    message->len = 0;

    // build buffer to send
    DSTR_VAR(buffer, 256);
    PROP( FMT(&buffer, "RETR %x\r\n", FU(index)) );
    // send buffer
    PROP( pop_client_send_dstr(pc, &buffer) );
    // get response
    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    return E_OK;
}


derr_t pop_client_delete(pop_client_t* pc, unsigned int index,
                         bool* status_ok, dstr_t* message){
    // set default value for outputs
    *status_ok = false;
    message->len = 0;

    // build buffer to send
    DSTR_VAR(buffer, 256);
    PROP( FMT(&buffer, "DELE %x\r\n", FU(index)) );
    // send buffer
    PROP( pop_client_send_dstr(pc, &buffer) );
    // get response
    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    return E_OK;
}


derr_t pop_client_reset(pop_client_t* pc, bool* status_ok, dstr_t* message){
    // set default value for outputs
    *status_ok = false;
    message->len = 0;

    // buffer to send
    DSTR_STATIC(buffer, "RSET\r\n");
    // send buffer
    PROP( pop_client_send_dstr(pc, &buffer) );
    // get response
    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    return E_OK;
}


derr_t pop_client_quit(pop_client_t* pc, bool* status_ok, dstr_t* message){
    // set default value for outputs
    *status_ok = false;
    message->len = 0;

    // buffer to send
    DSTR_STATIC(buffer, "QUIT\r\n");
    // send buffer
    PROP( pop_client_send_dstr(pc, &buffer) );
    // get response
    PROP( pop_client_get_status_and_message(pc, status_ok, message) );

    return E_OK;
}


//////// helper functions below

/* throws: E_INTERNAL if pc->response fills up
           E_CONN on broken connection */
static derr_t pop_client_read(pop_client_t* pc){
    size_t new_text_start = pc->response.len;
    derr_t error = connection_read(&pc->conn, &pc->response, NULL);
    // if we fill the pc->response, that is an internal error
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);
    dstr_t sub = dstr_sub(&pc->response, new_text_start, 0);
    LOG_DEBUG("pop_client read: %x", FD(&sub));
    return E_OK;
}

derr_t pop_client_send_dstr(pop_client_t* pc, const dstr_t* buffer){
    PROP( connection_write(&pc->conn, buffer) );

    LOG_DEBUG("pop_client wrote: %x", FD(buffer));

    return E_OK;
}


derr_t pop_client_get_status_and_message(pop_client_t* pc,
                                         bool* status_ok, dstr_t* message){
    LIST_PRESET(dstr_t, patterns, DSTR_LIT("\r\n"));
    char* end_pos;
    size_t which_pat;
    // read from the socket until we get the status and message
    while(1){
        if(pc->response.len > 0){
            // check if we have our end_token
            end_pos = dstr_find(&pc->response, &patterns, &which_pat, NULL);
            if(end_pos){
                break;
            }
        }
        // if not, try to read from the socket
        PROP( pop_client_read(pc) );
    }
    // find out where the message starts
    size_t offset = 0;
    if(pc->response.len >= 3
            && strncmp("+OK", pc->response.data, 3) == 0){
        *status_ok = true;
        offset = 4;
    }else if(pc->response.len >= 4
            && strncmp("-ERR", pc->response.data, 4) == 0){
        *status_ok = false;
        offset = 5;
    }else{
        ORIG(E_RESPONSE, "invalid status in remote pop server response");
    }
    // the end of the message is the char* end_pos from earlier
    if((uintptr_t)end_pos - (uintptr_t)pc->response.data <= offset){
        // in this case the message is an empty string
        message->len = 0;
    }else{
        size_t start = offset;
        size_t length = (uintptr_t)(end_pos - pc->response.data);
        dstr_t sub = dstr_sub(&pc->response, start, length);
        PROP( dstr_copy(&sub, message) );
    }
    // left-shift remaining bytes in pc->response dstr, if any
    size_t amount = (uintptr_t)(end_pos - pc->response.data)
                    + patterns.data[which_pat].len;
    dstr_leftshift(&pc->response, amount);

    return E_OK;
}

derr_t pop_client_get_body(pop_client_t* pc, dstr_t* body,
                           bool first_pass, bool* found_end){
    DSTR_STATIC(empty_body, ".\r\n");
    DSTR_STATIC(line_end, "\r\n");
    // on first pass, we read until we get at least 3 bytes to check
    if(first_pass == true){
        while( pc->response.len < empty_body.len ) {
            PROP( pop_client_read(pc) );
        }
        // then we check against the empty_body pattern
        dstr_t sub = dstr_sub(&pc->response, 0, empty_body.len);
        int result = dstr_cmp(&sub, &empty_body);
        // on match, leftshift pc->response, append to body, and set found_end
        if(result == 0){
            PROP( dstr_append(body, &line_end) );
            dstr_leftshift(&pc->response, empty_body.len);
            *found_end = true;
            return E_OK;
        }
    }

    // first try and decode any body that is already there
    size_t len_orig = pc->response.len;
    PROP( pop3_decode(&pc->response, body, found_end) );
    // if we decoded anything, just exit
    if(*found_end == true || pc->response.len < len_orig){
        return E_OK;
    }

    // if we didn't decode anything, try to read from the socket first
    PROP( pop_client_read(pc) );

    // then decode anything we read
    PROP( pop3_decode(&pc->response, body, found_end) );
    return E_OK;
}

derr_t pop3_decode(dstr_t* in, dstr_t* out, bool* found_end){
    // list of patterns
    LIST_PRESET(dstr_t, search, DSTR_LIT("\r\n.\r\n"), DSTR_LIT("\r\n.."));
    LIST_PRESET(dstr_t, replace, DSTR_LIT("\r\n"), DSTR_LIT("\r\n."));

    PROP( dstr_recode_stream(in, out, &search, &replace, false, 0, found_end) );

    return E_OK;
}

derr_t pop3_encode(dstr_t* in, dstr_t* out, bool force_end){
    // list of patterns (end pattern not appended by this function)
    LIST_PRESET(dstr_t, search, DSTR_LIT("\r\n."), DSTR_LIT("\n."));
    LIST_PRESET(dstr_t, replace, DSTR_LIT("\r\n.."), DSTR_LIT("\r\n.."));

    PROP( dstr_recode_stream(in, out, &search, &replace, force_end, 0, NULL) );

    return E_OK;
}
