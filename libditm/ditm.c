#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>

#include "libditm/libditm.h"


#define UIDS_LENGTH 128
#define UIDS_SIZE (128*16)
#define CHUNK_SIZE 4096

// common response messages
DSTR_STATIC(msg_dne, "-ERR no such message\r\n");
DSTR_STATIC(msg_ok, "+OK\r\n");
DSTR_STATIC(multi_line_end, ".\r\n");

static derr_t ditm_download_new_message(ditm_t* ditm, key_tool_t* kt,
                                        ignore_list_t* il, const dstr_t* uid,
                                        size_t index);

DSTR_STATIC(update_subj, "Splintermail Software Update");
DSTR_STATIC(update_body,
    "The Splintermail software you are using is out of date.\r\n\r\n"
    "Please see:\r\n"
    "    https://github.com/splintermail/splintermail-client\r\n"
    "for upgrade instructions.\r\n");

DSTR_STATIC(badfs_subj, "File System Issue");
DSTR_STATIC(badfs_body,
    "DITM (the Splintermail software running on your computer for decrypting\r\n"
    "your Splintermail email) encountered a error reading or writing a file.\r\n"
    "\r\n"
    "This could be a file permissions issue, a full file system, or a bug.\r\n"
    "Please correct the issue, or contact help@splintermail.com for help.\r\n");

DSTR_STATIC(badsrv_subj, "Invalid Server Response");
DSTR_STATIC(badsrv_body,
    "DITM (the Splintermail software running on your computer for decrypting\r\n"
    "your Splintermail email) encountered a error talking to the server\r\n"
    "at Splintermail.com.\r\n"
    "\r\n"
    "This is probably a bug.  Please let us know at help@splintermail.com so\r\n"
    " we can fix it.\r\n");

DSTR_STATIC(intrn_err_subj, "DITM Internal Error");
DSTR_STATIC(intrn_err_body,
    "DITM (the Splintermail software running on your computer for decrypting\r\n"
    "your Splintermail email) encountered an internal error.\r\n"
    "\r\n"
    "This is definitely a bug.  Please let us know at help@splintermail.com so\r\n"
    " we can fix it.\r\n");

static derr_t loginhook_error_sensitive_part(ditm_t *ditm, bool *status_ok,
        dstr_t *userdir, const dstr_t* username, const dstr_t* password,
        dstr_t *message){
    derr_t e = E_OK;
    derr_t e2;

    // initialize the key_tool
    key_tool_t kt;
    PROP(&e, key_tool.new(&kt, userdir, 4096) );

    // load the ignore list
    ignore_list_t il;
    PROP_GO(&e, ignore_list_load(&il, userdir), cu_kt);

    // download new messages
    LOG_DEBUG("DITM UID's before download (%x):\n", FU(ditm->maildir.uids.len));
    for(size_t i = 0; i < ditm->maildir.uids.len; i++){
        LOG_DEBUG("    %x\n", FD(&ditm->maildir.uids.data[i]));
    }

    // get UID list from remote server
    e2 = pop_client_uidl(&ditm->pc, status_ok, message);
    // E_FIXEDSIZE from filling *message means a bad server response
    CATCH(e2, E_FIXEDSIZE){
        RETHROW_GO(&e, &e2, E_RESPONSE, cu_il);
    }else PROP_GO(&e, e2, cu_il);

    LOG_DEBUG("Remote server UID's (%x):\n", FU(ditm->pc.uids.len));
    for(size_t i = 0; i < ditm->pc.uids.len; i++){
        LOG_DEBUG("    %x\n", FD(&ditm->pc.uids.data[i]));
    }

    // make sure we have every message downloaded
    for(size_t i = 0; i < ditm->pc.idxs.len; i++){
        // get a pointer to the uid in question
        dstr_t* uid = &ditm->pc.uids.data[i];
        // don't do anything if it is on the ignore list
        if(ignore_list_should_ignore(&il, uid)){
            // this also registers a UID as "seen" in the ignore list
            continue;
        }
        size_t index = ditm->pc.idxs.data[i];
        // download the message
        PROP_GO(&e, ditm_download_new_message(ditm, &kt, &il, uid, index), cu_il);
    }

    LOG_DEBUG("DITM UID's after download (%x):\n", FU(ditm->maildir.uids.len));
    for(size_t i = 0; i < ditm->maildir.uids.len; i++){
        LOG_DEBUG("    %x\n", FD(&ditm->maildir.uids.data[i]));
    }

    // now prepare the deletions list
    for(size_t i = 0; i < ditm->maildir.lengths.len; i++){
        PROP_GO(&e, LIST_APPEND(bool, &ditm->deletions, false), cu_il);
    }

    // update the ignore list
    PROP_GO(&e, ignore_list_write(&il, userdir), cu_il);

    // update the key_tool
    PROP_GO(&e, key_tool.update(&kt, ditm->api_host, ditm->api_port,
                            username, password), cu_il);

cu_il:
    ignore_list_free(&il);
cu_kt:
    key_tool.free(&kt);

    return e;
}

