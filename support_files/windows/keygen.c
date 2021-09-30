#include <stdio.h>
#include <stdlib.h>

#include <libdstr/libdstr.h>

derr_t do_popen(const char* args, const dstr_t* input, dstr_t* output){
    derr_t e = E_OK;

    LOG_INFO("spawing child process: %x\n", FS(args));
    // check args
    if(input != NULL && output != NULL){
        ORIG(&e, E_PARAM, "popen can only read OR write");
    }

    char* popen_type;
    if(output){
        popen_type = "rt";
    }else{
        // if we aren't reading or writing, a write-text type popen is fine
        popen_type = "wt";
    }

    // https://msdn.microsoft.com/en-us/library/96ayss4b.aspx
    FILE* f = _popen(args, popen_type);
    if(f == NULL){
        ORIG(&e, E_OS, "popen failed");
    }

    if(input){
        // write all of input to stdin
        PROP(&e,  dstr_fwrite(f, input) );
    }else if(output){
        // read all of stdout into output
        while(true){
            size_t amnt_read;
            PROP(&e,  dstr_fread(f, output, 0, &amnt_read) );
            if(amnt_read == 0) break;
        }
    }

    if(_pclose(f)){
        ORIG(&e, E_OS, "child process failed");
    }
    LOG_INFO("child process succeeded\n");

    return e;
}

// for_each_file_in_dir2 version of:
//  rm -f "$outdir/$(echo QWER ca_name REWQ | sed -e 's/[^a-z].*//')"*.srl
static derr_t remove_srl(
    const string_builder_t *base, const dstr_t *file, bool isdir, void *arg
){
    (void)arg;
    derr_t e = E_OK;

    if(isdir) return e;
    if(!dstr_endswith(file, &DSTR_LIT(".srl"))) return e;

    // check prefix manually
    DSTR_STATIC(ca_name, "QWER ca_name REWQ");
    for(size_t i = 0; i < ca_name.len && i < file->len; i++){
        char c = ca_name.data[i];
        if(c < 'a' || c > 'z'){
            // prefix ended; this is a match
            break;
        }
        if(file->data[i] != c){
            // no match
            return e;
        }
    }

    string_builder_t path = sb_append(base, FD(file));
    PROP(&e, dunlink_path(&path) );

    return e;
}


