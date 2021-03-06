#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>

#include <common.h>
#include <logger.h>
#include <key_tool.h>
#include <fixed_lengths.h>
#include <fileops.h>
#include <networking.h>

#include "fake_api_server.h"
#include "test_utils.h"

#include <win_compat.h>

// path to where the test files can be found
const char* g_test_files;

/*
devices:
    m: the key of this device (me), if there is one
    a: always there
    e: expired peer
    n: new peer

API calls that could be made:
    list_devices     (LIST)
    add_device       (ADD)

inputs:
    device file states (key exists = K, token exists = T):
        kt kT Kt KT
    states of peers_on_file:
        none, aem
    server_peers {e present = E, new present = N, m present = M)
        ENM ENm EnM Enm eNM eNm enM enm
    decrypt
        ENM ENm EnM Enm eNM eNm enM enm

outputs (some outputs can't be tested together):
    decrypt {aem, am, aenm, anm, ae, a, aen, an}
        E_OK, E_NOT4ME
    sequence of API calls:
    list_dev called
        none, INI, INI_REG_FIN, REG_FIN
    new_peers
        [], [n]

rule format:
    // presence of local storage files
    [kK] K = key on file
    [pP] P = peers on file (aem), p = no peers on file
    // server peer list contents
    [eE] E = "e" peer present
    [nN] N = "n" peer present
    [mM] M = our key is present
    // files attempted to be decrypted
    [eE] e = "e" peer absent in any passed messages
    [nN] N = "n" peer present in any passed messages
    [mM] m = our key is absent in any passed messages
    // unencrypted file found
    [uU] U = unencrypted file found


rules to fully describe outputs:

output              rule            english explanation
--------------------------------------------------------------------------
api = NONE          KP ... EnM u    if all of these are true no call is made
api = LIST          K. ..M ... .    else if I have key and server knows me
api = LIST_ADD      K. ... ... .    else if I have key
api = ADD           .P ... EnM u    if I don't have key, but see no changes
api = LIST_ADD      .. ... ... .    in all other cases

new_peers = no      .p ... ... .    if no peer list on file, can't ID new peers
new_peers = yes     .. ... .N. .    if we got mail to N new_peers is yes
new_peers = no     (check api seq)  if we don't ask the server we won't know
new_peers = yes     .. .N. ... .    otherwise new_peer will come from server
new_peers = no      .. ... ... .    or it won't come at all

# Note that testing decryption output is not orthogonoal to the previous tests
# because it affects the other outputs, and it doesn't require as many inputs.
# Also note that here the "files attempted to be decrypted" column is per msg,
# not a mask over all of the messages like above.
# However, in the course of testing the above, we will actually pass through
# each state that we need to below.

output              rule            english explanation
--------------------------------------------------------------------------
decrypt = E_NOT4ME  k.. ... ... .   ("if we have no key dec return NOT4ME")
decrypt = E_NOT4ME  ... ... ..m .   ("or if the message isn't to me")
decrypt = E_OK      ... ... ... .   ("otherwise it should always return E_OK")

OK: there's 32768 input combinations in the first test suite
    and only 64 in the second suite....

    if we don't do all 2^8 combinations of message passes to each key_tool
    setup, but instead cover each of the 8 [Ee][Nn][Mm] masks, we will have a
    more reasonable 1024 tests.  I think that is the way to go.

    It works out nicely that all of the test conditions are binary, so we can
    use a regular unsigned int to represent any given test case
*/

#define K  (1<<8)
#define P  (1<<7)
#define SE (1<<6)
#define SN (1<<5)
#define SM (1<<4)
#define DE (1<<3)
#define DN (1<<2)
#define DM (1<<1)
#define U  (1<<0)

typedef enum {
    API_NONE, // no calls are made
    API_LIST, // only the list_devices call is made
    API_LIST_ADD,  // list_devices then add_device
    API_ADD, // only adding devices
} api_call_sequence_t;

static api_call_sequence_t api_seq_by_counter(unsigned int ctr){

    if((ctr & (K|P|DE|DN|DM|U)) == (K|P|DE|DM)) return API_NONE;
    if((ctr & (K|SM)) == (K|SM)) return API_LIST;
    if((ctr & K) == K) return API_LIST_ADD;
    if((ctr & (P|DE|DN|DM|U)) == (P|DE|DM)) return API_ADD;

    return API_LIST_ADD;
}


