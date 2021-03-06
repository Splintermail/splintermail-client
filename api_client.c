#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "common.h"
#include "logger.h"
#include "networking.h"
#include "json.h"
#include "crypto.h"
#include "api_client.h"

#include "win_compat.h"

void api_token_init(api_token_t* token){
    // wrap the buffer with the dstr
    DSTR_WRAP_ARRAY(token->secret, token->secret_buffer);
    token->key = 0;
    token->nonce = 0;
}

derr_t api_token_read(const char* path, api_token_t* token){
    api_token_init(token);

    // read the file into memory
    DSTR_VAR(creds, 1024);
    derr_t error = dstr_fread_file(path, &creds);
    // if we got a fixedsize error it is not a valid file
    CATCH(E_FIXEDSIZE){
        LOG_WARN("api credential file seems too long, ignoring\n");
        RETHROW(E_PARAM);
    }else CATCH(E_OPEN){
        RETHROW(E_FS);
    }else PROP(error);

    // try to parse the file contents as json
    // should just be token, secret, and nonce
    LIST_VAR(json_t, json, 32);

    error = json_parse(&json, &creds);
    // if we got a fixedsize error it is not a valid file
    CATCH(E_FIXEDSIZE){
        LOG_WARN("api creds contains way too much json\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    // now we can dereference things
    // we can let this E_PARAM just propagate
    PROP( jtou(jk(json.data[0], "token"), &token->key) );

    dstr_t secret_local;
    // we can let this E_PARAM just propagate
    PROP( j_to_dstr(jk(json.data[0], "secret"), &secret_local) );

    // we can let this E_PARAM just propagate
    PROP( jtoul(jk(json.data[0], "nonce"), &token->nonce) );

    // the file contents are in the wrong scope; need to do a copy operation
    error = dstr_copy(&secret_local, &token->secret);
    // a E_FIXEDSIZE error means the file wasn't valid to begin with
    CATCH(E_FIXEDSIZE){
        LOG_WARN("api secret is too long\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    return E_OK;
}

derr_t api_token_write(const char* path, api_token_t* token){
    // open the file for writing (with the new nonce)
    FILE* f = fopen(path, "w");
    if(!f){
        LOG_ERROR("%x: %x\n", FS(path), FE(&errno));
        ORIG(errno == ENOMEM ? E_NOMEM : E_FS, "unable to open API credentials for reading");
    }

    // write the json to the file
    derr_t error = FFMT(f, NULL, "{ \"token\"  : %x,\n"
                                 "  \"secret\" : \"%x\",\n"
                                 "  \"nonce\"  : %x }\n",
                                 FU(token->key),
                                 FD(&token->secret),
                                 FU(token->nonce));
    fclose(f);

    return error;
}

/* throws: E_NOMEM (creating BIO, or adding to *reason)
           E_FIXEDSIZE (adding to *reason or to *recv)
           E_PARAM (command or headers are too long, probably headers)
           E_INTERNAL
           E_PARAM (address longer than 256 bytes)
           E_CONN (failed or broken connection with host)
           E_SSL (server SSL certificate invalid)
           E_RESPONSE (bad response from server) */
static derr_t native_api_call(const char* host, unsigned int port,
                              dstr_t* command, dstr_t* req_body,
                              dstr_t *headers, int* code, dstr_t* reason,
                              dstr_t* recv, LIST(json_t)* json){
    derr_t error;

    // some useful variables
    LIST_PRESET(dstr_t, line_end, DSTR_LIT("\r\n"), DSTR_LIT("\n"));
    LIST_PRESET(dstr_t, space, DSTR_LIT(" "));
    // prepare to use SSL
    ssl_context_t ctx;
    PROP( ssl_context_new_client(&ctx) );

    // connect
    connection_t conn;
    PROP_GO( connection_new_ssl(&conn, &ctx, host, port), cleanup_1);

    // send the POST request
    DSTR_VAR(req, 4096);
    error = FMT(&req, "POST /api/%x HTTP/1.0\r\n"
                      "%x"
                      "Content-Length: %x\r\n"
                      "\r\n", FD(command), FD(headers), FU(req_body->len));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("command or headers are too long\n");
        RETHROW_GO(E_PARAM, cleanup_2);
    }else PROP_GO(error, cleanup_2);

    PROP_GO( connection_write(&conn, &req), cleanup_2);
    PROP_GO( connection_write(&conn, req_body), cleanup_2);

    DSTR_VAR(temp, 4096);
    // read until we have a new line
    char* endpos = NULL;
    size_t amnt_read = 0; // initializing for Clang, it is always set in loop
    while(!endpos){
        // read from the wire
        error = connection_read(&conn, &temp, &amnt_read);
        CATCH(E_FIXEDSIZE){
            LOG_ERROR("HTTP response first line way too long\n");
            RETHROW_GO(E_RESPONSE, cleanup_2);
        }else PROP_GO(error, cleanup_2);
        // make sure we didn't find the end of the request early
        if(amnt_read == 0){
            ORIG_GO(E_CONN, "HTTP connection closed early", cleanup_2);
        }
        endpos = dstr_find(&temp, &line_end, NULL, NULL);
        if(temp.len > temp.size / 2){
            // this line is way too long
            ORIG_GO(E_RESPONSE, "first line of HTTP response too long", cleanup_2);
        }
    }

    // find the begnning of the numeric code
    char* codepos = dstr_find(&temp, &space, NULL, NULL);
    if(!codepos) ORIG_GO(E_VALUE, "failed parsing HTTP response", cleanup_2);

    // find the beginning of the reason
    dstr_t sub = dstr_sub(&temp, (uintptr_t)(codepos - temp.data) + 1, 0);
    char* reasonpos = dstr_find(&sub, &space, NULL, NULL);
    if(!reasonpos) ORIG_GO(E_VALUE, "failed parsing HTTP response", cleanup_2);

    // populate *code
    sub = dstr_sub(&temp, (uintptr_t)(codepos - temp.data) + 1,
                   (uintptr_t)(reasonpos - temp.data));
    error = dstr_toi(&sub, code);
    CATCH(E_PARAM){
        RETHROW_GO(E_RESPONSE, cleanup_2);
    }else PROP_GO(error, cleanup_2);

    // populate *reason
    sub = dstr_sub(&temp, (uintptr_t)(reasonpos - temp.data) + 1,
                   (uintptr_t)(endpos - temp.data));
    PROP_GO( dstr_copy(&sub, reason), cleanup_2);
    PROP_GO( dstr_null_terminate(reason), cleanup_2);

    // now find the end of the headers
    LIST_PRESET(dstr_t, headers_end, DSTR_LIT("\r\n\r\n"), DSTR_LIT("\n\n"));

    size_t which, partial;
    endpos = dstr_find(&temp, &headers_end, &which, &partial);
    while(!endpos){
        // remove everything that we know is headers
        dstr_leftshift(&temp, temp.len - partial);
        // read more in
        PROP_GO( connection_read(&conn, &temp, &amnt_read), cleanup_2);
        CATCH(E_FIXEDSIZE){
            LOG_ERROR("HTTP headers too long for buffer\n");
            RETHROW_GO(E_RESPONSE, cleanup_2);
        }else PROP_GO(error, cleanup_2);
        // do another search
        endpos = dstr_find(&temp, &headers_end, &which, &partial);
        if(!endpos && amnt_read == 0){
            ORIG_GO(E_VALUE, "HTTP closed connection before end of headers", cleanup_2);
        }
    }

    // append any body in temp to *recv
    sub = dstr_sub(&temp, (uintptr_t)(endpos - temp.data)
                   + headers_end.data[which].len, 0);
    PROP_GO( dstr_append(recv, &sub), cleanup_2);

    // read to the end, directly into *recv
    while(amnt_read){
        PROP_GO( connection_read(&conn, recv, &amnt_read), cleanup_2);
    }

    // now if the reason code was 2xx, parse the json
    if(*code >= 200 && *code < 300){
        error = json_parse(json, recv);
        CATCH(E_PARAM){
            RETHROW_GO(E_RESPONSE, cleanup_2);
        }else PROP_GO(error, cleanup_2);
    }

cleanup_2:
    connection_close(&conn);
cleanup_1:
    ssl_context_free(&ctx);

    return error;
}


derr_t api_password_call(const char* host, unsigned int port, dstr_t* command,
                         dstr_t* arg, const dstr_t* username,
                         const dstr_t* password, int* code, dstr_t* reason,
                         dstr_t* recv, LIST(json_t)* json){
    derr_t error;

    // first build json string out of the command and arg
    DSTR_VAR(body, 4096);
    if(arg){
        error = FMT(&body, "{\"path\":\"/api/%x\", \"arg\":\"%x\"}", FD(command), FD_DBG(arg));
    }else{
        error = FMT(&body, "{\"path\":\"/api/%x\", \"arg\":null}", FD(command));
    }
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("arg or command too long\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    // then base64-encode that string
    /* this step is purely to make server-side code easier, because libraries
       which autoparse the json value in the post body make the signing process
       undeterministic */
    DSTR_VAR(payload, 8192);
    error = bin2b64(&body, &payload, 0, true);
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    // build the authorization
    DSTR_VAR(user_pass, 256);
    error = FMT(&user_pass, "%x:%x", FD(username), FD(password));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("username or password too long\n");
        RETHROW(E_INTERNAL);
    }else PROP(error);

    DSTR_VAR(up_b64, 384);
    error = bin2b64(&user_pass, &up_b64, 0, true);
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    DSTR_VAR(headers, 512);
    error = FMT(&headers, "Authorization: Basic %x\r\n", FD(&up_b64));
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    // make the API request
    PROP( native_api_call(host, port, command, &payload, &headers,
                          code, reason, recv, json) );

    return E_OK;
}

derr_t api_token_call(const char* host, unsigned int port, dstr_t* command,
                      dstr_t* arg, api_token_t* token, int* code,
                      dstr_t* reason, dstr_t* recv, LIST(json_t)* json){
    derr_t error;

    // first build json string out of the command and arg
    DSTR_VAR(body, 4096);
    if(arg){
        error = FMT(&body, "{\"path\":\"/api/%x\", \"arg\":\"%x\", \"nonce\":%x}",
                           FD(command), FD_DBG(arg), FU(token->nonce));
    }else{
        error = FMT(&body, "{\"path\":\"/api/%x\", \"arg\":null, \"nonce\":%x}",
                           FD(command), FU(token->nonce));
    }
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("arg or command too long\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    // then base64-encode that string
    /* this step is purely to make server-side code easier, because libraries
       which autoparse the json value in the post body make the signing process
       undeterministic */
    DSTR_VAR(payload, 8192);
    error = bin2b64(&body, &payload, 0, true);
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    // sign the request
    DSTR_VAR(signature, 128);
    DSTR_VAR(hexsig, 256);
    error = hmac(&token->secret, &payload, &signature);
    // token->secret shouldn't be too long, signature shouldn't be too short
    CATCH(E_PARAM | E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    // convert signature to hex
    error = bin2hex(&signature, &hexsig);
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    // build the authorization hedaers
    DSTR_VAR(key_header, 256);
    error = FMT(&key_header, "X-AUTH-TOKEN: %x", FU(token->key));
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    DSTR_VAR(signature_header, 512);
    error = FMT(&signature_header, "X-AUTH-SIGNATURE: %x", FD(&hexsig));
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);

    // use native http requests
    DSTR_VAR(headers, 1024);
    error = FMT(&headers, "%x\r\n%x\r\n", FD(&key_header), FD(&signature_header));
    CATCH(E_FIXEDSIZE){
        RETHROW(E_INTERNAL);
    }else PROP(error);
    // PFMT("payload %x\n", FD(&payload));
    // PFMT("headers %x\n", FD(&headers));
    // PFMT("command %x\n", FD(command));
    PROP( native_api_call(host, port, command, &payload, &headers,
                          code, reason, recv, json) );

    return E_OK;
}


derr_t register_api_token(const char* host,
                          unsigned int port,
                          const dstr_t* user,
                          const dstr_t* pass,
                          const char* creds_path){
    derr_t error;
    LOG_INFO("attempting to register a new token\n");
    // get ready to recieve JSON
    LIST_VAR(json_t, json, 32);

    // get ready for the api call
    DSTR_STATIC(command, "add_token");
    int code;
    DSTR_VAR(reason, 1024);
    DSTR_VAR(recv, 4096);

    // do the api call
    error = api_password_call(host, port, &command, NULL,
                              user, pass, &code, &reason, &recv, &json);
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("server response too long\n");
        RETHROW(E_RESPONSE);
    }else PROP(error);

    // make sure the code was correct
    if(code < 200 || code > 299){
        LOG_ERROR("API server responded with a non-200 HTTP code\n");
        ORIG(E_RESPONSE, reason.data);
    }

    // dereference the status out of the json response
    dstr_t status;
    error = j_to_dstr(jk(json.data[0], "status"), &status);
    CATCH(E_PARAM){
        RETHROW(E_RESPONSE);
    }else PROP(error);
    // now make sure that the request was a success
    DSTR_STATIC(success, "success");
    int result = dstr_cmp(&success, &status);
    if(result != 0){
        dstr_t contents;
        error = j_to_dstr(jk(json.data[0], "contents"), &contents);
        CATCH(E_ANY){
            // ignore errors here
            error = E_OK;
        }else{
            LOG_ERROR("server said: %x\n", FD(&contents));
        }
        ORIG(E_RESPONSE, "add_device failed");
    }

    // now we need to pull out the contents/{token, secret}
    api_token_t token;
    api_token_init(&token);
    json_t contents = jk(json.data[0], "contents");
    // grab the token identifier from the json response
    error = jtou(jk(contents, "token"), &token.key);
    CATCH(E_PARAM){
        RETHROW(E_RESPONSE);
    }else PROP(error);
    // derefernce the secret from the json response
    dstr_t local_secret;
    error = j_to_dstr(jk(contents, "secret"), &local_secret);
    CATCH(E_PARAM){
        RETHROW(E_RESPONSE);
    }else PROP(error);
    // copy the secret into the api_token_t object
    error = dstr_copy(&local_secret, &token.secret);
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("server responded with a too-long token secret\n");
        RETHROW(E_RESPONSE);
    }else PROP(error);

    // nonce has to be at least 1, since the nonce starts at 0 on the server
    token.nonce = 1;

    // now write the token to a file
    PROP( api_token_write(creds_path, &token) );

    LOG_INFO("sucessfully registered API token\n");
    return E_OK;
}
