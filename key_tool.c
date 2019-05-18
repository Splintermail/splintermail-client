#include <errno.h>

#include "key_tool.h"
#include "logger.h"

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
    LOG_INFO("key tool attempting to register key\n");
    // get ready to recieve JSON
    LIST_VAR(json_t, json, 32);

    // get ready for the api call
    DSTR_STATIC(command, "add_device");
    DSTR_VAR(argument, 4096);
    e = keypair_get_public_pem(&kt->key, &argument);
    CATCH(e, E_FIXEDSIZE){
        RETHROW(e, E_INTERNAL);
    }else PROP(e, e);
    int code;
    DSTR_VAR(reason, 1024);
    DSTR_VAR(recv, 4096);

    // do the api call
    e = api_password_call(host, port, &command, &argument,
                              user, pass, &code, &reason, &recv, &json);
    CATCH(e, E_FIXEDSIZE){
        TRACE(e, "server response too long\n");
        RETHROW(e, E_RESPONSE);
    }else PROP(e, e);

    // make sure the code was correct
    if(code < 200 || code > 299){
        TRACE(e, "API server responded with a non-200 HTTP code\n");
        ORIG(e, E_RESPONSE, reason.data);
    }

    // dereference the status out of the json response
    dstr_t status;
    e = j_to_dstr(jk(json.data[0], "status"), &status);
    CATCH(e, E_PARAM){
        RETHROW(e, E_RESPONSE);
    }else PROP(e, e);
    // now make sure that the request was a success
    DSTR_STATIC(success, "success");
    int result = dstr_cmp(&success, &status);
    if(result != 0){
        dstr_t contents;
        e = j_to_dstr(jk(json.data[0], "contents"), &contents);
        CATCH(e, E_ANY){
            DROP(e);
        }else{
            TRACE(e, "server said: %x\n", FD(&contents));
        }
        ORIG(e, E_RESPONSE, "add_device failed");
    }
    return E_OK;
}

derr_t key_tool_peer_list_load(key_tool_t* kt, const char* filename){
    kt->json.len = 0;
    kt->json_block.len = 0;
    // read the peer list file
    derr_t e = dstr_fread_file(filename, &kt->json_block);
    CATCH(e, E_OPEN | E_OS){
        RETHROW(e, E_FS);
    }else PROP(e, e);

    // parse the peer list
    PROP(e, json_parse(&kt->json, &kt->json_block) );

    // now try to read all of the peers out of the peerlist
    if(kt->json.data[0].type != JSON_ARRAY){
        ORIG(e, E_PARAM, "json root object must be array");
    }
    json_t* jpeer = kt->json.data[0].first_child;
    while(jpeer){
        // get the hex fingerprint from json
        dstr_t hexfpr;
        // convert this array element to a dstr
        PROP(e, j_to_dstr(*jpeer, &hexfpr) );
        // allocate new fingerprint
        dstr_t fpr;
        PROP(e, dstr_new(&fpr, FL_FINGERPRINT) );
        // convert from binary
        e = hex2bin(&hexfpr, &fpr);
        if(e.type){
            dstr_free(&fpr);
            PROP(e, e);
        }
        // append to list
        e = LIST_APPEND(dstr_t, &kt->peer_list, fpr);
        if(e.type){
            dstr_free(&fpr);
            PROP(e, e);
        }
        // move on to the next json element
        jpeer = jpeer->next;
    }
    return E_OK;
}

