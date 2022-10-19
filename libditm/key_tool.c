#include <errno.h>

#include "libditm/libditm.h"
#include "api_client.h"

/* throws: E_NOMEM
           E_INTERNAL
           E_RESPONSE (server gave a bad response)
           E_PARAM (host, username, or password too long)
           E_SSL (bad server certificate)
           E_CONN (failed or broken connection with server) */
static derr_t key_tool_register_key(key_tool_t* kt,
                                    const char* host,
                                    unsigned int port,
                                    const dstr_t* user,
                                    const dstr_t* pass){
    derr_t e = E_OK;
    derr_t e2;
    LOG_INFO("key tool attempting to register key\n");
    // get ready to recieve JSON
    json_t json;
    JSON_PREP_PREALLOCATED(json, 4096, 32, true);

    // get ready for the api call
    DSTR_STATIC(command, "add_device");
    DSTR_VAR(argument, 4096);
    NOFAIL(&e, E_FIXEDSIZE, keypair_get_public_pem(kt->key, &argument) );
    int code;
    DSTR_VAR(reason, 1024);
    DSTR_VAR(recv, 4096);

    // do the api call
    e2 = api_password_call(host, port, &command, &argument,
                              user, pass, &code, &reason, &recv, &json);
    CATCH(e2, E_FIXEDSIZE){
        TRACE(&e2, "server response too long\n");
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP(&e, e2);

    // make sure the code was correct
    if(code < 200 || code > 299){
        ORIG(&e,
            E_RESPONSE,
            "API server responded with a non-200 HTTP code: %x %x",
            FI(code),
            FD(&reason)
        );
    }

    // read the response
    dstr_t status;
    json_ptr_t jcontents;
    bool ok, contents_ok;

    jspec_t *jspec = JOBJ(false,
        JKEYOPT("contents", &contents_ok, JPTR(&jcontents)),
        JKEY("status", JDREF(&status)),
    );

    DSTR_VAR(errbuf, 1024);
    e2 = jspec_read_ex(jspec, json.root, &ok, &errbuf);
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP(&e, e2);
    if(!ok){
        TRACE(&e, "%x", FD(&errbuf));
        ORIG(&e, E_RESPONSE, "invalid add_device response");
    }

    // now make sure that the request was a success
    if(!dstr_eq(status, DSTR_LIT("success"))){
        if(contents_ok){
            dstr_t dcontents;
            derr_t e2 = jspec_read_ex(JDREF(&dcontents), jcontents, &ok, NULL);
            if(is_error(e2)){
                DROP_VAR(&e2);
            }else if(ok){
                TRACE(&e, "server said: %x\n", FD(&dcontents));
            }
        }
        ORIG(&e, E_RESPONSE, "add_device failed");
    }

    return e;
}

static derr_t read_peer_list_item(jctx_t *ctx, size_t i, void *arg){
    derr_t e = E_OK;

    (void)i;
    key_tool_t *kt = (key_tool_t*)arg;

    dstr_t hexfpr;
    PROP(&e, jctx_read(ctx, JDREF(&hexfpr)) );
    if(!*ctx->ok) return e;

    // allocate new fingerprint
    dstr_t fpr;
    PROP(&e, dstr_new(&fpr, FL_FINGERPRINT) );
    // convert from binary
    PROP_GO(&e, hex2bin(&hexfpr, &fpr), fail_fpr);
    // append to list
    PROP_GO(&e, LIST_APPEND(dstr_t, &kt->peer_list, fpr), fail_fpr);

    return e;

fail_fpr:
    dstr_free(&fpr);
    return e;
}

static derr_t key_tool_peer_list_load(key_tool_t* kt, const char* filename){
    derr_t e = E_OK;
    derr_t e2;
    json_free(&kt->json);
    json_prep(&kt->json);
    kt->json_text.len = 0;
    // read the peer list file
    e2 = dstr_read_file(filename, &kt->json_text);
    CATCH(e2, E_OPEN, E_OS){
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // parse the peer list
    PROP(&e, json_parse(kt->json_text, &kt->json) );

    jspec_t *jspec = JLIST(read_peer_list_item, kt);
    bool ok;
    PROP(&e, jspec_read_ex(jspec, kt->json.root, &ok, NULL) );
    if(!ok){
        ORIG(&e, E_PARAM, "invalid peer list");
    }

    return e;
}

static derr_t key_tool_peer_list_write(key_tool_t* kt, const char* filename){
    derr_t e = E_OK;
    FILE* f = compat_fopen(filename, "w");
    if(!f){
        TRACE(&e, "%x: %x\n", FS(filename), FE(&errno));
        ORIG(&e, errno == ENOMEM ? E_NOMEM : E_FS, "unable to open peer_list file for writing");
    }

    DSTR_STATIC(start, "[\"");
    DSTR_STATIC(separator, "\",\n \"");
    DSTR_STATIC(end, "\"]\n");

    // start the json array
    PROP_GO(&e, dstr_fwrite(f, &start), cleanup);

    for(size_t i = 0; i < kt->peer_list.len; i++){
        // get a hex of the fingerprint
        DSTR_VAR(hexfpr, FL_FINGERPRINT * 2);
        // an E_FIXEDSIZE represents an internal error
        NOFAIL_GO(&e, E_FIXEDSIZE, bin2hex(&kt->peer_list.data[i], &hexfpr),
                cleanup);
        // write the hex fingerprint
        PROP_GO(&e, dstr_fwrite(f, &hexfpr), cleanup);
        // write the separator if necessary
        if(i + 1 < kt->peer_list.len){
            PROP_GO(&e, dstr_fwrite(f, &separator), cleanup);
        }
    }

    // finish the json array
    PROP_GO(&e, dstr_fwrite(f, &end), cleanup);

cleanup:
    fclose(f);
    return e;
}

static derr_t key_tool_new(key_tool_t* kt, const dstr_t* dir, int def_key_bits){
    derr_t e = E_OK;
    derr_t e2;
    // store the dir
    DSTR_WRAP_ARRAY(kt->dir, kt->dir_buffer);
    e2 = dstr_copy(dir, &kt->dir);
    // fixed-size error here means file-system error to higher code
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // try to load the key
    DSTR_VAR(temp_path, 4096);
    PROP(&e, FMT(&temp_path, "%x/device.pem", FD(dir)) );
    e2 = keypair_load(&kt->key, temp_path.data);
    // an E_OPEN means it is likely that the key doesn't exist
    // and an SSL error means we should just regenerate the key
    CATCH(e2, E_OPEN, E_SSL){
        DROP_VAR(&e2);
        // if we can't load the key, gen_key now and register it later
        LOG_WARN("Unable to load key, generating a new one\n");

        // generate the key
        e2 = gen_key(def_key_bits, temp_path.data);
        // E_OPEN here means E_FS to higher level code
        CATCH(e2, E_OPEN){
            RETHROW(&e, &e2, E_FS);
        }else PROP(&e, e2);
        LOG_WARN("key generated!\n");

        // now try and load the key
        // E_OPEN or E_SSL here represents an internal error
        NOFAIL(&e, ERROR_GROUP(E_OPEN, E_SSL), keypair_load(&kt->key, temp_path.data) );

        LOG_INFO("key tool generated a new key\n");
        kt->did_key_gen = true;
    }else{
        // propagate uncaught errors
        PROP(&e, e2);
        // or continue
        LOG_DEBUG("key tool loaded an old key\n");
        kt->did_key_gen = false;
    }

    // allocate the json_block
    PROP_GO(&e, dstr_new(&kt->json_text, 4096), fail_1);

    // prepare the json list, allow for dynamic memory
    json_prep(&kt->json);

    // allocate the peer_list
    PROP_GO(&e, LIST_NEW(dstr_t, &kt->peer_list, FL_DEVICES), fail_3);

    // now attempt to load the peer list
    temp_path.len = 0;
    e2 = FMT(&temp_path, "%x/peer_list.json", FD(dir));
    // E_FIXEDSIZE means a file system error
    CATCH(e2, E_FIXEDSIZE){
        RETHROW_GO(&e, &e2, E_FS, fail_4);
    }else PROP_GO(&e, e2, fail_4);

    e2 = key_tool_peer_list_load(kt, temp_path.data);
    // the list might not exist or it might be corrupted
    CATCH(e2, E_FS, E_PARAM){
        DROP_VAR(&e2);
        // if we can't load the list, set state to new
        kt->peer_list.len = 0;
        kt->peer_list_state = KT_PL_NEW;
    }else{
        PROP(&e, e2);
        kt->peer_list_state = KT_PL_OLD;
    }

    // allocate the new_peer_list
    PROP_GO(&e, LIST_NEW(dstr_t, &kt->new_peer_list, 32), fail_4);

    // indicate that we haven't found any expired peers yet
    kt->found_expired_peer = false;

    // allocate for the decrypter
    PROP_GO(&e, decrypter_new(&kt->dc), fail_5);

    return e;

fail_5:
    LIST_FREE(dstr_t, &kt->new_peer_list);
fail_4:
    // free all dstr_t's in peer_list
    for(size_t i = 0; i < kt->peer_list.len; i++){
        dstr_free(&kt->peer_list.data[i]);
    }
    // free peer_list
    LIST_FREE(dstr_t, &kt->peer_list);
fail_3:
    json_free(&kt->json);
// fail_2:
    dstr_free(&kt->json_text);
fail_1:
    keypair_free(&kt->key);
    return e;
}

static void key_tool_free(key_tool_t* kt){
    decrypter_free(&kt->dc);

    // free all dstr_t's in new_peer_list
    for(size_t i = 0; i < kt->new_peer_list.len; i++){
        dstr_free(&kt->new_peer_list.data[i]);
    }
    // free new_peer_list
    LIST_FREE(dstr_t, &kt->new_peer_list);

    // free all dstr_t's in peer_list
    for(size_t i = 0; i < kt->peer_list.len; i++){
        dstr_free(&kt->peer_list.data[i]);
    }
    // free peer_list
    LIST_FREE(dstr_t, &kt->peer_list);
    json_free(&kt->json);
    dstr_free(&kt->json_text);
    keypair_free(&kt->key);
}

/* throws: E_NOMEM
           E_PARAM (username, password, or host too long)
           E_INTERNAL
           E_CONN (failed or broken connection with host)
           E_SSL (server SSL certificate invalid)
           E_RESPONSE (bad response from server) */
static derr_t call_list_devices(key_tool_t* kt, const char* host,
                                unsigned int port, const dstr_t* user,
                                const dstr_t* pass){
    derr_t e = E_OK;
    derr_t e2;
    LOG_INFO("calling list devices\n");
    // get ready to recieve JSON
    json_free(&kt->json);
    json_prep(&kt->json);

    int code;
    DSTR_VAR(reason, 1024);
    kt->json_text.len = 0;

    // get ready for the api call
    DSTR_STATIC(command, "list_devices");

    // do the api call
    e2 = api_password_call(host, port, &command, NULL, user, pass,
                              &code, &reason, &kt->json_text, &kt->json);
    CATCH(e2, E_FIXEDSIZE){
        TRACE(&e2, "server response too long\n");
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP(&e, e2);

    // non-200-series responses are errors
    if(code < 200 || code > 299){
        ORIG(&e,
            E_RESPONSE,
            "API server responded with a non-200 HTTP code: %x %x",
            FI(code),
            FD(&reason)
        );
    }

    dstr_t status;
    json_ptr_t jcontents;
    bool ok, contents_ok;

    // parse out just status and contents for now
    jspec_t *jspec = JOBJ(true,
        JKEYOPT("contents", &contents_ok, JPTR(&jcontents)),
        JKEY("status", JDREF(&status)),
    );

    DSTR_VAR(errbuf, 1024);
    e2 = jspec_read_ex(jspec, kt->json.root, &ok, &errbuf);
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP(&e, e2);
    if(!ok){
        TRACE(&e, "%x", FD(&errbuf));
        ORIG(&e, E_RESPONSE, "invalid list_devices response");
    }

    if(!dstr_eq(status, DSTR_LIT("success"))){
        if(contents_ok){
            dstr_t dcontents;
            derr_t e2 = jspec_read_ex(JDREF(&dcontents), jcontents, &ok, NULL);
            if(is_error(e2)){
                DROP_VAR(&e2);
            }else if(ok){
                TRACE(&e, "server said: %x\n", FD(&dcontents));
            }
        }
        ORIG(&e, E_RESPONSE, "list_devices failed");
    }

    return e;
}

typedef struct {
    dstr_t *fpr_block;
    LIST(dstr_t) *srv_fprs;
} read_device_item_t;

// pull fingerprints out of json format (and convert to binary format)
static derr_t read_device_item(jctx_t *ctx, size_t i, void *data){
    derr_t e = E_OK;

    (void)i;
    read_device_item_t *arg = (read_device_item_t*)data;

    dstr_t hexfpr;
    PROP(&e, jctx_read(ctx, JDREF(&hexfpr)) );
    if(!*ctx->ok) return e;

    // verify the length of the fingerprint
    if(hexfpr.len != 2 * FL_FINGERPRINT){
        ORIG(&e, E_RESPONSE, "response contained a wrong-sized fingerprint");
    }
    // convert to binary binary fingerprint and add to list
    size_t start = arg->fpr_block->len;
    derr_t e2 = hex2bin(&hexfpr, arg->fpr_block);
    CATCH(e2, E_PARAM){
        // this means that there was a bad hex string from the server
        TRACE(&e2, "response contained bad hex string\n");
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP(&e, e2);
    dstr_t binfpr = dstr_sub(arg->fpr_block, start, 0);
    // verify the length of the binary fingerprint
    if(binfpr.len != FL_FINGERPRINT){
        ORIG(&e, E_RESPONSE, "response contained a wrong-sized fingerprint");
    }
    e2 = LIST_APPEND(dstr_t, arg->srv_fprs, binfpr);
    CATCH(e2, E_FIXEDSIZE){
        // this means there were more entries than was allowed
        TRACE(&e2, "too many fingerprints returned by list_devices\n");
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP(&e, e2);

    return e;
}

static derr_t key_tool_update(key_tool_t* kt, const char* host, unsigned int port,
                       const dstr_t* user, const dstr_t* pass){
    derr_t e = E_OK;
    derr_t e2;

    // under some specific conditions we can skip all of this
    if(kt->peer_list_state == KT_PL_OLD
            && kt->new_peer_list.len == 0
            && kt->found_expired_peer == false
            && kt->did_key_gen == false){
        LOG_DEBUG("skipping key_tool_update()\n");
        return e;
    }

    // get ready to pull fingerprints out of json format
    DSTR_VAR(fpr_block, FL_DEVICES * FL_FINGERPRINT);
    LIST_VAR(dstr_t, srv_fprs, FL_DEVICES);


    // check if we need to list devices
    bool our_key_missing_from_list_devices = false;
    if(kt->peer_list_state == KT_PL_NEW
            || kt->new_peer_list.len > 0
            || kt->found_expired_peer == true){

        // make call to list devices
        PROP(&e, call_list_devices(kt, host, port, user, pass) );

        // read device list
        read_device_item_t arg = {&fpr_block, &srv_fprs};
        jspec_t *jspec = JOBJ(true,
            JKEY("contents", JOBJ(true,
                JKEY("devices", JLIST(read_device_item, &arg)),
            )),
        );
        bool ok;
        DSTR_VAR(errbuf, 1024);
        PROP(&e, jspec_read_ex(jspec, kt->json.root, &ok, &errbuf) );
        if(!ok){
            TRACE(&e, "%x", FD(&errbuf));
            ORIG(&e, E_RESPONSE, "bad list_devices response");
        }

        // if there was a peer list on file, we need to check for new peers
        if(kt->peer_list_state == KT_PL_OLD){
            // for every device listed on the server...
            for(size_t i = 0; i < srv_fprs.len; i++){
                // ... if its not in the peer list...
                if(in_list(&srv_fprs.data[i], &kt->peer_list, NULL))
                    continue;
                // ... and its not already in new_peers...
                if(in_list(&srv_fprs.data[i], &kt->new_peer_list, NULL))
                    continue;
                // ... add it to new_peer_list.
                // allocate new fingerprint
                dstr_t fpr;
                PROP(&e, dstr_new(&fpr, FL_FINGERPRINT) );
                // copy the server's fingerprint to the new dstr_t
                PROP_GO(&e, dstr_copy(&srv_fprs.data[i], &fpr), fail_fpr);
                // append to list
                PROP_GO(&e, LIST_APPEND(dstr_t, &kt->new_peer_list, fpr), fail_fpr);
                continue;

            fail_fpr:
                dstr_free(&fpr);
                return e;
            }
        }

        // if we are using an old key, make sure it is in the list
        if(kt->did_key_gen == false){
            if(in_list(kt->key->fingerprint, &srv_fprs, NULL) == false){
                our_key_missing_from_list_devices = true;
            }
        }

        // now copy the server's list to our local list
        // (first empty the existing peer_list)
        for(size_t i = 0; i < kt->peer_list.len; i++){
            dstr_free(&kt->peer_list.data[i]);
        }
        kt->peer_list.len = 0;
        // then append each new fingerprint
        for(size_t i = 0; i < srv_fprs.len; i++){
            // allocate new fingerprint
            dstr_t fpr;
            PROP(&e, dstr_new(&fpr, FL_FINGERPRINT) );
            // copy the server's fingerprint to the new dstr_t
            PROP_GO(&e, dstr_copy(&srv_fprs.data[i], &fpr), fail_fpr2);
            // append to list
            PROP_GO(&e, LIST_APPEND(dstr_t, &kt->peer_list, fpr), fail_fpr2);
            continue;

        fail_fpr2:
            dstr_free(&fpr);
            return e;
        }
    }

    // check if we need to add_device
    if(kt->did_key_gen == true || our_key_missing_from_list_devices == true){
        // add the device
        PROP(&e, key_tool_register_key(kt, host, port, user, pass) );
    }

    /* now that we have checked the server's list of devices (if necessary)
       and registered our own key (if necessary), we need to update the peer
       list on file */

    // if our own key is not in the list, add it
    if(in_list(kt->key->fingerprint, &kt->peer_list, NULL) == false){
        // it has to be dynamically allocated for the kt->peer_list
        dstr_t fpr;
        PROP(&e, dstr_new(&fpr, FL_FINGERPRINT) );
        e2 = dstr_copy(kt->key->fingerprint, &fpr);
        if(e2.type){
            dstr_free(&fpr);
            PROP(&e, e2);
        }
        // append to list
        e2 = LIST_APPEND(dstr_t, &kt->peer_list, fpr);
        if(e2.type){
            dstr_free(&fpr);
            PROP(&e, e2);
        }
    }

    // get peer list filename
    DSTR_VAR(temp_path, 4096);
    e2 = FMT(&temp_path, "%x/peer_list.json", FD(&kt->dir));
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // and write peer list to file
    PROP(&e, key_tool_peer_list_write(kt, temp_path.data) );
    LOG_INFO("key tool update complete\n");
    return e;
}

static derr_t key_tool_check_recips(key_tool_t* kt, LIST(dstr_t)* recips){
    derr_t e = E_OK;

    // do nothing if we had no peer list on file
    if(kt->peer_list_state == KT_PL_NEW) return e;

    /* because we don't need to track the identity of expired peers, it should
       be sufficient to check if the list is shorter than ours and check if
       there's any recipients in *recips that are not in our list */

    if(kt->peer_list.len > recips->len){
        kt->found_expired_peer = true;
    }
    // PFMT("peers:\n");
    // for(size_t i = 0; i < kt->peer_list.len; i++){
    //     DSTR_VAR(hex, 2 * FL_FINGERPRINT);
    //     PROP(&e, bin2hex(&kt->peer_list.data[i], &hex) );
    //     PFMT("    %x\n", FD(&hex));
    // }
    // PFMT("recips:\n");
    // for(size_t i = 0; i < recips->len; i++){
    //     DSTR_VAR(hex, 2 * FL_FINGERPRINT);
    //     PROP(&e, bin2hex(&recips->data[i], &hex) );
    //     PFMT("    %x\n", FD(&hex));
    // }

    // for every recipient on message...
    for(size_t i = 0; i < recips->len; i++){
        dstr_t* recip = &recips->data[i];
        // ... if the recipient is not in the peerlist...
        if(!in_list(recip, &kt->peer_list, NULL)){
            // ... and it is not already listed in new_peer_list
            if(!in_list(recip, &kt->new_peer_list, NULL)){
                // ... then add it to new_peer_list
                dstr_t new_peer;
                PROP(&e, dstr_new(&new_peer, recip->len) );
                PROP_GO(&e, dstr_copy(recip, &new_peer), fail_new_peer);
                PROP_GO(&e, LIST_APPEND(dstr_t, &kt->new_peer_list, new_peer),
                        fail_recip);
            fail_recip:
                if(is_error(e)){
                    dstr_free(recip);
                }
            fail_new_peer:
                if(is_error(e)){
                    dstr_free(&new_peer);
                    return e;
                }
            }
        }
    }
    return e;
}

static derr_t key_tool_decrypt(key_tool_t* kt, int infd, int outfd, size_t* outlen){
    derr_t e = E_OK;
    derr_t e2;
    DSTR_VAR(recips_block, FL_DEVICES * FL_FINGERPRINT);
    LIST_VAR(dstr_t, recips, FL_DEVICES);
    PROP(&e, decrypter_start(&kt->dc, kt->key, &recips, &recips_block) );

    DSTR_VAR(inbuf, 4096);
    DSTR_VAR(outbuf, 4096 + FL_ENCRYPTION_BLOCK_SIZE);
    size_t amnt_read;
    *outlen = 0;
    while(true){
        // read from input, this should never result in E_FIXEDSIZE
        NOFAIL(&e, E_FIXEDSIZE, dstr_read(infd, &inbuf, 0, &amnt_read) );

        // break if we are done
        if(amnt_read == 0) break;

        // decrypt a chunk
        e2 = decrypter_update(&kt->dc, &inbuf, &outbuf);
        /* in one special error case the decryption fails but we know we have
           a complete list of recipients */
        if(e2.type == E_NOT4ME){
            MERGE_CMD(&e2, key_tool_check_recips(kt, &recips), "checking recips");
        }
        // if there was a FIXEDSIZE error that is our fault
        CATCH(e2, E_FIXEDSIZE){
            RETHROW(&e, &e2, E_INTERNAL);
        }
        // An SSL error is just a bad message, as far as higher code cares
        else CATCH(e2, E_SSL){
            RETHROW(&e, &e2, E_PARAM);
        }else PROP(&e, e2);

        // write to buffer
        PROP(&e, dstr_write(outfd, &outbuf) );
        *outlen += outbuf.len;
        outbuf.len = 0;

    }
    // decrypt the last chunk
    e2 = decrypter_finish(&kt->dc, &outbuf);
    // if there was a FIXEDSIZE error that is our fault
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_INTERNAL);
    }
    // An SSL error is just a bad message, as far as higher code cares
    else CATCH(e2, E_SSL){
        RETHROW(&e, &e2, E_PARAM);
    }else PROP(&e, e2);

    // write the last chunk
    PROP(&e, dstr_write(outfd, &outbuf) );
    *outlen += outbuf.len;
    outbuf.len = 0;
    PROP(&e, key_tool_check_recips(kt, &recips) );
    return e;
}

// non-test harness
key_tool_harness_t key_tool = {
    key_tool_new,
    key_tool_free,
    key_tool_update,
    key_tool_decrypt,
    key_tool_peer_list_load,
    key_tool_peer_list_write,
    key_tool_check_recips,
};