static derr_t loginhook(void* arg, const dstr_t* username,
                        const dstr_t* password, bool* login_ok){
    derr_t e = E_OK;
    derr_t e2;
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;

    // default outputs
    *login_ok = false;

    // try login
    bool status_ok;
    DSTR_VAR(message, 1024);
    // USER
    PROP(&e, pop_client_username(&ditm->pc, username, &status_ok, &message) );
    if(!status_ok){
        return e;
    }
    // PASS
    PROP(&e, pop_client_password(&ditm->pc, password, &status_ok, &message) );
    if(!status_ok){
        return e;
    }

    *login_ok = true;
    ditm->logged_in = true;

    // HERE IS WHERE ERROR HANDLING BECOMES VERY IMPORTANT

    /* After this point, but before the maildir is set up, errors are not able
       to be passed to the user through the "local ditm email" strategy.
       However, it is possible there could be a user-caused configuration
       error, like a missing directory or permissions issue that the user would
       need to be alerted to.  Except unless the user reads the error log they
       won't know that.  So careful measures will have to be take care to make
       sure that DITM fails at startup rather than here, although in some
       situations (no disk space, no memory, user deletes ditm_dir) there is no
       way to guarantee we won't throw an error here. */

    // path to user dir, such as: /var/lib/splintermail/user@splintermail.com
    DSTR_VAR(userdir, 4096);
    PROP(&e, FMT(&userdir, "%x/%x", FD(&ditm->dirpath), FD(username)) );

    // check if ditm_dir/username is a valid directory
    if(!dir_rw_access(userdir.data, true)){
        ORIG(&e, E_FS, "failure to either create or access the userdir");
    }

    // open the maildir
    PROP(&e, maildir_new(&ditm->maildir, &userdir) );

    // HERE BEGINS SOME VERY CAREFUL ERROR HANDLING
    /* now that we have stood up the maildir we can inject meaningful messages
       to the user about what is happening.

       Caveat: we are choosing to drop path-too-long errors because they are
       too rare to be worth complicating the code over handling. */

    // make sure we met the minimum version or go straight to the msg injection
    if(ditm->minversion_met == false){
        ditm->conn_is_live = false;
        PROP(&e, ditm_inject_message(ditm, &update_subj, &update_body) );
        return e;
    }

    e2 = loginhook_error_sensitive_part(ditm, &status_ok, &userdir, username,
            password, &message);

    // END OF VERY CAREFUL ERROR HANDLING

    // silently disconnect from email client on temporary errors
    CATCH(e2, E_CONN, E_NOMEM, E_OS, E_SSL){
        // allow the error to propagate
        TRACE(&e2, "temporary error detected, disconnecting\n");
        RETHROW(&e, &e2, e2.type);
    }

    CATCH(e2, E_PARAM){
        /* E_PARAM comes from key_tool.update, and it means username, password,
           or host is too long, but we know username and password are valid...
           since we just checked with the server */
        /* this is really not a likely error to happen since the api_host is
           a hardcoded string in the user application */
        TRACE(&e2, "configured api_server hostname is too long\n");
        DUMP(e2);
        DROP_VAR(&e2);
        // continue in offline mode
        ditm->conn_is_live = false;
    }
    // if the user has a filesystem issue, tell them now
    else CATCH(e2, E_FS){
        DUMP(e2);
        DROP_VAR(&e2);
        // continue in offline mode
        ditm->conn_is_live = false;
        PROP(&e, ditm_inject_message(ditm, &badfs_subj, &badfs_body) );
    }
    // if we got an invalid server response... have the user complain to us
    else CATCH(e2, E_RESPONSE){
        DUMP(e2);
        DROP_VAR(&e2);
        // continue in offline mode
        ditm->conn_is_live = false;
        // TODO: include the trace in the message
        PROP(&e, ditm_inject_message(ditm, &badsrv_subj, &badsrv_body) );
    }
    // if we caught an internal error... sorry, user.  Here's an apology
    else CATCH(e2, E_INTERNAL, E_ANY){
        if(e2.type != E_INTERNAL){
            TRACE(&e2, "HEY! this error shouldn't be possible here\n");
        }
        DUMP(e2);
        DROP_VAR(&e2);
        // continue in offline mode
        ditm->conn_is_live = false;
        // TODO: include the trace in the message
        PROP(&e, ditm_inject_message(ditm, &intrn_err_subj, &intrn_err_body) );
    }

    return e;
}

static derr_t stathook(void* arg){
    derr_t e = E_OK;
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    // sum lengths and count how many deletions we have
    size_t num_messages = ditm->maildir.lengths.len;
    size_t num_deletions = 0;
    size_t total_length = 0;
    for(size_t i = 0; i < num_messages; i++){
        bool deleted = ditm->deletions.data[i];
        if(deleted == false){
            total_length += ditm->maildir.lengths.data[i];
        }else{
            num_deletions++;
        }
    }
    DSTR_VAR(response, 64);
    PROP(&e, FMT(&response, "+OK %x %x\r\n",
              FU(num_messages - num_deletions), FU(total_length)) );
    PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
    return e;
}

static derr_t listhook(void* arg, int index){
    derr_t e = E_OK;
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    DSTR_VAR(response, CHUNK_SIZE);

    // pop_server enforces that indicies are 1 or greater, or -1
    // so, first handle the case where we are given a specific index
    if(index > 0 ){
        // get the internal index
        size_t internal_index = (size_t)index - 1;
        // make sure the index isn't too high
        if(internal_index >= ditm->deletions.len){
            PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
            return e;
        }
        // make sure the email isn't deleted
        bool deleted = ditm->deletions.data[internal_index];
        if(deleted == true){
            PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
            return e;
        }
        // now build a response
        size_t length = ditm->maildir.lengths.data[internal_index];
        PROP(&e, FMT(&response, "+OK %x %x\r\n", FI(index), FU(length)) );
        PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
        return e;
    }
    //now we can handle the case where we need to list all messages
    // sum lengths and count how many deletions we have
    size_t num_messages = ditm->maildir.lengths.len;
    size_t num_deletions = 0;
    size_t total_length = 0;
    for(size_t i = 0; i < num_messages; i++){
        bool deleted = ditm->deletions.data[i];
        if(deleted == false){
            total_length += ditm->maildir.lengths.data[i];
        }else{
            num_deletions++;
        }
    }
    // get the first line of the response
    PROP(&e, FMT(&response, "+OK %x messages (%x octets)\r\n",
              FU(num_messages - num_deletions), FU(total_length)) );
    // now get all the other lines of the response
    for(size_t i = 0; i < num_messages; i++){
        bool deleted = ditm->deletions.data[i];
        if(deleted == false){
            // add one line to the response
            size_t index_show = i+1;
            size_t length = ditm->maildir.lengths.data[i];
            PROP(&e, FMT(&response, "%x %x\r\n", FU(index_show), FU(length)) );
            // check if response should be flushed
            if(response.size - response.len < 64){
                PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
                response.len = 0;
            }
        }else{
            LOG_DEBUG("deleted message not shown in listhook\n");
        }
    }
    // now add the end of the multi-line response
    PROP(&e, dstr_append(&response, &multi_line_end) );
    // send the response
    PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
    return e;
}

static derr_t retrhook(void* arg, unsigned int index){
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    DSTR_VAR(buffer, CHUNK_SIZE);
    DSTR_VAR(response, CHUNK_SIZE * 2);
    derr_t e = E_OK;

    // get the internal index
    size_t internal_index = index - 1;
    // make sure the index isn't too high
    if(internal_index >= ditm->deletions.len){
        PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
        return e;
    }
    // make sure the email isn't deleted
    bool deleted = ditm->deletions.data[internal_index];
    if(deleted == true){
        PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
        return e;
    }
    // open the message from the maildir
    int fd;
    PROP(&e, maildir_open_message(&ditm->maildir, internal_index, &fd) );

    // now build a response
    size_t length = ditm->maildir.lengths.data[internal_index];
    // get the first line of the response
    PROP_GO(&e, FMT(&buffer, "+OK %x octets\r\n", FU(length)), cleanup);

    while(true){
        // read from the file
        size_t amnt_read;
        PROP_GO(&e, dstr_read(fd, &buffer, 0, &amnt_read), cleanup);
        if(amnt_read == 0){
            break;
        }
        // check if we should flush the response
        if(buffer.len > buffer.size / 2){
            PROP_GO(&e, pop3_encode(&buffer, &response, false), cleanup);
            PROP_GO(&e, pop_server_send_dstr(&ditm->ps, &response), cleanup);
            response.len = 0;
        }
    }
    compat_close(fd);

    // encode the last of the response
    PROP(&e, pop3_encode(&buffer, &response, true));
    // now add the end of the multi-line response
    PROP(&e, dstr_append(&response, &multi_line_end) );
    // send the last of the response
    PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
    return e;

cleanup:
    compat_close(fd);
    return e;
}

