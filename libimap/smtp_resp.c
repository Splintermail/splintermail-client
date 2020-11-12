#include <libimap/libimap.h>

#include <smtp_resp.tab.h>


static derr_t smtp_resp_parser_init(
    smtp_resp_parser_t *p, const dstr_t *bytes
){
    derr_t e = E_OK;

    *p = (smtp_resp_parser_t){
        .error=E_OK,
        .bytes=bytes,
    };

    return e;
}

static void smtp_resp_parser_free(smtp_resp_parser_t *p){
    DROP_VAR(&p->error);
    ie_dstr_free(p->resp.text);
    *p = (smtp_resp_parser_t){0};
}

void smtp_resp_free(smtp_resp_t *resp){
    ie_dstr_free(resp->text);
    *resp = (smtp_resp_t){0};
}

int smtp_resp_yylex(dstr_t *llocp, smtp_resp_parser_t *p){
    size_t start = p->idx;

    // token types are any of:  SP \r \n . -
    // or RAW, aka any chain of not-the-above
    int type;

    #define NEXT() p->bytes->data[p->idx++]
    #define END() (p->idx >= p->bytes->len)

    // EOF check
    if(END()){
        type = -1;
        goto done;
    }

    int c = NEXT();
    switch(c){
        case '\r':
        case '\n':
        case ' ':
        case '-':
        case '.':
            type = c;
            goto done;
        default:
            // the start of a RAW chunk
            type = RAW;
    }

    // read the full RAW chunk
    for(c = NEXT(); !END(); c = NEXT()){
        switch(c){
            case '\r':
            case '\n':
            case ' ':
            case '-':
            case '.':
                // we went too far!
                p->idx--;
                goto done;
        }
    }
    // EOF is the boundary of the RAW token

done:
    // handle the goofy zero length case
    if(p->idx == 0){
        *llocp = (dstr_t){0};
    }else{
        *llocp = dstr_sub(p->bytes, start, p->idx);
    }
    return type;
}


void smtp_resp_yyerror(dstr_t *yyloc, smtp_resp_parser_t *p, char const *str){
    if(p->freeing) return;
    // prepare message buffer
    DSTR_VAR(buf, 64);
    DROP_CMD( FMT(&buf, "%x at location: %x", FS(str), FD_DBG(yyloc)) );
    // truncate message
    if(buf.len == buf.size){
        buf.len = MIN(buf.len, buf.size - 4);
        DROP_CMD( FMT(&buf, "...") );
    }

    /* smtp clients immediately raise a derr_t error; we only talk to dovecot
       so in practice any parsing error as a client is our bug */
    TRACE(&p->error, "%x\n", FD(&buf));
    TRACE_ORIG(&p->error, E_RESPONSE, "error parsing server response");
}


derr_t smtp_resp_parse(const dstr_t *buf, bool *more, smtp_resp_t *r){
    derr_t e = E_OK;
    *more = false;
    *r = (smtp_resp_t){0};

    smtp_resp_parser_t p;
    smtp_resp_parser_init(&p, buf);

    smtp_resp_yypstate *yyps = smtp_resp_yypstate_new();
    if(!yyps){
        ORIG_GO(&e, E_NOMEM, "nomem", cu_parser);
    }

    // handle empty buffers correctly by starting with YYPUSH_MORE
    int yyret = YYPUSH_MORE;
    int type;
    dstr_t token;
    do {
        type = smtp_resp_yylex(&token, &p);
        if(type < 0) break;
        yyret = smtp_resp_yypush_parse(yyps, type, NULL, &token, &p);
    } while(yyret == YYPUSH_MORE);

    // detect errors from the parser first
    PROP_VAR_GO(&e, &p.error, cu_yyps);

    switch(yyret){
        case 0:
            // YYACCEPT: parsing completed successfully
            if(smtp_resp_yylex(&token, &p) != -1){
                ORIG_GO(&e,
                    E_RESPONSE, "extra tokens after response",
                cu_yyps);
            }
            *r = STEAL(smtp_resp_t, &p.resp);
            *more = false;
            break;

        case YYPUSH_MORE:
            // incomplete buffer, but not an error
            *more = true;
            break;

        case 2:
            // memory exhaustion; parser is reset
            ORIG_GO(&e, E_NOMEM, "memory exhaustion", cu_yyps);

        case 1:
        default:
            // YYABORT or syntax invalid; parser is reset
            ORIG_GO(&e,
                E_INTERNAL, "invalid input, but no error was thrown",
            cu_yyps);
    }

cu_yyps:
    if(yyps){
        p.freeing = true;
        // push an invalid token to the parser to get it to reset its state
        token = (dstr_t ){0};
        smtp_resp_yypush_parse(yyps, SMTP_RESP_YYEOF, NULL, &token, &p);
        smtp_resp_yypstate_delete(yyps);
    }

cu_parser:
    smtp_resp_parser_free(&p);

    return e;
}