derr_t keygen_main(int argc, char **argv){
    derr_t e = E_OK;

    // log to stdout
    logger_add_fileptr(LOG_LVL_DEBUG, stdout);

    // also log to a log file in %TEMP%
    bool ok;
    dstr_t temp = dgetenv(DSTR_LIT("TEMP"), &ok);
    DSTR_VAR(logfile, 256);
    if(!ok){
        LOG_ERROR("$TEMP not set\n");
    }else{
        IF_PROP(&e, FMT(&logfile, "%x\\splintermail-keygen.log", FD(&temp)) ){
            TRACE(&e, "unable to configure to logfile in %TEMP%\n");
            DUMP(e);
            DROP_VAR(&e);
        }else{
            logger_add_filename(LOG_LVL_DEBUG, logfile.data);
        }
    }

    LOG_DEBUG("keygen called like this:\n");
    for(int i = 0; i < argc; i++){
        LOG_DEBUG("%x\n", FS(argv[i]));
    }
    LOG_DEBUG("\n");

    // check number of arguments
    if(argc != 4){
        LOG_ERROR("wrong number of arguments!\n");
        LOG_ERROR("usage: %x OPENSSL_BIN OPENSSL_CNF OUTDIR\n", FS(argv[0]));
        exit(255);
    }

    // gather the arguments
    char* openssl_bin = argv[1];
    char* openssl_cnf = argv[2];
    char* output_dir = argv[3];

    bool gen_needed = false;
    // check if we already have the certificate authority
    DSTR_VAR(path, 256);
    PROP(&e, FMT(&path, "%xQWER ca_name REWQ", FS(output_dir)) );
    gen_needed |= !file_r_access(path.data);

    // check if we already have the key
    path.len = 0;
    PROP(&e, FMT(&path, "%xQWER key_name REWQ", FS(output_dir)) );
    gen_needed |= !file_r_access(path.data);

    // check if we already have the cert
    path.len = 0;
    PROP(&e, FMT(&path, "%xQWER cert_name REWQ", FS(output_dir)) );
    gen_needed |= !file_r_access(path.data);

    // if we don't need to continue... don't continue
    if(gen_needed == false){
        LOG_DEBUG("keygen says: nothing to do\n");
        return e;
    }

    // delete any pre-existing CA's laying around
    PROP(&e,
        do_popen(
            "\"certutil -delstore Root \"QWER ca_common_name REWQ\"\"",
            NULL,
            NULL
        )
    );
    LOG_DEBUG("removed any pre-existing splintermail certs\n");

    // do the "script" ////////////////////////////////////////////////////////

    // generate a certificate authority key, store only in memory
    DSTR_VAR(ca_key, 4096);
    DSTR_VAR(args, 1024);
    PROP(&e,
        FMT(
            &args,
            "\"\"QWER["join", "\\\" \\\"", "generate_ca_key_args"]REWQ\"\"",
            FS(openssl_bin)
        )
    );
    PROP(&e, do_popen(args.data, NULL, &ca_key) );

    // self-sign rootca, store cert as file
    args.len = 0;
    PROP(&e,
        FMT(
            &args,
            "\"\"QWER["join", "\\\" \\\"", "self_sign_ca_args"]REWQ\"\"",
            FS(openssl_bin),
            FS(openssl_cnf),
            FS(output_dir)
        )
    );
    PROP(&e, do_popen(args.data, &ca_key, NULL) );

    // create key
    args.len = 0;
    PROP(&e,
        FMT(
            &args,
            "\"\"QWER["join", "\\\" \\\"", "create_key_args"]REWQ\"\"",
            FS(openssl_bin),
            FS(output_dir)
        )
    );
    PROP(&e, do_popen(args.data, NULL, NULL) );

    // create certificate sign request
    args.len = 0;
    PROP(&e,
        FMT(
            &args,
            "\"\"QWER["join", "\\\" \\\"", "create_csr_args"]REWQ\"\"",
            FS(openssl_bin),
            FS(openssl_cnf),
            FS(output_dir),
            FS(output_dir)
        )
    );
    PROP(&e, do_popen(args.data, NULL, NULL) );

    // sign with rootca
    args.len = 0;
    PROP(&e,
        FMT(
            &args,
            "\"\"QWER["join", "\\\" \\\"", "sign_csr_args"]REWQ\"\"",
            FS(openssl_bin),
            FS(openssl_cnf),
            FS(output_dir),
            FS(output_dir),
            FS(output_dir)
        )
    );
    PROP(&e, do_popen(args.data, &ca_key, NULL) );

    // turn the certificate into a proper chain
    DSTR_VAR(ca_cert_file, 1024);
    PROP(&e, FMT(&ca_cert_file, "%xQWER ca_name REWQ", FS(output_dir)) );
    DSTR_VAR(ca_cert, 4096);
    PROP(&e, dstr_read_file(ca_cert_file.data, &ca_cert) );

    DSTR_VAR(ssl_cert_file, 1024);
    PROP(&e, FMT(&ssl_cert_file, "%xQWER cert_name REWQ", FS(output_dir)) );
    FILE* f = compat_fopen(ssl_cert_file.data, "a");
    PROP(&e, dstr_fwrite(f, &ca_cert) );
    PROP(&e, dffsync(f) );
    fclose(f);

    // trust the new CA
    args.len = 0;
    PROP(&e,
        FMT(
            &args,
            "\"certutil -addstore Root \"%xQWER ca_name REWQ\"\"",
            FS(output_dir)
        )
    );
    PROP(&e, do_popen(args.data, NULL, NULL) );
    LOG_DEBUG("trusted local certificate authority\n");


    // cleanup unecessary files
    {
        // rm -f "$outdir/$(echo QWER ca_name REWQ | sed -e 's/[^a-z].*//')"*.srl
        string_builder_t output_path = SB(FS(output_dir));
        PROP(&e, for_each_file_in_dir2(&output_path, remove_srl, NULL));
    }
    {
        // rm -f "$outdir/sig_req.csr"
        DSTR_VAR(fname, 1024);
        PROP(&e, FMT(&fname, "%xsig_req.csr", FS(output_dir)) );
        PROP(&e, dremove(fname.data) );
    }

    return e;
}

/* arguments are:
    path\to\openssl.exe
    path\to\openssl.cnf
    path\to\output\dir\ (with trailing backslash)
*/
int main(int argc, char** argv) {
    derr_t e = E_OK;
    IF_PROP(&e, keygen_main(argc, argv) ){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }
    return 0;
}