static derr_t delehook(void* arg, unsigned int index){
    derr_t e = E_OK;
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    DSTR_VAR(response, 64);

    // pop_server enforces that indicies are 1 or greater, or -1

    // get the internal index
    size_t internal_index = index - 1;
    // make sure the index isn't too high
    if(internal_index >= ditm->deletions.len){
        PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
        return e;
    }
    // make sure the email isn't already deleted (thats an error)
    bool deleted = ditm->deletions.data[internal_index];
    if(deleted == true){
        PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
        return e;
    }
    // check the local index to make sure that it's not a local-only message
    dstr_t* l_uid = &ditm->maildir.uids.data[internal_index];
    DSTR_STATIC(l_only, "LOCAL-");
    dstr_t sub = dstr_sub(l_uid, 0, l_only.len);
    // if it is not local-only, we need to coordinate with the remote server
    if(dstr_cmp(&sub, &l_only) != 0){
        // fail if trying to delete a remote message without minimum version
        if(ditm->conn_is_live == false){
            PROP(&e, FMT(&response, "-ERR not deleted; remote connection not live\r\n") );
            PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
            return e;
        }
        // get the index of the message on the remote server
        size_t r_idx = 0;
        bool found_uid = false;
        for(size_t i = 0; i < ditm->pc.uids.len; i++){
            int result = dstr_cmp(l_uid, &ditm->pc.uids.data[i]);
            if(result == 0){
                r_idx = ditm->pc.idxs.data[i];
                found_uid = true;
                break;
            }
        }
        // make sure we found something
        if(found_uid == false){
            LOG_WARN("did not find uid on remote server\n");
        }else{
            // make sure we can delete the index from the remote server
            bool status_ok;
            DSTR_VAR(message, 1024);
            PROP(&e, pop_client_delete(&ditm->pc, (unsigned int)r_idx, &status_ok, &message) );
            if(status_ok == false){
                ORIG(&e, E_VALUE, "failed to delete on remote server");
            }
        }
    }
    // mark message for deletion
    ditm->deletions.data[internal_index] = true;
    // now build a response
    PROP(&e, FMT(&response, "+OK %x deleted\r\n", FI(index)) );
    PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
    return e;
}

static derr_t rsethook(void* arg){
    derr_t e = E_OK;
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    // first make sure we can propagate the reset to the server
    if(ditm->conn_is_live == true){
        bool status_ok;
        DSTR_VAR(message, 1024);
        PROP(&e, pop_client_reset(&ditm->pc, &status_ok, &message) );
        // if the server puked we just pass that message on
        if(!status_ok){
            DSTR_STATIC(error_prefix, "-ERR ");
            DSTR_STATIC(line_end, "\r\n");
            PROP(&e, pop_server_send_dstr(&ditm->ps, &error_prefix) );
            PROP(&e, pop_server_send_dstr(&ditm->ps, &message) );
            PROP(&e, pop_server_send_dstr(&ditm->ps, &line_end) );
            return e;
        }
    }
    // mark all messages as not deleted
    size_t num_messages = ditm->deletions.len;
    for(size_t i = 0; i < num_messages; i++){
        ditm->deletions.data[i] = false;
    }
    PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_ok) );
    return e;
}

static derr_t tophook(void* arg, unsigned int index, unsigned int lines){
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    // raw is what is read from the file
    DSTR_VAR(raw, CHUNK_SIZE);
    // checked is what is approved for the response (taking into TOP request)
    DSTR_VAR(checked, CHUNK_SIZE * 2);
    // the response is checked but with POP3 encoding
    DSTR_VAR(response, CHUNK_SIZE * 2);
    derr_t e = E_OK;

    // get the internal index
    size_t internal_index = index - 1;
    // make sure the index isn't too high
    if(internal_index >= ditm->deletions.len){
        PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
        return e;
    }
    // make sure the email isn't deleted
    bool deleted = ditm->deletions.data[internal_index];
    if(deleted == true){
        PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
        return e;
    }
    // open the message from the maildir
    int fd;
    PROP(&e, maildir_open_message(&ditm->maildir, internal_index, &fd) );

    // now build a response
    PROP_GO(&e, dstr_append(&response, &msg_ok), fail );

    // count how many lines we are going to send after sending headers
    bool header_done = false;
    size_t lines_sent = 0;
    while(true){
        // read from the file
        size_t amnt_read;
        PROP_GO(&e, dstr_read(fd, &raw, 0, &amnt_read), fail);
        // check if we need to cut off the message early
        if(header_done == false){
            // look for header end
            LIST_PRESET(dstr_t, header_end_pat, DSTR_LIT("\r\n\r\n"));
            size_t partial;
            char* pos = dstr_find(&raw, &header_end_pat, NULL, &partial);
            if(!pos){
                /* in this case we still haven't found the end of the headers,
                   so copy over everything (excpet the partial match at end) */
                PROP_GO(&e, dstr_append(&checked, &raw), fail);
                // remove the partial match from checked
                checked.len -= partial;
                // leftshift raw, leaving partial match in place
                dstr_leftshift(&raw, raw.len - partial);
            }else{
                /* in this case we just found the end of the headers,
                   so copy the headers over */
                header_done = true;
                dstr_t header;
                size_t hlen = (uintptr_t)(pos - raw.data)
                              + header_end_pat.data[0].len;
                header = dstr_sub(&raw, 0, hlen);
                PROP_GO(&e, dstr_append(&checked, &header), fail);
                dstr_leftshift(&raw, hlen);
            }
        }
        /* whether or not we started off looking for headers (the above "if"
           statment, if we are in the body we start counting lines (the next
           "if" statement. */
        if(header_done == true){
            // if lines is 0, just exit loop
            if(lines == 0){
                break;
            }
            // check if we have enough lines in raw to meet the limit
            DSTR_STATIC(line_end, "\r\n");
            size_t count = dstr_count(&raw, &line_end);
            if(count + lines_sent < lines){
                // we don't have enough lines, copy over all of raw
                PROP_GO(&e, dstr_append(&checked, &raw), fail);
                // if the last character is '\r' we will save it
                size_t partial = raw.len > 0 && raw.data[raw.len - 1] == '\r';
                checked.len -= partial;
                dstr_leftshift(&raw, raw.len - partial);
                lines_sent += count;
            }else{
                // we can't send all of what's in raw, we need to count lines
                count = 0;
                char* pos = NULL;
                LIST_PRESET(dstr_t, line_end_pat, DSTR_LIT("\r\n"));
                // let the substring start as all of raw
                dstr_t sub = dstr_sub(&raw, 0, 0);
                while(count + lines_sent < lines){
                    // find next line ending
                    pos = dstr_find(&sub, &line_end_pat, NULL, NULL);
                    // redefine substring to be what follows the line ending
                    pos += line_end_pat.data[0].len;
                    sub = dstr_sub(&sub,  (uintptr_t)(pos - sub.data), 0);
                    // increment count
                    count++;
                }
                // now let sub be all the lines we're about to send
                sub = dstr_sub(&raw, 0, (uintptr_t)(pos - raw.data));
                PROP_GO(&e, dstr_append(&checked, &sub), fail);
                // no more reading needs to be done
                break;
            }
        }
        // if we are done reading, then we break and dump what we have
        if(amnt_read == 0){
            break;
        }
        // check if we should flush the response
        if(checked.len >= checked.size / 2){
            PROP_GO(&e, pop3_encode(&checked, &response, false), fail);
            PROP_GO(&e, pop_server_send_dstr(&ditm->ps, &response), fail);
            response.len = 0;
        }
    }
    compat_close(fd);

    // encode the last of the response
    PROP(&e, pop3_encode(&checked, &response, true));
    // now add the end of the multi-line response
    PROP(&e, dstr_append(&response, &multi_line_end) );
    // send the last of the response
    PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
    return e;

fail:
    compat_close(fd);
    return e;
}

