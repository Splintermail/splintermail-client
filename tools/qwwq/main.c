#include "libdstr/libdstr.h"
#include "tools/qwwq/libqw.h"

#include <stdio.h>
#include <errno.h>

static void print_help(void){
    fprintf(stdout,
        "qwwq: a template processor based on the qwwq language\n"
        "\n"
        "usage: qwwq [OPTIONS] CONFIG [DYNAMIC...]\n"
        "\n"
        "OPTIONS:\n"
        "  -h, --help      Print help information and usage.\n"
        "  -i, --input     Specify an input file.  Default: stdin.\n"
        "  -o, --output    Specify an output file.  Default: stdout.\n"
        #ifndef _WIN32
        "      --mode      Specify an output file mode.  Default: copy mode\n"
        "                  from --input file, if provided.\n"
        #endif
        "  -s, --stamp     Specify a stamp file.  In cases where the output\n"
        "                  file would be unchanged, touch the stamp file\n"
        "                  instead.  Requires --output.  Default: none.\n"
        "  -b  --boundary  Specify alternate snippet boundaries in PRE:POST\n"
        "                  format.  Default: QW:WQ\n"
        "      --stack     Parser stack size.  Default: 4096\n"
        "  -p, --plugins   Specify a plugin search path.  May be provided\n"
        "                  multiple times.  Default: none.\n"
        "  -d, --debug     Enable debug-level logs.\n"
        "\n"
        "CONFIG must be a qwwq-code file.\n"
        "\n"
        "The --input file may contain QW...WQ snippets of qwwq-code,\n"
        "each of which must evaluate to a string, and after evaluation will\n"
        "be embedded into the output.\n"
        "\n"
        "DYNAMIC values are of the form KEY=VAL, and will be available to\n"
        "snippets as if they were set on the top-level dictionary.  If there\n"
        "is a pre-existing key, the command-line KEY will be preferred.\n"
    );
}

static derr_t path_cb(void *data, dstr_t val){
    derr_t e = E_OK;
    LIST(dstr_t) *paths = data;
    IF_PROP(&e, LIST_APPEND(dstr_t, paths, val) ){
        fprintf(stderr, "too many --modpath args\n");
        return e;
    }
    return e;
}