// this implements the "new_peers" rules, above
static bool new_peer_from_counter(unsigned int ctr){
    api_call_sequence_t seq = api_seq_by_counter(ctr);
    // if no peer list on file, can't identify new peers
    if((ctr & P) == 0) return false;
    // else if we got mail to a new peer, then obviously yes
    else if((ctr & DN) == DN) return true;
    // else we won't get new peers if we don't talk to the server
    else if(seq == API_NONE || seq == API_ADD) return false;
    // else the new_peer comes from the server
    else return (ctr & SN) == SN;
}

static derr_t decrypt_result_from_counter(unsigned int ctr){
    // implements the "decrypt" rules
    //         k........
    if((ctr & K) == 0) return E_NOT4ME;
    //            .......m.
    else if((ctr & DM) == 0) return E_NOT4ME;
    else return E_OK;
}

// these correspond to real keys in test/files/key_tool
DSTR_STATIC(fpr_a, "3d94f057f427e2ee34bb51733b8d3ee62a8fdaaa50da71d14e4b2d7f44763471");
DSTR_STATIC(fpr_e, "8c7e72356d46734eeaf2d163302cc560f60b513d7644dae92b390b7d8f28ae95");
DSTR_STATIC(fpr_n, "d99c55626294263cf8dbe54c0286673f666d9ac969b5856ff78558394365f360");
DSTR_STATIC(fpr_m, "eefdab7d7d97bf74d16684f803f3e2a4ef7aa181c9940fbbaff4427f1f7dde32");

DSTR_STATIC_VAR(reg_fpr, 2*FL_FINGERPRINT);
DSTR_STATIC_VAR(reg_pem, 4096);

// I will also need access to the public key, which will be loaded from a file
DSTR_STATIC(pubkey_m,
    "-----BEGIN PUBLIC KEY-----\\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCU9j/irie2dpd2gaiVpEh7LKg6\\n"
    "fI2OMab/tBcoZqYvsQkQX1dgi8s9bFXpibycxzuyy4S3DyeVP2Vx8jhNHkTa9BLP\\n"
    "BmPmhk7U6qgE2rV9jTdwOaAoUiv0POWUhJXIITEvejobiYMFcQ9hLCJYddwam9o/\\n"
    "UwJpH89DBf32mJduYQIDAQAB\\n"
    "-----END PUBLIC KEY-----\\n");

DSTR_STATIC(privkey_m,
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIICXQIBAAKBgQCU9j/irie2dpd2gaiVpEh7LKg6fI2OMab/tBcoZqYvsQkQX1dg\n"
    "i8s9bFXpibycxzuyy4S3DyeVP2Vx8jhNHkTa9BLPBmPmhk7U6qgE2rV9jTdwOaAo\n"
    "Uiv0POWUhJXIITEvejobiYMFcQ9hLCJYddwam9o/UwJpH89DBf32mJduYQIDAQAB\n"
    "AoGAFFTb+WON1hCvsaQWz33hyrYYrAruAzdxtLru4jvIeP/v3cU1lt7duZ98xmhf\n"
    "TwK+ejPfBGFUJMHHZdsKpjP4b7jLVtzYkGYtxlpy4Ioyhozg2vCVPmyg84yY8atw\n"
    "KI7FlNjCpNGkGNheBX7SMYEGaIQGuQ1MVdVDCNjoc3qOZQECQQDE9N/vXheKAnZd\n"
    "I95s5yZKg5kln7KYGfrL77/qCRADtGEJ+E2oaed2tvUD8Sc46woEpWo0O1mBWV8Y\n"
    "MwTvduXRAkEAwZ4Z5i+V9F2j5BybDPTP/nZe+pUjG0AwhIzngpmnvzYgpu8/3Koe\n"
    "DOxUx421mKP6OfyB5QM+ZP8lRLrR+pTTkQJAXQ+vN7TnvmgHcV7fW+mkKBUiKarZ\n"
    "ghDUdcPklDqP/JAgQcu3NdpEac1s2934QGaeJy/ZjLB2TC3kRtTkghlV4QJBAJUN\n"
    "9thLqACxGhvhncgSrBE05Ze5uoYfG3rf0tarHgXJUMfTBfIGEQ5X3kimIrg4/Mkp\n"
    "SIKaxa0Q84r+2+oyKtECQQC0mreZ3jup6aLKIs4ztCNLmemB8wLRnFV36o0eq+iY\n"
    "dRWR9AsEh8gFtNqDvVLMx6OSqMTRLZr6XGIDeQbUcjaZ\n"
    "-----END RSA PRIVATE KEY-----\n");

