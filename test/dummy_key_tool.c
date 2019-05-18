#include <key_tool.h>
#include <logger.h>

#include "dummy_key_tool.h"

extern const char* g_test_files;
derr_type_t key_tool_update_should_return = E_NONE;
derr_type_t key_tool_new_should_return = E_NONE;

derr_t key_tool_new(key_tool_t* kt, const dstr_t* dir, int def_key_bits){
    derr_t e = E_OK;

    if(key_tool_new_should_return != E_NONE){
        return (derr_t){.type=key_tool_new_should_return};
    }
    // suppress unused parameter warning
    (void) dir;
    (void) def_key_bits;

    // indicate that we haven't found any expired peers yet
    kt->found_expired_peer = false;

    // try to load the key
    DSTR_VAR(path, 4096);
    PROP(e, FMT(&path, "%x/dummy_key_tool/key_m.pem", FS(g_test_files)) );
    PROP(e, keypair_load(&kt->key, path.data) );

    // allocate for the decrypter
    PROP_GO(e, decrypter_new(&kt->dc), fail_1);

    return E_OK;

fail_1:
    keypair_free(&kt->key);
    return e;
}

void key_tool_free(key_tool_t* kt){
    decrypter_free(&kt->dc);
    keypair_free(&kt->key);
}

derr_t key_tool_update(key_tool_t* kt, const char* host, unsigned int port,
                       const dstr_t* user, const dstr_t* pass){
    // suppress unused parameter warning
    (void) kt;
    (void) host;
    (void) port;
    (void) user;
    (void) pass;
    return (derr_t){.type=key_tool_update_should_return};
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
        PROP(e, dstr_read(infd, &inbuf, 0, &amnt_read) );
        if(amnt_read == 0) break;

        // decrypt a chunk
        PROP(e, decrypter_update(&kt->dc, &inbuf, &outbuf) );

        // write to buffer
        PROP(e, dstr_write(outfd, &outbuf) );
        *outlen += outbuf.len;
        outbuf.len = 0;

    }
    // get the last chunk and write it
    PROP(e, decrypter_finish(&kt->dc, &outbuf) );
    PROP(e, dstr_write(outfd, &outbuf) );
    *outlen += outbuf.len;
    outbuf.len = 0;
    return E_OK;
}