derr_t key_tool_peer_list_write(key_tool_t* kt, const char* filename){
    derr_t e = E_OK;
    FILE* f = fopen(filename, "w");
    if(!f){
        TRACE(e, "%x: %x\n", FS(filename), FE(&errno));
        ORIG(e, errno == ENOMEM ? E_NOMEM : E_FS, "unable to open peer_list file for writing");
    }

    DSTR_STATIC(start, "[\"");
    DSTR_STATIC(separator, "\",\n \"");
    DSTR_STATIC(end, "\"]\n");

    // start the json array
    PROP_GO(e, dstr_fwrite(f, &start), cleanup);

    for(size_t i = 0; i < kt->peer_list.len; i++){
        // get a hex of the fingerprint
        DSTR_VAR(hexfpr, FL_FINGERPRINT * 2);
        e =  bin2hex(&kt->peer_list.data[i], &hexfpr);
        // an E_FIXEDSIZE represents an internal error
        CATCH(e, E_FIXEDSIZE){
            RETHROW_GO(e, E_INTERNAL, cleanup);
        }else PROP_GO(e, e, cleanup);
        // write the hex fingerprint
        PROP_GO(e, dstr_fwrite(f, &hexfpr), cleanup);
        // write the separator if necessary
        if(i + 1 < kt->peer_list.len){
            PROP_GO(e, dstr_fwrite(f, &separator), cleanup);
        }
    }

    // finish the json array
    PROP_GO(e, dstr_fwrite(f, &end), cleanup);

cleanup:
    fclose(f);
    return e;
}

derr_t key_tool_new(key_tool_t* kt, const dstr_t* dir, int def_key_bits){
    derr_t e = E_OK;
    // store the dir
    DSTR_WRAP_ARRAY(kt->dir, kt->dir_buffer);
    e = dstr_copy(dir, &kt->dir);
    // fixed-size error here means file-system error to higher code
    CATCH(e, E_FIXEDSIZE){
        RETHROW(e, E_FS);
    }else PROP(e, e);

    // try to load the key
    DSTR_VAR(temp_path, 4096);
    PROP(e, FMT(&temp_path, "%x/device.pem", FD(dir)) );
    e = keypair_load(&kt->key, temp_path.data);
    // an E_OPEN means it is likely that the key doesn't exist
    // and an SSL error means we should just regenerate the key
    CATCH(e, E_OPEN | E_SSL){
        DROP(e);
        // if we can't load the key, gen_key now and register it later
        LOG_WARN("Unable to load key, generating a new one\n");

        // generate the key
        e = gen_key(def_key_bits, temp_path.data);
        // E_OPEN here means E_FS to higher level code
        CATCH(e, E_OPEN){
            RETHROW(e, E_FS);
        }else PROP(e, e);
        LOG_WARN("key generated!\n");

        // now try and load the key
        e = keypair_load(&kt->key, temp_path.data);
        // E_OPEN or E_SSL here represents an internal error
        CATCH(e, E_OPEN | E_SSL){
            RETHROW(e, E_INTERNAL);
        }else PROP(e, e);

        LOG_INFO("key tool generated a new key\n");
        kt->did_key_gen = true;
    }else{
        // propagate uncaught errors
        PROP(e, e);
        // or continue
        LOG_DEBUG("key tool loaded an old key\n");
        kt->did_key_gen = false;
    }

    // allocate the json_block
    PROP_GO(e, dstr_new(&kt->json_block, 4096), fail_1);

    // allocate the json list
    PROP_GO(e, LIST_NEW(json_t, &kt->json, 32), fail_2);

    // allocate the peer_list
    PROP_GO(e, LIST_NEW(dstr_t, &kt->peer_list, FL_DEVICES), fail_3);

    // now attempt to load the peer list
    temp_path.len = 0;
    e = FMT(&temp_path, "%x/peer_list.json", FD(dir));
    // E_FIXEDSIZE means a file system error
    CATCH(e, E_FIXEDSIZE){
        RETHROW_GO(e, E_FS, fail_4);
    }else PROP_GO(e, e, fail_4);

    e = key_tool_peer_list_load(kt, temp_path.data);
    // the list might not exist or it might be corrupted
    CATCH(e, E_FS | E_PARAM){
        DROP(e);
        // if we can't load the list, set state to new
        kt->peer_list.len = 0;
        kt->peer_list_state = KT_PL_NEW;
    }else{
        PROP(e, e);
        kt->peer_list_state = KT_PL_OLD;
    }

    // allocate the new_peer_list
    PROP_GO(e, LIST_NEW(dstr_t, &kt->new_peer_list, 32), fail_4);

    // indicate that we haven't found any expired peers yet
    kt->found_expired_peer = false;

    // allocate for the decrypter
    PROP_GO(e, decrypter_new(&kt->dc), fail_5);

    return E_OK;

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
    LIST_FREE(json_t, &kt->json);
fail_2:
    dstr_free(&kt->json_block);
fail_1:
    keypair_free(&kt->key);
    return e;
}

