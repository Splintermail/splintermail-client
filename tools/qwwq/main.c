#include "libdstr/libdstr.h"
#include "tools/qwwq/libqw.h"

#include <stdio.h>

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
        "  -b  --boundary  Specify alternate snippet boundaries in PRE:POST\n"
        "                  format.  Default: QW:WQ\n"
        "      --stack     Parser stack size.  Default: 4096\n"
        "\n"
        "CONFIG must be a qwwq-code file.\n"
        "\n"
        "The --input file may contain QW...WQ snippets of qwwq-code,\n"
        "each of which must evaluate to a string, and after evaluation will\n"
        "be embedded into the output.\n"
        "\n"
        "DYNAMIC values are of the form KEY=VAL, and will be available to\n"
        "snippets as if they were set on the top-level dictionary.  If there\n"
        "is a pre-existing key, the command-line KEY will be preferred."
    );
}

int main(int argc, char **argv){
    opt_spec_t o_help  = {'h',  "help",     false};
    opt_spec_t o_in    = {'i',  "input",    true };
    opt_spec_t o_out   = {'o',  "output",   true };
    opt_spec_t o_bound = {'b',  "boundary", true };
    opt_spec_t o_stack = {'\0', "stack",    true };

    opt_spec_t* spec[] = {
        &o_help,
        &o_in,
        &o_out,
        &o_bound,
        &o_stack,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;

    // parse options
    derr_t e2 = opt_parse(argc, argv, spec, speclen, &newargc);
    CATCH(e2, E_ANY){
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

    char *confpath = argv[1];

    // handle --boundary
    DSTR_STATIC(colon, ":");
    dstr_t pre = DSTR_LIT("QW");
    dstr_t post = DSTR_LIT("WQ");
    if(o_bound.found){
        if(dstr_count2(o_bound.val, colon) != 1){
            FFMT_QUIET(stderr, NULL,
                "invalid --boundary string: %x\n", FD(&o_bound.val)
            );
            return 1;
        }
        dstr_split2_soft(o_bound.val, colon, NULL, &pre, &post);
    }

    // handle --stack
    size_t stack = 4096;
    if(o_stack.found){
        if(dstr_tosize_quiet(o_stack.val, &stack, 10) != E_NONE){
            FFMT_QUIET(stderr, NULL,
                "invalid --stack size: %x\n", FD(&o_stack.val)
            );
            return 1;
        }
    }

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
    fclose(ftempl); ftempl = NULL;

    // calculate output
    PROP_GO(&e,
        qwwq(
            conf,
            &confdirname,
            dynamics,
            templ,
            o_in.found ? &templdirname : NULL,
            pre,
            post,
            stack,
            &out
        ),
    cu);

    // write output to file (or stdout)
    if(o_out.found){
        PROP_GO(&e, dfopen(o_out.val.data, "w", &fout), cu);
    }else{
        fout = stdout;
    }
    PROP_GO(&e, dstr_fwrite(fout, &out), cu);
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
        FFMT_QUIET(stderr, NULL, "%x\n", FD(&buf));
        return 1;
    }

    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