int main(int argc, char **argv){
    LIST_VAR(dstr_t, paths, 8);

    opt_spec_t o_help  = {'h',  "help",     false};
    opt_spec_t o_in    = {'i',  "input",    true };
    opt_spec_t o_out   = {'o',  "output",   true };
    #ifndef _WIN32
    opt_spec_t o_mode  = {'m',  "mode",     true };
    #endif
    opt_spec_t o_stamp = {'s',  "stamp",    true };
    opt_spec_t o_bound = {'b',  "boundary", true };
    opt_spec_t o_stack = {'\0', "stack",    true };
    opt_spec_t o_plug  = {'p',  "plugins",  true, path_cb, &paths};
    opt_spec_t o_debug = {'\0', "debug",    false};

    opt_spec_t* spec[] = {
        &o_help,
        &o_in,
        &o_out,
        #ifndef _WIN32
        &o_mode,
        #endif
        &o_stamp,
        &o_bound,
        &o_stack,
        &o_plug,
        &o_debug,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;

    // parse options
    derr_t e2 = opt_parse(argc, argv, spec, speclen, &newargc);
    CATCH_ANY(&e2){
        DROP_VAR(&e2);
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    // help option
    if(o_help.found){
        print_help();
        return 0;
    }

    // invalid positional arguments
    if(newargc < 2){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    // --stamp requires --output
    if(o_stack.found && !o_out.found){
        fprintf(stderr, "--stamp requires --output\n");
        return 1;
    }

    // handle --debug
    if(o_debug.found){
        DROP_CMD(logger_add_fileptr(LOG_LVL_DEBUG, stderr));
    }

    char *confpath = argv[1];

    // handle --boundary
    DSTR_STATIC(colon, ":");
    dstr_t pre = DSTR_LIT("QW");
    dstr_t post = DSTR_LIT("WQ");
    if(o_bound.found){
        if(dstr_count2(o_bound.val, colon) != 1){
            FFMT_QUIET(stderr,
                "invalid --boundary string: %x\n", FD(o_bound.val)
            );
            return 1;
        }
        dstr_split2_soft(o_bound.val, colon, NULL, &pre, &post);
    }

    // handle --stack
    size_t stack = 4096;
    if(o_stack.found){
        if(dstr_tosize_quiet(o_stack.val, &stack, 10) != E_NONE){
            FFMT_QUIET(stderr,
                "invalid --stack size: %x\n", FD(o_stack.val)
            );
            return 1;
        }
    }

    #ifndef _WIN32
    mode_t mode = 0;
    bool set_mode = false;
    // handle --mode
    if(o_mode.found){
        set_mode = true;
        unsigned int umode;
        derr_type_t etype = dstr_tou_quiet(o_mode.val, &umode, 8);
        if(etype || umode > 0777){
            FFMT_QUIET(stderr, "invalid --mode: %x\n", FD(o_mode.val));
            return 1;
        }
        mode = (mode_t)umode;
    }
    #endif

    derr_t e = E_OK;

    qw_dynamics_t dynamics = {0};
    dstr_t conf = {0};
    dstr_t templ = {0};
    dstr_t out = {0};
    FILE *fconf = NULL;
    FILE *ftempl = NULL;
    FILE *fout = NULL;

    // read dynamics
    PROP_GO(&e,
        qw_dynamics_init(&dynamics, argv + 2, (size_t)(newargc - 2)),
    cu);

    // read config file
    PROP_GO(&e, dfopen(confpath, "r", &fconf), cu);
    PROP_GO(&e, dstr_fread_all(fconf, &conf), cu);
    fclose(fconf); fconf = NULL;

    dstr_t confdirname = ddirname(dstr_from_cstr(confpath));

    // read template file (or stdin)
    dstr_t templdirname = {0};
    if(o_in.found){
        PROP_GO(&e, dfopen(o_in.val.data, "r", &ftempl), cu);
        templdirname = ddirname(o_in.val);
    }else{
        ftempl = stdin;
    }
    PROP_GO(&e, dstr_fread_all(ftempl, &templ), cu);
    #ifndef _WIN32
    if(!o_mode.found && o_out.found && o_in.found){
        // unix only: match output mode to input mode
        set_mode = true;
        struct stat s;
        PROP_GO(&e, dfstat(fileno(ftempl), &s), cu);
        mode = s.st_mode & 0777;
    }
    #endif
    fclose(ftempl); ftempl = NULL;

    // calculate output
    PROP_GO(&e,
        qwwq(
            conf,
            &confdirname,
            dynamics,
            paths,
            templ,
            o_in.found ? &templdirname : NULL,
            pre,
            post,
            stack,
            &out
        ),
    cu);

    if(o_stamp.found){
        // check if output already exists
        bool ok;
        PROP_GO(&e, dexists(o_out.val.data, &ok), cu);
        if(!ok) goto write_output;

        // try to read output
        PROP_GO(&e, dfopen(o_out.val.data, "r", &fout), cu);

        #ifndef _WIN32
        // unix only: check output mode
        if(set_mode){
            struct stat s;
            PROP_GO(&e, dfstat(fileno(fout), &s), cu);
            if((s.st_mode & 0777) != mode){
                fclose(fout); fout = NULL;
                goto write_output;
            }
        }
        #endif

        // reuse conf, we don't need that memory anymore
        conf.len = 0;
        PROP_GO(&e, dstr_fread_all(fout, &conf), cu);
        fclose(fout); fout = NULL;

        if(!dstr_eq(out, conf)) goto write_output;

        // no changes, touch stampfile and exit
        fprintf(stderr, "no changes, not overwriting\n");
        PROP_GO(&e, touch(o_stamp.val.data), cu);
        goto cu;
    }

write_output:
    // write output to file (or stdout)
    if(o_out.found){
        PROP_GO(&e, dfopen(o_out.val.data, "w", &fout), cu);
    }else{
        fout = stdout;
    }
    PROP_GO(&e, dstr_fwrite(fout, &out), cu);

    #ifndef _WIN32
    // unix only: match output mode to input mode
    if(set_mode){
        int ret = fchmod(fileno(fout), mode);
        if(ret){
            ORIG_GO(&e, E_OS, "chmod(%x): %x", cu, FD(o_out.val), FE(errno));
        }
    }
    #endif // _WIN32
    FILE *temp = fout; fout = NULL;
    PROP_GO(&e, dfclose(temp), cu);

cu:
    qw_dynamics_free(&dynamics);
    dstr_free(&conf);
    dstr_free(&templ);
    dstr_free(&out);
    if(fconf) fclose(fconf);
    if(ftempl) fclose(ftempl);
    if(fout) fclose(fout);

    if(!is_error(e)) return 0;

    if(e.type == E_USERMSG){
        DSTR_VAR(buf, 256);
        consume_e_usermsg(&e, &buf);
        FFMT_QUIET(stderr, "%x\n", FD(buf));
        return 1;
    }

    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