void key_tool_free(key_tool_t* kt){
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
    LIST_FREE(json_t, &kt->json);
    dstr_free(&kt->json_block);
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
    LOG_INFO("calling list devices\n");
    // get ready to recieve JSON
    kt->json.len = 0;

    int code;
    DSTR_VAR(reason, 1024);
    kt->json_block.len = 0;

    // get ready for the api call
    DSTR_STATIC(command, "list_devices");

    // do the api call
    e = api_password_call(host, port, &command, NULL, user, pass,
                              &code, &reason, &kt->json_block, &kt->json);
    CATCH(e, E_FIXEDSIZE){
        TRACE(e, "server response too long\n");
        RETHROW(e, E_RESPONSE);
    }else PROP(e, e);

    // non-200-series responses are errors
    if(code < 200 || code > 299){
        TRACE(e, "API server responded with a HTTP code %x\n", FI(code));
        ORIG(e, E_RESPONSE, reason.data);
    }

    // now make sure the api call returned success
    DSTR_STATIC(success, "success");
    dstr_t status;
    e = j_to_dstr(jk(kt->json.data[0], "status"), &status);
    CATCH(e, E_PARAM){
        TRACE(e, "server response contained the wrong json\n");
        RETHROW(e, E_RESPONSE);
    }else PROP(e, e);

    int match = dstr_cmp(&success, &status);
    if(match != 0){
        dstr_t contents;
        e = j_to_dstr(jk(kt->json.data[0], "contents"), &contents);
        CATCH(e, E_PARAM){
            DROP(e);
            TRACE(e, "server response contained the wrong json\n");
            TRACE(e, "server said: %x\n", FD(&contents));
        }else{
            PROP(e, e);
            TRACE(e, "server said: %x\n", FD(&kt->json_block));
        }
        ORIG(e, E_RESPONSE, "call to list_devices API endpoint failed");
    }

    return E_OK;
}

