#include <string.h>

#include "networking.h"
#include "logger.h"
#include "pop_server.h"

#define BUFFER_SIZE 4096

derr_t pop_server_send_dstr(pop_server_t* ps, const dstr_t* buffer){
    PROP( connection_write(&ps->conn, buffer) );

    LOG_DEBUG("pop_server wrote: %x", FD(buffer));

    return E_OK;
}

derr_t pop_server_loop(pop_server_t* ps, void* arg){
    derr_t error;
    // static strings for comparisons
    DSTR_STATIC(USER, "USER");
    DSTR_STATIC(PASS, "PASS");
    DSTR_STATIC(STLS, "STLS");
    DSTR_STATIC(CAPA, "CAPA");
    DSTR_STATIC(STAT, "STAT");
    DSTR_STATIC(LIST, "LIST");
    DSTR_STATIC(RETR, "RETR");
    DSTR_STATIC(DELE, "DELE");
    DSTR_STATIC(RSET, "RSET");
    DSTR_STATIC(NOOP, "NOOP");
    DSTR_STATIC(TOP,  "TOP");
    DSTR_STATIC(UIDL, "UIDL");
    DSTR_STATIC(QUIT, "QUIT");

    // static strings for common response messages
    DSTR_STATIC(resp_ok, "+OK\r\n");
    DSTR_STATIC(resp_0_arg, "-ERR command requires exactly 0 arguments.\r\n");
    DSTR_STATIC(resp_1_arg, "-ERR command requires exactly 1 argument.\r\n");
    DSTR_STATIC(resp_2_arg, "-ERR command requires exactly 2 arguments.\r\n");
    DSTR_STATIC(resp_0_or_1_arg, "-ERR command requires 0 or 1 arguments.\r\n");
    DSTR_STATIC(resp_bad_arg, "-ERR bad argument.\r\n");
    DSTR_STATIC(resp_only_auth, "-ERR command only allowed in AUTHORIZATION state.\r\n");
    DSTR_STATIC(resp_only_trans, "-ERR command only allowed in TRANSACTION state.\r\n");

    bool state = POP_SERVER_STATE_AUTH;
    bool pass_ready = false;
    bool starttls = false;
    bool can_ssl = false;
    DSTR_VAR(username, BUFFER_SIZE);
    DSTR_VAR(buffer, BUFFER_SIZE);

    while(true){
        // before reading, check if we have an end-of-line already in the buffer
        LIST_PRESET(dstr_t, patterns, DSTR_LIT("\r\n"));
        char* pos = dstr_find(&buffer, &patterns, NULL, NULL);
        if(!pos){
            size_t new_text_start = buffer.len;
            PROP( connection_read(&ps->conn, &buffer, NULL) );
            dstr_t sub = dstr_sub(&buffer, new_text_start, 0);
            LOG_DEBUG("pop_server read: %x", FD(&sub));
            // find the end of the line
            pos = dstr_find(&buffer, &patterns, NULL, NULL);
            if(!pos){
                // if buffer is more than half full, command is way too long to be valid
                if(buffer.size - buffer.len < buffer.size / 2){
                    // throw away what we have recieved
                    buffer.len = 0;
                    DSTR_STATIC(response, "-ERR line too long.\r\n");
                    PROP( pop_server_send_dstr(ps, &response) );
                }
                // try and read again
                continue;
            }
        }

        // pull first line out of the buffer
        dstr_t buffer_sub = dstr_sub(&buffer, 0, (uintptr_t)(pos - buffer.data));
        DSTR_VAR(line, BUFFER_SIZE);
        PROP( dstr_copy(&buffer_sub, &line) );
        dstr_leftshift(&buffer, line.len + 2);

        LIST_VAR(dstr_t, tokens, 8);
        // check for PASS command (spaces are handled differently)
        DSTR_STATIC(pass, "PASS ");
        // copy up to the first 5 characters of the line to a buffer for dstr_upper()
        dstr_t sub = dstr_sub(&line, 0, 5);
        DSTR_VAR(start, 5);
        PROP( dstr_copy(&sub, &start) );
        // dstr_upper() before comparison
        dstr_upper(&start);
        int match = dstr_cmp(&start, &pass);
        if(match == 0){
            // match == 0 means it is a PASS command
            // so append the PASS token
            DSTR_STATIC(pass_nospace, "PASS");
            PROP( LIST_APPEND(dstr_t, &tokens, pass_nospace) );
            // then append the rest of the line as a single token
            dstr_t password = dstr_sub(&line, 5, 0);
            PROP( LIST_APPEND(dstr_t, &tokens, password) );
        }else{
            // if this is not PASS command, just split the string normally
            DSTR_STATIC(space, " ");
            error = dstr_split(&line, &space, &tokens);
            CATCH(E_FIXEDSIZE){
                // there should never be more than like 3 tokens in a line
                DSTR_STATIC(response, "-ERR Too many tokens in command.\r\n");
                PROP( pop_server_send_dstr(ps, &response) );
                continue;
            }else{
                PROP(error);
            }
        }

        // make sure we got at least one token
        if(tokens.len < 1){
            DSTR_STATIC(response, "-ERR Empty command.\r\n");
            PROP( pop_server_send_dstr(ps, &response) );
            continue;
        }

        // get ready to parse!

        // copy the command to a buffer for dstr_upper()
        DSTR_VAR(command, 128);
        error = dstr_copy(&tokens.data[0], &command);
        CATCH(E_FIXEDSIZE){
            // command should never even be longer than 4 characters
            DSTR_STATIC(response, "-ERR Command too long.\r\n");
            PROP( pop_server_send_dstr(ps, &response) );
            continue;
        }else{
            PROP(error);
        }

        // change command to upper case
        dstr_upper(&command);
        size_t nargs = tokens.len - 1;

        // for dstr_cmp()
        int result;

        // USER
        result = dstr_cmp(&command, &USER);
        if(result == 0){
            if(state != POP_SERVER_STATE_AUTH){
                PROP( pop_server_send_dstr(ps, &resp_only_auth) );
                continue;
            }
            if(nargs != 1){
                PROP( pop_server_send_dstr(ps, &resp_1_arg) );
                continue;
            }
            // copy username to the username buffer
            PROP( dstr_copy(&tokens.data[1], &username) );
            pass_ready = true;
            PROP( pop_server_send_dstr(ps, &resp_ok) );
            continue;
        }
        // PASS
        result = dstr_cmp(&command, &PASS);
        if(result == 0){
            if(state != POP_SERVER_STATE_AUTH){
                PROP( pop_server_send_dstr(ps, &resp_only_auth) );
                continue;
            }
            if(nargs != 1){
                PROP( pop_server_send_dstr(ps, &resp_1_arg) );
                continue;
            }
            if(pass_ready == false){
                DSTR_STATIC(response, "-ERR PASS must follow successful USER command.\r\n");
                PROP( pop_server_send_dstr(ps, &response) );
                continue;
            }
            pass_ready = false;
            // call hook
            bool login_ok;
            error = ps->hooks.login(arg, &username, &tokens.data[1],
                                           &login_ok);
            /* even if we threw an error, we *always* want to let the email
               client know that the password was correct so that the user isn't
               bothered by password reset prompts */
            if(login_ok == true){
                DSTR_STATIC(response, "+OK Logged in.\r\n");
                PROP( pop_server_send_dstr(ps, &response) );
                state = POP_SERVER_STATE_TRANS;
            }
            // now we can propagate any uncaught error that was thrown
            PROP(error);
            // if we didn't throw an error we know creds are bad
            if(login_ok == false){
                DSTR_STATIC(response, "-ERR Bad login credentials.\r\n");
                PROP( pop_server_send_dstr(ps, &response) );
            }
            continue;
        }
        // STLS
        result = dstr_cmp(&command, &STLS);
        if(result == 0){
            if(nargs != 0){
                PROP( pop_server_send_dstr(ps, &resp_0_arg) );
                continue;
            }
            DSTR_STATIC(response, "-ERR STARTTLS not yet implemented.\r\n");
            PROP( pop_server_send_dstr(ps, &response) );
            continue;
        }
        // CAPA
        result = dstr_cmp(&command, &CAPA);
        if(result == 0){
            if(nargs != 0){
                PROP( pop_server_send_dstr(ps, &resp_0_arg) );
                continue;
            }
            if(starttls == true || can_ssl == false){
                DSTR_STATIC(response, "+OK Capability list follows.\r\n"
                                      "USER\r\nTOP\r\nUIDL\r\n.\r\n");
                PROP( pop_server_send_dstr(ps, &response) );
            }else{
                DSTR_STATIC(response, "+OK Capability list follows.\r\n"
                                      "USER\r\nTOP\r\nUIDL\r\nSTLS\r\n.\r\n");
                PROP( pop_server_send_dstr(ps, &response) );
            }
            continue;
        }
        // STAT
        result = dstr_cmp(&command, &STAT);
        if(result == 0){
            if(state != POP_SERVER_STATE_TRANS){
                PROP( pop_server_send_dstr(ps, &resp_only_trans) );
                continue;
            }
            if(nargs != 0){
                PROP( pop_server_send_dstr(ps, &resp_0_arg) );
                continue;
            }
            // call hook
            PROP( ps->hooks.stat(arg) );
            continue;
        }
        // LIST
        result = dstr_cmp(&command, &LIST);
        if(result == 0){
            if(state != POP_SERVER_STATE_TRANS){
                PROP( pop_server_send_dstr(ps, &resp_only_trans) );
                continue;
            }
            if(nargs != 0 && nargs != 1){
                PROP( pop_server_send_dstr(ps, &resp_0_or_1_arg) );
                continue;
            }
            if(nargs == 1){
                int index;
                error = dstr_toi(&tokens.data[1], &index);
                CATCH(E_ANY){
                    PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                    continue;
                }
                if(index < 1){
                    PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                    continue;
                }
                // call hook with 1 argument
                PROP( ps->hooks.list(arg, index) );
            }else{
                // call hook with no argument
                PROP( ps->hooks.list(arg, -1) );
            }
            continue;
        }
        // RETR
        result = dstr_cmp(&command, &RETR);
        if(result == 0){
            if(state != POP_SERVER_STATE_TRANS){
                PROP( pop_server_send_dstr(ps, &resp_only_trans) );
                continue;
            }
            if(nargs != 1){
                PROP( pop_server_send_dstr(ps, &resp_1_arg) );
                continue;
            }
            unsigned int index;
            error = dstr_tou(&tokens.data[1], &index);
            CATCH(E_ANY){
                PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                continue;
            }
            if(index < 1){
                PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                continue;
            }
            // call hook
            PROP( ps->hooks.retr(arg, index) );
            continue;
        }
        // DELE
        result = dstr_cmp(&command, &DELE);
        if(result == 0){
            if(state != POP_SERVER_STATE_TRANS){
                PROP( pop_server_send_dstr(ps, &resp_only_trans) );
                continue;
            }
            if(nargs != 1){
                PROP( pop_server_send_dstr(ps, &resp_1_arg) );
                continue;
            }
            unsigned int index;
            error = dstr_tou(&tokens.data[1], &index);
            CATCH(E_ANY){
                PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                continue;
            }
            if(index < 1){
                PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                continue;
            }
            // call hook
            PROP( ps->hooks.dele(arg, index) );
            continue;
        }
        // RSET
        result = dstr_cmp(&command, &RSET);
        if(result == 0){
            if(state != POP_SERVER_STATE_TRANS){
                PROP( pop_server_send_dstr(ps, &resp_only_trans) );
                continue;
            }
            if(nargs != 0){
                PROP( pop_server_send_dstr(ps, &resp_0_arg) );
                continue;
            }
            // call hook
            PROP( ps->hooks.rset(arg) );
            continue;
        }
        // NOOP
        result = dstr_cmp(&command, &NOOP);
        if(result == 0){
            if(nargs != 0){
                PROP( pop_server_send_dstr(ps, &resp_0_arg) );
                continue;
            }
            PROP( pop_server_send_dstr(ps, &resp_ok) );
            continue;
        }
        // TOP
        result = dstr_cmp(&command, &TOP);
        if(result == 0){
            if(state != POP_SERVER_STATE_TRANS){
                PROP( pop_server_send_dstr(ps, &resp_only_trans) );
                continue;
            }
            if(nargs != 2){
                PROP( pop_server_send_dstr(ps, &resp_2_arg) );
                continue;
            }
            // convert first arg to integer
            unsigned int index;
            error = dstr_tou(&tokens.data[1], &index);
            CATCH(E_ANY){
                PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                continue;
            }
            if(index < 1){
                PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                continue;
            }
            // convert second arg to integer
            unsigned int lines;
            error = dstr_tou(&tokens.data[2], &lines);
            CATCH(E_ANY){
                PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                continue;
            }
            // call hook
            PROP( ps->hooks.top(arg, index, lines) );
            continue;
        }
        // UIDL
        result = dstr_cmp(&command, &UIDL);
        if(result == 0){
            if(state != POP_SERVER_STATE_TRANS){
                PROP( pop_server_send_dstr(ps, &resp_only_trans) );
                continue;
            }
            if(nargs != 0 && nargs != 1){
                PROP( pop_server_send_dstr(ps, &resp_0_or_1_arg) );
                continue;
            }
            if(nargs == 1){
                int index;
                error = dstr_toi(&tokens.data[1], &index);
                CATCH(E_ANY){
                    PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                    continue;
                }
                if(index < 1){
                    PROP( pop_server_send_dstr(ps, &resp_bad_arg) );
                    continue;
                }
                // call hook with 1 argument
                PROP( ps->hooks.uidl(arg, index) );
            }else{
                // call hook with no argument
                PROP( ps->hooks.uidl(arg, -1) );
            }
            continue;
        }
        // QUIT
        result = dstr_cmp(&command, &QUIT);
        if(result == 0){
            DSTR_STATIC(response, "+OK Goodbye, my love.\r\n");
            DSTR_STATIC(fail, "-ERR Not all messages marked for deletion got deleted\r\n");
            // call hook
            bool update_ok;
            PROP( ps->hooks.quit(arg, &update_ok) );
            if(update_ok){
                PROP( pop_server_send_dstr(ps, &response) );
            }else{
                PROP( pop_server_send_dstr(ps, &fail) );
            }
            break;
        }
        // if we are here, we didn't recognize the command
        DSTR_STATIC(response, "-ERR Unrecognized command.\r\n");
        PROP( pop_server_send_dstr(ps, &response) );

    } // end of while loop

    return E_OK;
}