DSTR_STATIC(old_peer_list,
    "[\"3d94f057f427e2ee34bb51733b8d3ee62a8fdaaa50da71d14e4b2d7f44763471\",\n"
    " \"8c7e72356d46734eeaf2d163302cc560f60b513d7644dae92b390b7d8f28ae95\",\n"
    " \"eefdab7d7d97bf74d16684f803f3e2a4ef7aa181c9940fbbaff4427f1f7dde32\"]\n");

static derr_t build_peer_list_result(unsigned int ctr, dstr_t* out){
    out->len = 0;

    api_call_sequence_t seq = api_seq_by_counter(ctr);

    // if we have no calls, peer list won't change
    if(seq == API_NONE){
        PROP( dstr_copy(&old_peer_list, out) );
        return E_OK;
    }

    bool fetched_list = (seq == API_LIST || seq == API_LIST_ADD);
    bool added_device = (seq == API_ADD || seq == API_LIST_ADD);

    DSTR_STATIC(start, "[\"");
    DSTR_STATIC(separator, "\",\n \"");
    DSTR_STATIC(end, "\"]\n");
    // always append the "start"
    out->len = 0;
    PROP( dstr_append(out, &start) );
    // device "a" is always present
    PROP( dstr_append(out, &fpr_a) );
    // append device "e"
    if(!fetched_list || (ctr & SE) == SE){
        PROP( dstr_append(out, &separator) );
        PROP( dstr_append(out, &fpr_e) );
    }
    // append device "m"
    if(!fetched_list || (ctr & SM) == SM){
        PROP( dstr_append(out, &separator) );
        PROP( dstr_append(out, &fpr_m) );
    }
    // append device "n"
    if(fetched_list && (ctr & SN) == SN){
        PROP( dstr_append(out, &separator) );
        PROP( dstr_append(out, &fpr_n) );
    }
    // append the newly registered device
    if(added_device){
        PROP( dstr_append(out, &separator) );
        // if it is a re-registration, just append fpr_m again
        if(ctr & K){
            PROP( dstr_append(out, &fpr_m) );
        // otherwise append the fingerprint we got in the handle_new_key() hook
        }else{
            PROP( dstr_append(out, &reg_fpr) );
        }
    }
    PROP( dstr_append(out, &end) );
    return E_OK;
}

DSTR_STATIC(add_device_path, "/api/add_device");
DSTR_STATIC(list_devices_path, "/api/list_devices");
DSTR_STATIC(wildcard_arg, "*");

static derr_t build_list_devices_response(unsigned int ctr, const dstr_t* reg,
                                          dstr_t* out){
    out->len = 0;
    DSTR_STATIC(start, "{\"status\":\"success\",\"contents\":{\"devices\":[");
    DSTR_STATIC(separator, ",");
    DSTR_STATIC(end, "]}}");
    // always append the "start"
    out->len = 0;
    PROP( dstr_append(out, &start) );
    // each device can reuse the formatted string
    const char* format = "\"%x\"";
    // always append device "a"
    PROP( FMT(out, format, FD(&fpr_a)) );
    // append device "e"
    if((ctr & SE) == SE){
        PROP( dstr_append(out, &separator) );
        PROP( FMT(out, format, FD(&fpr_e)) );
    }
    // append device "m"
    if((ctr & SM) == SM){
        PROP( dstr_append(out, &separator) );
        PROP( FMT(out, format, FD(&fpr_m)) );
    }
    // append device "n"
    if((ctr & SN) == SN){
        PROP( dstr_append(out, &separator) );
        PROP( FMT(out, format, FD(&fpr_n)) );
    }
    // append the newly registered device
    if(reg){
        PROP( dstr_append(out, &separator) );
        PROP( FMT(out, format, FD(reg)) );
    }
    PROP( dstr_append(out, &end) );
    return E_OK;
}

