#include "libdstr/libdstr.h"
#include "tools/qwerrewq/libqw.h"

#include <stdio.h>

static void print_help(void){
    fprintf(stdout,
        "qwerrewq: a template processor based\n"
        "\n"
        "usage: qwerrewq [OPTIONS] CONFIG [TEMPLATE]\n"
        "\n"
        "OPTIONS:\n"
        "  -h, --help      Print help information and usage.\n"
        "  -o, --output    Specify an output file.  Default: stdout.\n"
        "  -b  --boundary  Specify alternate snippet boundaries in PRE:POST\n"
        "                  format.  Default: QWER:REWQ\n"
        "\n"
        "CONFIG must be a qwerrweq-code file.\n"
        "\n"
        "TEMPLATE may contain QWER...REWQ snippets of qwerrweq-code, each of\n"
        "which must evaluate to a string, and after evaluation will be\n"
        "embedded into the output."
    );
}

int main(int argc, char **argv){
    opt_spec_t o_help  = {'h', "help",     false};
    opt_spec_t o_out   = {'o', "output",   true };
    opt_spec_t o_bound = {'b', "boundary", true };

    opt_spec_t* spec[] = {
        &o_help,
        &o_out,
        &o_bound,
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
    if(newargc < 2 || newargc > 3){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    char *confpath = argv[1];
    char *templpath = argv[2]; // might be NULL

    // handle --boundary
    DSTR_STATIC(colon, ":");
    dstr_t pre = DSTR_LIT("QWER");
    dstr_t post = DSTR_LIT("REWQ");
    if(o_bound.found){
        if(dstr_count2(o_bound.val, colon) != 1){
            FFMT_QUIET(stderr, NULL,
                "invalid --boundary string: %x\n", FD(&o_bound.val)
            );
            return 1;
        }
        dstr_split2_soft(o_bound.val, colon, NULL, &pre, &post);
    }

    derr_t e = E_OK;

    dstr_t conf = {0};
    dstr_t templ = {0};
    dstr_t out = {0};
    FILE *fconf = NULL;
    FILE *ftempl = NULL;
    FILE *fout = NULL;

    // read config file
    PROP_GO(&e, dfopen(confpath, "r", &fconf), cu);
    PROP_GO(&e, dstr_fread_all(fconf, &conf), cu);
    fclose(fconf); fconf = NULL;

    // reat template file (or stdin)
    if(templpath){
        PROP_GO(&e, dfopen(templpath, "r", &ftempl), cu);
    }else{
        ftempl = stdin;
    }
    PROP_GO(&e, dstr_fread_all(ftempl, &templ), cu);
    fclose(ftempl); ftempl = NULL;

    // calculate output
    PROP_GO(&e, qwerrewq(conf, templ, pre, post, &out), cu);

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