static derr_t uidlhook(void* arg, int index){
    derr_t e = E_OK;
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    DSTR_VAR(response, CHUNK_SIZE);

    // pop_server enforces that indicies are 1 or greater, or -1
    // so, first handle the case where we are given a specific index
    if(index > 0 ){
        // get the internal index
        size_t internal_index = (size_t)index - 1;
        // make sure the index isn't too high
        if(internal_index >= ditm->deletions.len){
            PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
            return e;
        }
        // make sure the email isn't deleted
        bool deleted = ditm->deletions.data[internal_index];
        if(deleted == true){
            PROP(&e, pop_server_send_dstr(&ditm->ps, &msg_dne) );
            return e;
        }
        // now build a response
        dstr_t* uid = &ditm->maildir.uids.data[internal_index];
        PROP(&e, FMT(&response, "+OK %x %x\r\n", FI(index), FD(uid)) );
        PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
        return e;
    }
    //now we can handle the case where we need to list all messages
    // get the first line of the response
    PROP(&e, dstr_append(&response, &msg_ok) );
    // now get all the other lines of the response
    size_t num_messages = ditm->maildir.uids.len;
    for(size_t i = 0; i < num_messages; i++){
        bool deleted = ditm->deletions.data[i];
        if(deleted == false){
            // add one line to the response
            size_t index_show = i+1;
            dstr_t* uid = &ditm->maildir.uids.data[i];
            PROP(&e, FMT(&response, "%x %x\r\n", FU(index_show), FD(uid)) );
            // check if response should be flushed
            if(response.size - response.len < 64){
                PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
                response.len = 0;
            }
        }
    }
    // now add the end of the multi-line response
    PROP(&e, dstr_append(&response, &multi_line_end) );
    // send the response
    PROP(&e, pop_server_send_dstr(&ditm->ps, &response) );
    return e;
}

static derr_t quithook(void* arg, bool* update_ok){
    derr_t e = E_OK;
    // cast arg to a ditm_t*
    ditm_t* ditm = (ditm_t*) arg;
    // make sure we can enter the update state on the server
    DSTR_VAR(message, 1024);
    PROP(&e, pop_client_quit(&ditm->pc, update_ok, &message) );
    if(ditm->logged_in == true && *update_ok){
        // delete all messages marked for deletion
        size_t num_messages = ditm->deletions.len;
        // they must be deleted in reverse order to preserve indicies
        for(size_t i = 0; i < num_messages; i ++){
            size_t idx = num_messages - 1 - i;
            bool deleted = ditm->deletions.data[idx];
            if(deleted == true){
                PROP(&e, maildir_delete_message(&ditm->maildir, idx) );
            }
        }
    }
    return e;
}

static derr_t ditm_parse_minversion(dstr_t* msg, unsigned int* maj,
                                    unsigned int* min, unsigned int* bld){
    derr_t e = E_OK;
    // search banner for minimum version
    LIST_PRESET(dstr_t, patterns, DSTR_LIT("DITMv"));
    char* position = dstr_find(msg, &patterns, NULL, NULL);
    if(!position){
        ORIG(&e, E_RESPONSE, "minimum version not found");
    }

    // find the end of the minversion string
    size_t start = (uintptr_t)(position - msg->data) + patterns.data[0].len;
    dstr_t sub = dstr_sub(msg, start, 0);
    LIST_PRESET(dstr_t, patterns2, DSTR_LIT(" "));
    char* endpos = dstr_find(&sub, &patterns2, NULL, NULL);

    // make a substring of just the version number
    size_t end = (uintptr_t)(endpos - msg->data);
    dstr_t vstring = dstr_sub(msg, start, end);

    // split version string (maj.min.bld) on decimalas
    LIST_VAR(dstr_t, majminbld, 3);
    derr_t e2 = dstr_split(&vstring, &DSTR_LIT("."), &majminbld);
    CATCH(e2, E_FIXEDSIZE){
        // too many periods, version string is invalid
        TRACE(&e2, "invalid minversion string: %x\n", FD(&vstring));
        RETHROW(&e, &e2, E_VALUE);
    }else PROP(&e, e2);

    // convert each element to an integer
    PROP(&e, dstr_tou(&majminbld.data[0], maj, 10) );
    // for minor and build, assume 0 if not present
    if(majminbld.len > 1){
        PROP(&e, dstr_tou(&majminbld.data[1], min, 10) );
    }else{
        *min = 0;
    }
    if(majminbld.len > 2){
        PROP(&e, dstr_tou(&majminbld.data[2], bld, 10) );
    }else{
        *bld = 0;
    }

    return e;
}