static derr_t build_add_device_response(const dstr_t* fpr, dstr_t* out){
    out->len = 0;
    PROP( FMT(out, "{\"status\":\"success\",\"contents\":{\"device\":\"%x\"}}",
                        FD(fpr)));
    return E_OK;
}

static derr_t handle_new_key(const dstr_t* path, const dstr_t* arg,
                             unsigned int counter){
    // suppress unused parameter warning
    (void) path;
    (void) counter;
    derr_t error;
    // the new key is going to be passed in PEM format, and we need the fpr
    keypair_t kp;
    DSTR_VAR(temp, 4096);
    PROP( json_decode(arg, &temp) );
    PROP( keypair_from_pem(&kp, &temp) );
    // get hex of the fingerprint
    DSTR_VAR(hexfpr, 2*FL_FINGERPRINT);
    PROP_GO( bin2hex(&kp.fingerprint, &hexfpr), cu);
    // build a response for add_device
    DSTR_VAR(response, 2048);
    PROP_GO( build_add_device_response(&hexfpr, &response), cu);
    PROP_GO( fas_response_put(200, &response), cu);

    // store the key for later use
    PROP_GO( dstr_copy(arg, &reg_pem), cu);
    // store the hexfpr for later use
    PROP_GO( dstr_copy(&hexfpr, &reg_fpr), cu);

cu:
    keypair_free(&kp);
    return E_OK;
}

static derr_t pack_responses_by_counter(unsigned int ctr){
    // from the order of API call rules, above

    api_call_sequence_t seq = api_seq_by_counter(ctr);

    DSTR_VAR(response, 2048);

    // do we pack a response for the initial list_devices?
    if(seq == API_LIST || seq == API_LIST_ADD){
        // pack the expected API call
        PROP( fas_expect_put(&list_devices_path, &wildcard_arg, NULL, 0) );
        PROP( build_list_devices_response(ctr, NULL, &response) );
        PROP( fas_response_put(200, &response) );
    }

    response.len = 0;

    // do we pack a response for the add_device command?
    if(seq == API_ADD || seq == API_LIST_ADD){
        // are we expecting a new key or a re-registration?
        if((ctr & K) == K){
            // key_tool has key on file, expecting a re-registration
            PROP( fas_expect_put(&add_device_path, &pubkey_m, NULL, 0) );
            // build the response
            PROP( build_add_device_response(&fpr_m, &response) );
            PROP( fas_response_put(200, &response) );
        }else{
            // key tool does not have key on file, need to deal with a new key
            PROP( fas_expect_put(&add_device_path, &wildcard_arg,
                                 handle_new_key, ctr) );
            // the response will be pushed by handle_new_key
        }
    }
    return E_OK;
}