derr_t key_tool_update(key_tool_t* kt, const char* host, unsigned int port,
                       const dstr_t* user, const dstr_t* pass){
    // under some specific conditions we can skip all of this
    if(kt->peer_list_state == KT_PL_OLD
            && kt->new_peer_list.len == 0
            && kt->found_expired_peer == false
            && kt->did_key_gen == false){
        LOG_DEBUG("skipping key_tool_update()\n");
        return E_OK;
    }

    derr_t e = E_OK;

    // get ready to pull fingerprints out of json format
    DSTR_VAR(fpr_block, FL_DEVICES * FL_FINGERPRINT);
    LIST_VAR(dstr_t, srv_fprs, FL_DEVICES);


    // check if we need to list devices
    bool our_key_missing_from_list_devices = false;
    if(kt->peer_list_state == KT_PL_NEW
            || kt->new_peer_list.len > 0
            || kt->found_expired_peer == true){

        // make call to list devices
        PROP(e, call_list_devices(kt, host, port, user, pass) );

        // get a pointer to the first item of the devices list
        json_t devices = jk(jk(kt->json.data[0], "contents"), "devices");
        // devices should be a JSON_ARRAY of fingerprints
        if(devices.type != JSON_ARRAY){
            ORIG(e, E_RESPONSE, "failed to interpret json");
        }
        json_t* device = devices.first_child;

        // pull fingerprints out of json format (and convert to binary format)
        while(device){
            // get the hex fingerprint from json
            dstr_t hexfpr;
            e = j_to_dstr(*device, &hexfpr);
            CATCH(e, E_PARAM){
                TRACE(e, "json response has an invalid device entry\n");
                RETHROW(e, E_RESPONSE);
            }else PROP(e, e);
            // verify the length of the fingerprint
            if(hexfpr.len != 2 * FL_FINGERPRINT){
                ORIG(e, E_RESPONSE, "response contained a wrong-sized fingerprint");
            }
            // convert to binary binary fingerprint and add to list
            size_t start = fpr_block.len;
            e = hex2bin(&hexfpr, &fpr_block);
            CATCH(e, E_PARAM){
                // this means that there was a bad hex string from the server
                TRACE(e, "response contained bad hex string\n");
                RETHROW(e, E_RESPONSE);
            }else PROP(e, e);
            dstr_t binfpr = dstr_sub(&fpr_block, start, 0);
            // verify the length of the binary fingerprint
            if(binfpr.len != FL_FINGERPRINT){
                ORIG(e, E_RESPONSE, "response contained a wrong-sized fingerprint");
            }
            e = LIST_APPEND(dstr_t, &srv_fprs, binfpr);
            CATCH(e, E_FIXEDSIZE){
                // this means there were more entries than was allowed
                TRACE(e, "too many fingerprints returned by list_devices\n");
                RETHROW(e, E_RESPONSE);
            }else PROP(e, e);
            // continue with next device in the json array
            device = device->next;
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
                PROP(e, dstr_new(&fpr, FL_FINGERPRINT) );
                // copy the server's fingerprint to the new dstr_t
                e = dstr_copy(&srv_fprs.data[i], &fpr);
                if(e.type){
                    dstr_free(&fpr);
                    PROP(e, e);
                }
                // append to list
                e = LIST_APPEND(dstr_t, &kt->new_peer_list, fpr);
                if(e.type){
                    dstr_free(&fpr);
                    PROP(e, e);
                }
            }
        }

        // if we are using an old key, make sure it is in the list
        if(kt->did_key_gen == false){
            if(in_list(&kt->key.fingerprint, &srv_fprs, NULL) == false){
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
            PROP(e, dstr_new(&fpr, FL_FINGERPRINT) );
            // copy the server's fingerprint to the new dstr_t
            e = dstr_copy(&srv_fprs.data[i], &fpr);
            if(e.type){
                dstr_free(&fpr);
                PROP(e, e);
            }
            // append to list
            e = LIST_APPEND(dstr_t, &kt->peer_list, fpr);
            if(e.type){
                dstr_free(&fpr);
                PROP(e, e);
            }
        }
    }

    // check if we need to add_device
    if(kt->did_key_gen == true || our_key_missing_from_list_devices == true){
        // add the device
        PROP(e, key_tool_register_key(kt, host, port, user, pass) );
    }

    /* now that we have checked the server's list of devices (if necessary)
       and registered our own key (if necessary), we need to update the peer
       list on file */

    // if our own key is not in the list, add it
    if(in_list(&kt->key.fingerprint, &kt->peer_list, NULL) == false){
        // it has to be dynamically allocated for the kt->peer_list
        dstr_t fpr;
        PROP(e, dstr_new(&fpr, FL_FINGERPRINT) );
        e = dstr_copy(&kt->key.fingerprint, &fpr);
        if(e.type){
            dstr_free(&fpr);
            PROP(e, e);
        }
        // append to list
        e = LIST_APPEND(dstr_t, &kt->peer_list, fpr);
        if(e.type){
            dstr_free(&fpr);
            PROP(e, e);
        }
    }

    // get peer list filename
    DSTR_VAR(temp_path, 4096);
    e = FMT(&temp_path, "%x/peer_list.json", FD(&kt->dir));
    CATCH(e, E_FIXEDSIZE){
        RETHROW(e, E_FS);
    }else PROP(e, e);

    // and write peer list to file
    PROP(e, key_tool_peer_list_write(kt, temp_path.data) );
    LOG_INFO("key tool update complete\n");
    return E_OK;
}