derr_t ditm_new(ditm_t* ditm, ssl_context_t* ctx, connection_t* conn,
                const char* rhost, unsigned int rport, const char* dirpath,
                const char* api_host, unsigned int api_port){
    derr_t e = E_OK;

    /* the maildir, the keypair, and the keystate can't be
       allocated/initialized until we know the username.  Therefore we will
       zeroize everything so that in error situations we don't try to free
       an uninitialized pointer */
    memset(ditm, 0, sizeof(ditm_t));

    // store api_host and api_port which aren't used yet
    ditm->api_host = api_host;
    ditm->api_port = api_port;

    // allocate pop_client
    PROP(&e, pop_client_new(&ditm->pc) );

    // set pop_server hooks
    pop_server_hooks_t hooks = { loginhook, stathook, listhook,
                                 retrhook, delehook, rsethook,
                                 tophook, uidlhook, quithook };

    // define pop_server
    ditm->ps.conn = *conn;
    ditm->ps.hooks = hooks;

    // allocate deletions for the local store, controlled by ditm
    PROP_GO(&e, LIST_NEW(bool, &ditm->deletions, 256), cu_pc);

    // dirpath should wrap dirpath_buffer
    DSTR_WRAP_ARRAY(ditm->dirpath, ditm->dirpath_buffer);
    // copy the ditm_dir parameter to ditm struct
    PROP_GO(&e, FMT(&ditm->dirpath, "%x", FS(dirpath)), cu_deletions);

    /* since ditm_new() is only called after a connection is made,
       we can start connecting the pop_client right away */

    // get the server greeting
    bool status_ok;
    DSTR_VAR(message, 1024);
    IF_PROP(&e, pop_client_connect(&ditm->pc, ctx, rhost, rport, &status_ok, &message) ){
        // send response over pop_server before giving up
        /* This response should be positive so that Thunderbird doesn't freak out
           (it will handle the broken connection gracefully, but not -ERR) */
        DSTR_STATIC(response, "+OK error connecting to remote server\r\n");
        MERGE_CMD(&e, pop_server_send_dstr(&ditm->ps, &response), "alerting client");
        goto cu_deletions;
    }
    // if we got a -ERR in the server greeting... that's weird. but whatever.
    if(status_ok == false){
        TRACE_ORIG(&e, E_VALUE, "server greeted with error message");
        DSTR_STATIC(response, "-ERR remote server greeted with error message\r\n");
        MERGE_CMD(&e, pop_server_send_dstr(&ditm->ps, &response), "sending -ERR");
        goto cu_deletions;
    }

    // now parse the minversion out of the message
    unsigned int maj, min, bld;
    IF_PROP(&e, ditm_parse_minversion(&message, &maj, &min, &bld) ){
        DSTR_STATIC(response, "-ERR error parsing minimum version from server greeting\r\n");
        MERGE_CMD(&e, pop_server_send_dstr(&ditm->ps, &response), "alerting client");
        goto cu_deletions;
    }

    // initalize the conn_is_live boolean
    ditm->conn_is_live = true;

    // check version, taking into account possible float rounding errors
    ditm->minversion_met = DITM_VERSION_MAJOR >= maj &&
                           DITM_VERSION_MINOR >= min &&
                           DITM_VERSION_BUILD >= bld;

    // send the banner over the pop_server
    DSTR_STATIC(response, "+OK DITM ready.\r\n");
    PROP_GO(&e, pop_server_send_dstr(&ditm->ps, &response), cu_deletions);

    ditm->logged_in = false;

    return e;

cu_deletions:
    LIST_FREE(bool, &ditm->deletions);
cu_pc:
    pop_client_free(&ditm->pc);
    return e;
}

void ditm_free(ditm_t* ditm){
    // this should be allocated in loginhook
    maildir_free(&ditm->maildir);
    // free everything from ditm_new
    LIST_FREE(bool, &ditm->deletions);
    pop_client_free(&ditm->pc);
}

typedef enum {
    DITM_DS_UNINITIALIZED,
    DITM_DS_UNENCRYPTED,
    DITM_DS_DECRYPTED,
    DITM_DS_CORRUPTED,
    DITM_DS_NOT_FOR_ME,
} decryption_state_t;


/* thows: E_CONN
          E_INTERNAL
          E_RESPONSE (invalid server response)
          E_FS
          E_OS (reading or writing and already-opened file)
          E_NOMEM
          */
static derr_t ditm_download_new_message(ditm_t* ditm, key_tool_t* kt,
                                        ignore_list_t* il, const dstr_t* uid,
                                        size_t index){
    derr_t e = E_OK;
    derr_t e2;
    DSTR_VAR(message, 1024);
    bool status_ok;

    decryption_state_t state = DITM_DS_UNINITIALIZED;

    // don't do anything if we already have it
    if(in_list(uid, &ditm->maildir.uids, NULL)){
        return e;
    }

    // send RETR N to pop_server
    e2 = pop_client_retrieve(&ditm->pc, (unsigned int)index, &status_ok, &message);
    // message too long means bad server response
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP(&e, e2);

    // if status is ERR, quit here
    if(status_ok == false){
        ORIG(&e, E_RESPONSE, "RETR command failed");
    }

    /* open two temporary files.  One is for the raw file and the second
       is for the encrypted/mangled file, which we will keep */
    int t1fd, t2fd;
    size_t t1len = 0, t2len = 0;
    DSTR_VAR(t1path, 4096);
    DSTR_VAR(t2path, 4096);
    PROP(&e, maildir_new_tmp_file(&ditm->maildir, &t1path, &t1fd) );
    PROP_GO(&e, maildir_new_tmp_file(&ditm->maildir, &t2path, &t2fd), close_t1);

    // store the first few bytes in memory so we know about encryption
    DSTR_STATIC(enc_header, "-----BEGIN SPLINTERMAIL MESSAGE-----");
    DSTR_VAR(first_bytes, sizeof(enc_header_buffer) - 1);

    DSTR_VAR(buffer, POP_CLIENT_CHUNK_SIZE);

    // read the first chunk
    bool end;
    // a FIXEDSIZE error here indicates buffer is full; internal error
    NOFAIL_GO(&e, E_FIXEDSIZE,
            pop_client_get_body(&ditm->pc, &buffer, true, &end), close_t2);

    while(true){
        // fill up first_bytes if there's room
        size_t fb_left = first_bytes.size - first_bytes.len;
        if(fb_left > 0){
            dstr_t sub = dstr_sub(&buffer, 0, fb_left);
            // should never ever fail
            NOFAIL_GO(&e, E_ANY, dstr_append(&first_bytes, &sub), close_t2);
        }

        // write buffer to file
        PROP_GO(&e, dstr_write(t1fd, &buffer), close_t2);
        t1len += buffer.len;
        buffer.len = 0;

        // check if there's more to read
        if(end) break;

        // read another chunk
        // a FIXEDSIZE error here indicates buffer is full; internal error
        NOFAIL_GO(&e, E_FIXEDSIZE,
                pop_client_get_body(&ditm->pc, &buffer, false, &end), close_t2);
    }

    // reset temp file 1 for reading
    off_t oret = compat_lseek(t1fd, 0, SEEK_SET);
    if(oret == (off_t) -1){
        TRACE(&e, "%x: %x\n", FS("lseek"), FE(&errno));
        // I don't see why this would ever fail
        ORIG_GO(&e, E_INTERNAL, "lseek failed", close_t2);
    }

    // find out if the email was encrypted
    bool encrypted = (dstr_cmp(&first_bytes, &enc_header) == 0);
    // handle encrypted mail
    if(encrypted){
        e2 = key_tool.decrypt(kt, t1fd, t2fd, &t2len);
        CATCH(e2, E_NOT4ME){
            // message encrypted but not for us, we'll have to ignore it
            DROP_VAR(&e2);
            state = DITM_DS_NOT_FOR_ME;
            LOG_DEBUG("msg state: DITM_DS_NOT_FOR_ME\n");
            goto close_t2;
        }else CATCH(e2, E_PARAM){
            DROP_VAR(&e2);
            // broken message, mangle the body and hand it to the user
            // reset temp file 1 and temp file 2, we're going to try again
            oret = compat_lseek(t1fd, 0, SEEK_SET);
            if(oret == (off_t) -1){
                TRACE(&e, "%x: %x\n", FS("lseek"), FE(&errno));
                // I don't see why this would ever fail
                ORIG_GO(&e, E_INTERNAL, "lseek failed", close_t2);
            }
            oret = compat_lseek(t2fd, 0, SEEK_SET);
            if(oret == (off_t) -1){
                TRACE(&e, "%x: %x\n", FS("lseek"), FE(&errno));
                // I don't see why this would ever fail
                ORIG_GO(&e, E_INTERNAL, "lseek failed", close_t2);
            }
            PROP_GO(&e, ditm_mangle_corrupted(t1fd, t2fd, &t2len), close_t2);
            state = DITM_DS_CORRUPTED;
            LOG_DEBUG("msg state: DITM_DS_CORRUPTED\n");
            goto close_t2;
        }else PROP(&e, e2);
        // if we recieved no errors, then we are good
        state = DITM_DS_DECRYPTED;
        LOG_DEBUG("msg state: DITM_DS_DECRYPTED\n");
        goto close_t2;
    }
    // handle unencrypted mail
    else{
        PROP_GO(&e, ditm_mangle_unencrypted(t1fd, t2fd, &t2len), close_t2);
        state = DITM_DS_UNENCRYPTED;
        LOG_DEBUG("msg state: DITM_DS_UNENCRYPTED\n");
        // inform the key_tool we received something unencrypted
        kt->found_expired_peer = true;
        LOG_DEBUG("makring found_expired_key as true\n");
        goto close_t2;
    }

close_t2:
    compat_close(t2fd);
close_t1:
    compat_close(t1fd);

    // in all cases we are done with t1, so delete it
    int ret = remove(t1path.data);
    if(ret != 0){
        LOG_ERROR("%x: %x\n", FS(t1path.data), FE(&errno));
        LOG_ERROR("failed to cleanup temporary file\n");
    }

    // if we have an uncaught error, delete t2 and propagate
    if(is_error(e)){
        ret = remove(t2path.data);
        if(ret != 0){
            LOG_ERROR("%x: %x\n", FS(t2path.data), FE(&errno));
            LOG_ERROR("failed to cleanup temporary file\n");
        }
        return e;
    }

    // now handle the various outcomes
    switch(state){
    case DITM_DS_UNINITIALIZED:
        ORIG(&e, E_INTERNAL, "switching on state but state is not initialized");
    case DITM_DS_UNENCRYPTED:
        // save t2 (subject-mangled) to maildir
        PROP(&e, maildir_new_rename(&ditm->maildir, t2path.data, uid, t2len) );
        // indicate that we need to sync peer list
        kt->found_expired_peer = true;
        break;
    case DITM_DS_DECRYPTED:
        // save t2 (decrypted) to maildir
        PROP(&e, maildir_new_rename(&ditm->maildir, t2path.data, uid, t2len) );
        break;
    case DITM_DS_CORRUPTED:
        // save t2 (body-mangled) to maildir
        PROP(&e, maildir_new_rename(&ditm->maildir, t2path.data, uid, t2len) );
        break;
    case DITM_DS_NOT_FOR_ME:
        // add to ignore list
        PROP(&e, ignore_list_add(il, uid) );
        // delete t2
        ret = remove(t2path.data);
        if(ret != 0){
            LOG_ERROR("%x: %x\n", FS(t2path.data), FE(&errno));
            LOG_ERROR("failed to cleanup temporary file\n");
        }
        break;
    }
    return e;
}