static derr_t decrypt_by_counter(key_tool_t* kt, unsigned int ctr){
    derr_t error = E_OK;
    char* e = (ctr & DE) ? "EE" : "e";
    char* n = (ctr & DN) ? "NN" : "n";
    char* m = (ctr & DM) ? "MM" : "m";
    char infile[4096];
    snprintf(infile, sizeof(infile), "%s/key_tool/AA%s%s%s.msg",
                                     g_test_files, e, n, m);
    // open a file descriptor for reading
    int from = open(infile, O_RDONLY);
    if(from < 0){
        LOG_ERROR("%x: %x\n", FS(infile), FE(&errno));
        ORIG(E_OS, "unable to open file for decrypting");
    }
    // open a file descriptor for writing (we don't check the output)
    int to = open("_tktdir/trash", O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if(to < 0){
        LOG_ERROR("%x: %x\n", FS("_tktdir/trash"), FE(&errno));
        ORIG_GO(E_OS, "unable to open trash file", cu1);
    }
    // now make sure we get the right response from key_tool_decrypt
    size_t len;
    derr_t err = key_tool_decrypt(kt, from, to, &len);
    if(err) LOG_CATCH(err);
    derr_t exp_e = decrypt_result_from_counter(ctr);
    if(err != exp_e){
        LOG_ERROR("expected %x but got %x\n",
                   FD(error_to_dstr(exp_e)), FD(error_to_dstr(err)));
        ORIG_GO(E_VALUE, "key_tool_decrypt returned wrong status", cu2);
    }
cu2:
    close(to);
cu1:
    close(from);
    return error;
}

static derr_t do_test(unsigned int ctr){
    derr_t error;
    // delete the temporary directory if it already exists
    error = rm_rf("_tktdir");
    CATCH(E_ANY){
        // ignore this error entirely
        error = E_OK;
    }
    // create the "test key tool directory"
    int ret = mkdir("_tktdir", 0770);
    if(ret != 0){
        LOG_ERROR("%x: %x\n", FS("_tktdir"), FE(&errno));
        ORIG(E_OS, "unable to create temporary directory");
    }
    // place initial files as necessary
    if(ctr & K){
        PROP( dstr_fwrite_file("_tktdir/device.pem", &privkey_m) );
    }
    if(ctr & P){
        PROP( dstr_fwrite_file("_tktdir/peer_list.json", &old_peer_list) );
    }

    // allocate the key_tool
    key_tool_t kt;
    DSTR_STATIC(tempdir, "_tktdir");
    // absurdly small RSA key size makes for reasonably fast testing
    PROP( key_tool_new(&kt, &tempdir, 512) );

    // do the specified decryption
    PROP_GO( decrypt_by_counter(&kt, ctr), cu);

    // set key_tool.found_expired_peer if necessary
    if((ctr & U) == U)
        kt.found_expired_peer = true;

    // pack API calls and responses
    PROP_GO( pack_responses_by_counter(ctr), cu);

    // update the key tool
    DSTR_STATIC(user, "user@splintermail.com");
    DSTR_STATIC(pass, "top_secr3t");
    PROP_GO( key_tool_update(&kt, "127.0.0.1", fas_api_port, &user, &pass), cu);

    // make sure we got all the calls we thought we would
    PROP_GO( fas_assert_done(), cu);

    // make sure that we have a new peer if we are supposed to
    bool expecting_new_peer = new_peer_from_counter(ctr);
    if(expecting_new_peer != (kt.new_peer_list.len > 0)){
        LOG_ERROR("expecting new peer = %x but new_peer_list.len = %x\n",
                  FS(expecting_new_peer ? "true" : "false"), FU(kt.new_peer_list.len));
        for(size_t i = 0; i < kt.new_peer_list.len; i++){
            DSTR_VAR(hex, 2*FL_FINGERPRINT);
            bin2hex(&kt.new_peer_list.data[i], &hex);
            LOG_ERROR("    %x\n", FD(&hex));
        }
        ORIG_GO(E_VALUE, "new peer list is wrong", cu);
    }

    DSTR_VAR(temp, 2048);
    int result;
    // compare both of the output files
    PROP_GO( build_peer_list_result(ctr, &temp), cu);
    PROP_GO( file_cmp_dstr("_tktdir/peer_list.json", &temp, &result), cu);
    if(result != 0){
        DSTR_VAR(ftemp, 2048);
        dstr_fread_file("_tktdir/peer_list.json", &ftemp);
        LOG_ERROR("expected:\n%x\n-----\n"
                  "but got:\n%x\n-----\n", FD(&temp), FD(&ftemp));
        ORIG_GO(E_VALUE, "peer_list.json did not match", cu);
    }

    // only compare device.pem if we already had a key
    if(ctr & K){
        PROP_GO( file_cmp_dstr("_tktdir/device.pem", &privkey_m, &result), cu);
        if(result != 0){
            DSTR_VAR(ftemp, 2048);
            dstr_fread_file("_tktdir/device.pem", &ftemp);
            LOG_ERROR("expected:\n%x-----\n"
                      "but got:\n%x-----\n", FD(&privkey_m), FD(&ftemp));
            ORIG_GO(E_VALUE, "device.pem did not match", cu);
        }
    }

cu:
    key_tool_free(&kt);
    return error;
}

static derr_t counter_to_dstr(unsigned int counter, dstr_t* buffer){
    buffer->len = 0;
    char k  = (counter & K)  ? 'K' : 'k';
    char p  = (counter & P)  ? 'P' : 'p';
    char se = (counter & SE) ? 'E' : 'e';
    char sn = (counter & SN) ? 'N' : 'n';
    char sm = (counter & SM) ? 'M' : 'm';
    char de = (counter & DE) ? 'E' : 'e';
    char dn = (counter & DN) ? 'N' : 'n';
    char dm = (counter & DM) ? 'M' : 'm';
    char u  = (counter & U)  ? 'U' : 'u';
    PROP( FMT(buffer, "%x%x%x%x%x%x%x%x%x",
                      FC(k), FC(p), FC(se), FC(sn), FC(sm),
                      FC(de), FC(dn), FC(dm), FC(u)) );
    return E_OK;
}

static derr_t test_key_tool(void){
    derr_t error = E_OK;

    // start the fake api server
    PROP( fas_start() );
    derr_t fas_error;

    // unsigned int counter = (K|P|SE|SM|DE|DM);
    // unsigned int counter = 128;
    // PROP_GO( do_test(counter), cu1);

    DSTR_VAR(mode, 16);
    for(unsigned int counter = 0; counter < 2*K; counter++){
        PROP_GO( counter_to_dstr(counter, &mode), cu1);
        PFMT("----- testing %x (%x) -----\n", FU(counter), FD(&mode));
        error = do_test(counter);
        if(error){
            LOG_ERROR("failed on %x (%x)\n", FU(counter), FD(&mode));
            PROP_GO(error, cu1);
        }
    }

cu1:
    fas_error = fas_join();
    if(fas_error){
        LOG_CATCH(fas_error);
        LOG_ERROR("fake pop server exited with error\n");
        if(!error) error = fas_error;
    }
    return error;
}

static derr_t test_peer_list_read_write(void){
    // remove the directory if there already is one
    derr_t error = rm_rf("_tktdir");
    CATCH(E_ANY){
        // ignore this error entirely
        error = E_OK;
    }
    // create the "test key tool directory"
    int ret = mkdir("_tktdir", 0770);
    if(ret != 0){
        LOG_ERROR("%x: %x\n", FS("_tktdir"), FE(&errno));
        ORIG(E_OS, "unable to create temporary directory");
    }

    LIST_VAR(json_t, json, 32);
    DSTR_VAR(json_block, 2048);
    LIST_VAR(dstr_t, peer_list, 32);

    /* create a fake key_tool_t that only has the objects that load_peer_list()
       and write_peer_list() will use */
    key_tool_t kt;
    kt.json = json;
    kt.json_block = json_block;
    kt.peer_list = peer_list;

    const char* old_name = "_tktdir/old_pl.json";
    const char* new_name = "_tktdir/new_pl.json";

    // create the initial peer list
    PROP( dstr_fwrite_file(old_name, &old_peer_list) );

    // read the list
    PROP( key_tool_peer_list_load(&kt, old_name) );

    // write the list
    PROP_GO( key_tool_peer_list_write(&kt, new_name), cleanup);

    // compare the files
    int result;
    PROP_GO( file_cmp(old_name, new_name, &result), cleanup);
    if(result != 0){
        ORIG_GO(E_VALUE, "peer_list files don't match", cleanup);
    }

cleanup:
    for(size_t i = 0; i < kt.peer_list.len; i++){
        dstr_free(&kt.peer_list.data[i]);
    }
    return E_OK;
}

static void sig_handler(int signum){
    if(signum == SIGINT){
        rm_rf("_tktdir");
        LOG_ERROR("FAIL (cancelled)\n");
        ssl_library_close();
        exit(1);
    }
}

int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_DEBUG);

    // setup the library (application-wide step)
    PROP_GO( ssl_library_init(), test_fail);

    signal(SIGINT, sig_handler);

    PROP_GO( test_peer_list_read_write(), test_fail);
    PROP_GO( test_key_tool(), test_fail);
    error = rm_rf("_tktdir");
    CATCH(E_ANY){
        // ignore this error entirely
        error = E_OK;
    }

    LOG_ERROR("PASS\n");
    ssl_library_close();
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    ssl_library_close();
    return 1;
}