derr_t key_tool_check_recips(key_tool_t* kt, LIST(dstr_t)* recips){
    derr_t e = E_OK;

    // do nothing if we had no peer list on file
    if(kt->peer_list_state == KT_PL_NEW) return E_OK;

    /* because we don't need to track the identity of expired peers, it should
       be sufficient to check if the list is shorter than ours and check if
       there's any recipients in *recips that are not in our list */

    if(kt->peer_list.len > recips->len){
        kt->found_expired_peer = true;
    }
    // PFMT("peers:\n");
    // for(size_t i = 0; i < kt->peer_list.len; i++){
    //     DSTR_VAR(hex, 2 * FL_FINGERPRINT);
    //     PROP(e, bin2hex(&kt->peer_list.data[i], &hex) );
    //     PFMT("    %x\n", FD(&hex));
    // }
    // PFMT("recips:\n");
    // for(size_t i = 0; i < recips->len; i++){
    //     DSTR_VAR(hex, 2 * FL_FINGERPRINT);
    //     PROP(e, bin2hex(&recips->data[i], &hex) );
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
                PROP(e, dstr_new(&new_peer, recip->len) );
                derr_t e = dstr_copy(recip, &new_peer);
                if(e.type){
                    dstr_free(&new_peer);
                    PROP(e, e);
                }
                e = LIST_APPEND(dstr_t, &kt->new_peer_list, new_peer);
                if(e.type){
                    dstr_free(&new_peer);
                    PROP(e, e);
                }
            }
        }
    }

    return E_OK;
}

derr_t key_tool_decrypt(key_tool_t* kt, int infd, int outfd, size_t* outlen){
    derr_t e = E_OK;
    DSTR_VAR(recips_block, FL_DEVICES * FL_FINGERPRINT);
    LIST_VAR(dstr_t, recips, FL_DEVICES);
    PROP(e, decrypter_start(&kt->dc, &kt->key, &recips, &recips_block) );

    DSTR_VAR(inbuf, 4096);
    DSTR_VAR(outbuf, 4096 + FL_ENCRYPTION_BLOCK_SIZE);
    size_t amnt_read;
    *outlen = 0;
    while(true){
        // read from input
        e = dstr_read(infd, &inbuf, 0, &amnt_read);
        // this should never result in E_FIXEDSIZE
        CATCH(e, E_FIXEDSIZE){
            RETHROW(e, E_INTERNAL);
        }else PROP(e, e);

        // break if we are done
        if(amnt_read == 0) break;

        // decrypt a chunk
        e = decrypter_update(&kt->dc, &inbuf, &outbuf);
        /* in one special error case the decryption fails but we know we have
           a complete list of recipients */
        if(e.type == E_NOT4ME){
            MERGE(e, key_tool_check_recips(kt, &recips), "checking recips");
        }
        // if there was a FIXEDSIZE error that is our fault
        CATCH(e, E_FIXEDSIZE){
            RETHROW(e, E_INTERNAL);
        }
        // An SSL error is just a bad message, as far as higher code cares
        else CATCH(e, E_SSL){
            RETHROW(e, E_PARAM);
        }else PROP(e, e);

        // write to buffer
        PROP(e, dstr_write(outfd, &outbuf) );
        *outlen += outbuf.len;
        outbuf.len = 0;

    }
    // decrypt the last chunk
    e = decrypter_finish(&kt->dc, &outbuf);
    // if there was a FIXEDSIZE error that is our fault
    CATCH(e, E_FIXEDSIZE){
        RETHROW(e, E_INTERNAL);
    }
    // An SSL error is just a bad message, as far as higher code cares
    else CATCH(e, E_SSL){
        RETHROW(e, E_PARAM);
    }else PROP(e, e);

    // write the last chunk
    PROP(e, dstr_write(outfd, &outbuf) );
    *outlen += outbuf.len;
    outbuf.len = 0;
    PROP(e, key_tool_check_recips(kt, &recips) );
    return E_OK;
}