/* read an unencrypted email on infd and write it to outfd with "NOT ENCRYPTED"
   in the subject line */
derr_t ditm_mangle_unencrypted(int infd, int outfd, size_t* outlen){
    derr_t e = E_OK;
    // subject line if none was provided:
    DSTR_STATIC(subj_entire, "Subject: NOT ENCRYPTED: (no subject)");
    // patterns for either either the subject line or end-of-headers
    LIST_PRESET(dstr_t, subj, DSTR_LIT("\nSubject:"),
                              DSTR_LIT("\r\n\r\n"),
                              DSTR_LIT("\n\n"));
    *outlen = 0;
    size_t amnt_read;
    DSTR_VAR(buffer, 4096);
    // we need to read until either the subject line or the body
    while(true){
        // read some stuff, E_FIXEDSIZE means we have an internal error
        NOFAIL(&e, E_FIXEDSIZE, dstr_read(infd, &buffer, 0, &amnt_read) );
        // break if necessary
        if(amnt_read == 0) break;
        // search for the patterns
        size_t which;
        size_t partial;
        char* pos = dstr_find(&buffer, &subj, &which, &partial);
        if(!pos){
            // if we found nothing, write all but the partial match
            dstr_t sub = dstr_sub2(buffer, 0, buffer.len - partial);
            PROP(&e, dstr_write(outfd, &sub) );
            *outlen += sub.len;
            dstr_leftshift(&buffer, buffer.len - partial);
            continue;
        }
        // if we found the end-of-headers but not the subject, insert fake subject
        if(which == 1 || which == 2){
            // if we didn't find a subject, insert one
            size_t headers_end = (uintptr_t)(pos - buffer.data);
            // write to the end of headers
            dstr_t pre = dstr_sub(&buffer, 0, headers_end);
            PROP(&e, dstr_write(outfd, &pre) );
            *outlen += pre.len;
            // get the native len break
            dstr_t nlb = dstr_sub(&subj.data[which],
                                  0, subj.data[which].len / 2);
            PROP(&e, dstr_write(outfd, &nlb) );
            *outlen += nlb.len;
            // write the missing subject line
            PROP(&e, dstr_write(outfd, &subj_entire) );
            *outlen += subj_entire.len;
            // write the rest of the buffer
            dstr_t post = dstr_sub(&buffer, headers_end, 0);
            PROP(&e, dstr_write(outfd, &post) );
            *outlen += post.len;
            buffer.len = 0;
            // now just dump the rest
            break;
        }
        // if we found the subject line, mangle it
        if(which == 0){
            size_t subj_end = (uintptr_t)(pos - buffer.data) + subj.data[which].len;
            // write to the end of "Subject:"
            dstr_t pre = dstr_sub2(buffer, 0, subj_end);
            PROP(&e, dstr_write(outfd, &pre) );
            *outlen += pre.len;
            // write the warning
            DSTR_STATIC(mangle, " NOT ENCRYPTED:");
            PROP(&e, dstr_write(outfd, &mangle) );
            *outlen += mangle.len;
            // write the rest of the buffer
            dstr_t post = dstr_sub(&buffer, subj_end, 0);
            PROP(&e, dstr_write(outfd, &post) );
            *outlen += post.len;
            buffer.len = 0;
            // we only expect one subject line, so just dump the rest
            break;
        }
    }
    // now just dump the rest of infd into outfd
    while(amnt_read){
        // read something
        NOFAIL(&e, E_FIXEDSIZE, dstr_read(infd, &buffer, 0, &amnt_read) );
        // break if necessary
        if(amnt_read == 0) break;
        // write something
        PROP(&e, dstr_write(outfd, &buffer) );
        *outlen += buffer.len;
        buffer.len = 0;
    }
    return e;
}

// take a corrupted message and prepend an explanation
derr_t ditm_mangle_corrupted(int infd, int outfd, size_t* outlen){
    derr_t e = E_OK;
    // get the current time
    time_t epoch = time(NULL);
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        TRACE(&e, "%x: %x\n", FS("localtime"), FE(&errno));
        ORIG(&e, E_INTERNAL, "error converting epoch time to time struct");
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t len;
    len = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(len == 0){
        TRACE(&e, "%x: %x\n", FS("strftime"), FE(&errno));
        ORIG(&e, E_INTERNAL, "error formatting time string");
    }
    // build the headers
    DSTR_VAR(buffer, 4096);
    // this should never fail
    NOFAIL(&e, E_FIXEDSIZE,
        FMT(&buffer, "From: DITM <ditm@localhost>\r\n"
                     "To: Local User <email_user@localhost>\r\n"
                     "Date: %x\r\n"
                     "Subject: DITM failed to decrypt message\r\n"
                     "\r\n"
                     "The following message appears to be corrupted"
                     " and cannot be decrypted:\r\n"
                     "\r\n", FS(d)) );

    // dump headers to message
    PROP(&e, dstr_write(outfd, &buffer) );
    *outlen = buffer.len;
    buffer.len = 0;

    // now read the entirety of the corrupted message from t1fd to t2fd
    while(true){
        size_t amnt_read;
        // read some stuff
        NOFAIL(&e, E_FIXEDSIZE, dstr_read(infd, &buffer, 0, &amnt_read) );
        // break if necessary
        if(amnt_read == 0) break;
        // write some stuff
        PROP(&e, dstr_write(outfd, &buffer) );
        *outlen += buffer.len;
        buffer.len = 0;
    }
    return e;
}

// inject a local-only message into the mailbox
derr_t ditm_inject_message(ditm_t* ditm, const dstr_t* subj, const dstr_t* msg){
    derr_t e = E_OK;
    // get the current time
    time_t epoch = time(NULL);
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        TRACE(&e, "%x: %x\n", FS("localtime"), FE(&errno));
        ORIG(&e, E_OS, "error converting epoch time to time struct");
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t len;
    len = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(len == 0){
        TRACE(&e, "%x: %x\n", FS("strftime"), FE(&errno));
        ORIG(&e, E_OS, "error formatting time string");
    }
    // build the headers
    DSTR_VAR(buffer, 4096);
    PROP(&e, FMT(&buffer, "From: DITM <ditm@localhost>\r\n"
                       "To: Local User <email_user@localhost>\r\n"
                       "Date: %x\r\n"
                       "Subject: %x\r\n"
                       "\r\n", FS(d), FD(subj)) );

    // prepare a UID
    DSTR_VAR(random, 16);
    DSTR_VAR(hex, 32);
    DSTR_VAR(uid, 40);
    PROP(&e, random_bytes(&random, random.size) );
    PROP(&e, bin2hex(&random, &hex) );
    PROP(&e, FMT(&uid, "LOCAL-%x", FD(&hex)) );

    // open a temp file
    DSTR_VAR(temp, 4096);
    int tfd;
    size_t tlen = 0; // clang wrongly complains this might be unint'd
    PROP(&e, maildir_new_tmp_file(&ditm->maildir, &temp, &tfd) );

    // dump headers to message
    PROP_GO(&e, dstr_write(tfd, &buffer), fail_tfd);
    tlen = buffer.len;

    // now write the body
    PROP_GO(&e, dstr_write(tfd, msg), fail_tfd);
    tlen += msg->len;

fail_tfd:
    compat_close(tfd);
    if(is_error(e)) goto fail_temp;

    // make sure we can append to ditm->deletions
    PROP_GO(&e, LIST_APPEND(bool, &ditm->deletions, false), fail_temp);

    // save the message
    PROP_GO(&e, maildir_new_rename(&ditm->maildir, temp.data, &uid, tlen), fail_deletions);

    return e;

fail_deletions:
    ditm->deletions.len -= 1;
fail_temp:
    remove(temp.data);
    return e;
}

static listener_t listener = {.bio=NULL};
static bool should_continue = true;

void ditm_signal_handler(int signum){
    (void) signum;
    //if(signum == SIGINT || signum == SIGTERM){
        LOG_ERROR("caught signal\n");
        // close the listener
        listener_close(&listener);
        // when we have threads, interrupt them here
#ifdef __APPLE__
        // this is not causing a failure in listener_accept on OSX
        // next best option is a hard exit
        exit(15);
#endif // __APPLE__
    //}
}

derr_t ditm_loop(const char* rhost, unsigned int rport,
                 const char* ditm_dir, unsigned int port,
                 const char* api_host, unsigned int api_port,
                 const char* certpath, const char* keypath){
    derr_t e = E_OK;
    derr_t e2;

    if(!dir_rw_access(ditm_dir, true)){
        ORIG(&e, E_FS, "failure to either create or access the ditm_dir");
    }

    // prepare server ssl context
    ssl_context_t s_ctx;
    bool server_ssl;
    {
        // grab default key and certificate if arguments weren't given
        DSTR_VAR(def_cert, 4096);
        if(!certpath){
            e2 = FMT(&def_cert, "%x/ditm-127.0.0.1-cert.pem", FS(ditm_dir));
            CATCH(e2, E_FIXEDSIZE){
                RETHROW(&e, &e2, E_FS);
            }else PROP(&e, e2);
        }
        DSTR_VAR(def_key, 4096);
        if(!keypath){
            e2 = FMT(&def_key, "%x/ditm-127.0.0.1-key.pem", FS(ditm_dir));
            CATCH(e2, E_FIXEDSIZE){
                RETHROW(&e, &e2, E_FS);
            }else PROP(&e, e2);
        }
        e2 = ssl_context_new_server(&s_ctx,
                certpath ? certpath : def_cert.data,
                keypath ? keypath : def_key.data, NULL);
        CATCH(e2, E_FS){
            // if the user manually specified cert or key, un-catch this error
            if(certpath || keypath){
                TRACE(&e2, "failure to set up SSL context, and user specified cert or key\n");
                RETHROW(&e, &e2, e2.type);
            }
            DROP_VAR(&e2);
            server_ssl = false;
        }else{
            PROP(&e, e2);
            server_ssl = true;
        }
    }
    // prepare client ssl context
    ssl_context_t c_ctx;
    PROP_GO(&e, ssl_context_new_client(&c_ctx), cleanup_1);

    // this variable is global
    //listener_t listener;
    if(server_ssl){
        PROP_GO(&e, listener_new_ssl(&listener, &s_ctx, "127.0.0.1", port), cleanup_2);
    }else{
        PROP_GO(&e, listener_new(&listener, "127.0.0.1", port), cleanup_2);
    }

    // prep the signal handler
    // struct sigaction sa;
    // memset(&sa, 0, sizeof(struct sigaction));
    // sa.sa_handler = ditm_signal_handler;
    // int ret = sigaction(SIGINT, &sa, NULL);
    // if(ret != 0){
    //     LOG_ERROR("%x: %x\n", FS("sigaction"), FE(&errno));
    //     ORIG(&e, E_OS, "unable to set signal handler");
    // }
    signal(SIGINT, ditm_signal_handler);
    signal(SIGTERM, ditm_signal_handler);

    while(should_continue){
        connection_t conn;
        // accept a connection
        PROP_GO(&e, listener_accept(&listener, &conn), cleanup_3);

        // create a ditm object for this connection
        ditm_t ditm;
        e2 = ditm_new(&ditm, &c_ctx, &conn, rhost, rport, ditm_dir,
                         api_host, api_port);
        CATCH(e2, E_ANY){
            connection_close(&conn);
            DUMP(e2);
            DROP_VAR(&e2);
            continue;
        }

        // set the pop_server to talk over this connection
        ditm.ps.conn = conn;

        // kick off server loop
        e2 = pop_server_loop(&ditm.ps, (void*)(&ditm));
        CATCH(e2, E_ANY){
            ditm_free(&ditm);
            connection_close(&conn);
            DUMP(e2);
            DROP_VAR(&e2);
            continue;
        }

        // done with ditm, cleanup
        connection_close(&conn);
        ditm_free(&ditm);
    }

cleanup_3:
    listener_close(&listener);
cleanup_2:
    ssl_context_free(&c_ctx);
cleanup_1:
    if(server_ssl) ssl_context_free(&s_ctx);
    return e;
}

derr_t ignore_list_load(ignore_list_t* il, const dstr_t* userdir){
    derr_t e = E_OK;
    // allocate the list, the backing memory, and *seen list
    PROP(&e, LIST_NEW(dstr_t, &il->list, 64) );
    PROP_GO(&e, dstr_new(&il->mem, 64*32), f_list);
    PROP_GO(&e, LIST_NEW(bool, &il->seen, 64), f_mem);
    // allocate to read a json file of unbounded length into memory
    dstr_t text;
    PROP_GO(&e, dstr_new(&text, 4096), f_seen);
    LIST(json_t) json;
    PROP_GO(&e, LIST_NEW(json_t, &json, 128), f_text);

    // after this point, failure mode means output an empty string

    // get the filename
    DSTR_VAR(path, 4096);
    derr_t e2 = FMT(&path, "%x/ignore.json", FD(userdir));
    // failing to generate the filename is not a recoverable error
    CATCH(e2, E_FIXEDSIZE){
        RETHROW_GO(&e, &e2, E_FS, f_text);
    }else PROP_GO(&e, e2, f_text);

    // read the file
    e2 = dstr_read_file(path.data, &text);
    // we can recover from not being able to open the file
    CATCH(e2, E_OPEN, E_OS){
        LOG_WARN("unable to load ignore.json\n");
        DROP_VAR(&e2);
        goto cu;
    }else{
        // but we can't recover if something else went wrong
        PROP_GO(&e, e2, f_text);
    }

    // parse the text
    e2 = json_parse(&json, &text);
    // we can recover from a bad json file
    CATCH(e2, E_PARAM){
        LOG_WARN("unable to parse ignore.json\n");
        DROP_VAR(&e2);
        goto cu;
    }else{
        // but we can't recover if something else went wrong
        PROP_GO(&e, e2, f_text);
    }

    // now make sure json root object is an array
    if(json.data[0].type != JSON_ARRAY){
        LOG_WARN("incorrect format of ignore.json\n");
        // if not we can recover from this
        goto cu;
    }

    // get the first element of the array
    json_t* elem = json.data[0].first_child;
    // loop through all elements
    while(elem){
        // pull the uid we should ignore out of json format
        dstr_t uid;
        PROP_GO(&e, j_to_dstr(*elem, &uid), cu);
        // and append it to ignore list
        PROP_GO(&e, list_append_with_mem(&il->list, &il->mem, uid, false),
                 f_text);
        // also append to *seen
        PROP_GO(&e, LIST_APPEND(bool, &il->seen, false), f_text);
        // go to next element
        elem = elem->next;
    }
cu:
    LIST_FREE(json_t, &json);
    dstr_free(&text);
    // if we got any error after allocating, just start with an empty list
    CATCH(e, E_ANY){
        il->list.len = 0;
        il->seen.len = 0;
        il->mem.len = 0;
        DROP_VAR(&e);
    }
    return e;

f_text:
    dstr_free(&text);
f_seen:
    LIST_FREE(bool, &il->seen);
f_mem:
    dstr_free(&il->mem);
f_list:
    LIST_FREE(dstr_t, &il->list);
    return e;
}

void ignore_list_free(ignore_list_t* il){
    LIST_FREE(bool, &il->seen);
    dstr_free(&il->mem);
    LIST_FREE(dstr_t, &il->list);
}

derr_t ignore_list_write(ignore_list_t* il, const dstr_t* userdir){
    derr_t e = E_OK;
    // get the filename
    DSTR_VAR(path, 4096);
    PROP(&e, FMT(&path, "%x/ignore.json", FD(userdir)) );
    // open the file for reading
    FILE* f = compat_fopen(path.data, "w");
    if(!f){
        TRACE(&e, "%x: %x\n", FS(path.data), FE(&errno));
        ORIG(&e, errno == ENOMEM ? E_NOMEM : E_FS, "unable to open ignore.json for writing");
    }

    DSTR_STATIC(start, "[");
    DSTR_STATIC(quote, "\"");
    DSTR_STATIC(separator, ",\n ");
    DSTR_STATIC(end, "]\n");

    // start the json array
    PROP_GO(&e, dstr_fwrite(f, &start), cleanup);

    bool one_written = false;

    for(size_t i = 0; i < il->list.len; i++){
        // only append this UID if it was seen
        if(il->seen.data[i]){
            // if we already wrote an element, write the separator
            if(one_written)
                PROP_GO(&e, dstr_fwrite(f, &separator), cleanup);
            // now write the UID to ignore
            PROP_GO(&e, dstr_fwrite(f, &quote), cleanup);
            PROP_GO(&e, dstr_fwrite(f, &il->list.data[i]), cleanup);
            PROP_GO(&e, dstr_fwrite(f, &quote), cleanup);
            one_written = true;
        }
    }

    // finish the json array
    PROP_GO(&e, dstr_fwrite(f, &end), cleanup);

cleanup:
    fclose(f);
    return e;
}

derr_t ignore_list_add(ignore_list_t* il, const dstr_t* uid){
    derr_t e = E_OK;
    PROP(&e, LIST_APPEND(bool, &il->seen, true) );
    PROP_GO(&e, list_append_with_mem(&il->list, &il->mem, *uid, false), fail);
    return e;

fail:
    il->seen.len--;
    return e;
}

bool ignore_list_should_ignore(ignore_list_t* il, const dstr_t* uid){
    size_t idx;
    if(in_list(uid, &il->list, &idx)){
        il->seen.data[idx] = true;
        return true;
    }
    return false;
}
