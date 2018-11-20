#include "logger.h"
#include "imap_parse.h"

// static derr_t parse_status_type(ixs_t *ixs, const dstr_t *in);
// static derr_t parse_capability(ixs_t *ixs, const dstr_t *in);
// static derr_t parse_list(ixs_t *ixs, const dstr_t *in);
// static derr_t parse_status(ixs_t *ixs, const dstr_t *in);
// static derr_t parse_fetch(ixs_t *ixs, const dstr_t *in);

// for parsing the tag from the beginning of a line
// *in will get reset to be a substring itself, so it shouldn't be the original
static derr_t parse_tag(dstr_t *in, dstr_t *tag, bool *done){
    // tag characters: anything but ( ) { % * " \ + %x00-%x20 %x7f
    for(size_t i = 0; i < in->len; i++){
        // make sure we haven't exceeded the tag size
        if(tag->len == tag->size){
            ORIG(E_FIXEDSIZE, "tag too long");
        }
        // otherwise get the next character from dstr_t
        unsigned char u = (unsigned char)in->data[i];
        // detect invalid characters
        if(u < ' ' || u == '"' || u == '%' || u == '(' || u == ')' || u == '*'
                   || u == '+' || u == '\\' || u == '{' || u == 127){
            ORIG(E_PARAM, "invalid character in tag");
        }
        // detect end-of-tag-character
        if(u == ' '){
            // make sure we have a tag
            if(tag->len == 0){
                ORIG(E_PARAM, "tag not found");
            }
            *done = true;
            // set *in to be the unused portion of itself, consuming the space
            *in = dstr_sub(in, i + 1, 0);
            return E_OK;
        }
        // append character to tag
        tag->data[tag->len++] = in->data[i];
    }
    // end of input, return false;
    *done = false;
    return E_OK;
}

// for parsing either a number or an IMAP response type (such as "EXISTS")
static derr_t parse_simple_token(dstr_t *in, dstr_t *out, bool *done){
    for(size_t i = 0; i < in->len; i++){
        // make sure we haven't exceeded the output size
        if(out->len == out->size){
            ORIG(E_FIXEDSIZE, "simple token too long");
        }
        unsigned char u = (unsigned char)in->data[i];
        // allow letters and numbers
        if((u >= 'a' && u <= 'z') || (u >= 'a' && u <= 'z')
                                  || (u >= '0' && u <= '9')){
            out->data[out->len++] = in->data[i];
        }
        // detect end-of-token-characters
        else if(u == ' ' || u == '\r' || u == '\n'){
            // make sure we have a token
            if(out->len == 0){
                ORIG(E_PARAM, "no token found");
            }
            *done = true;
            // set *in to be the unused portion of itself
            if(u == ' '){
                // "consume" the space
                *in = dstr_sub(in, i + 1, 0);
            }else{
                // but leave the "\r" or "\n" alone
                *in = dstr_sub(in, i, 0);
            }
            return E_OK;
        }else{
            ORIG(E_PARAM, "invalid character in simple token");
        }
    }
    return E_OK;
}

static derr_t is_num(const dstr_t *in){
    for(size_t i = 0; i < in->len; i++){
        unsigned char u = (unsigned char)in->data[i];
        if(u < '0' || u > '9'){
            return false;
        }
    }
    return true;
}

static derr_t get_response_type(const dstr_t *in, imap_response_type_t *type){
    if(dstr_cmp(in, &DSTR_LIT("OK")) == 0){
        *type = IMAP_RESP_TYPE_OK;
    }else if(dstr_cmp(in, &DSTR_LIT("NO")) == 0){
        *type = IMAP_RESP_TYPE_NO;
    }else if(dstr_cmp(in, &DSTR_LIT("BAD")) == 0){
        *type = IMAP_RESP_TYPE_BAD;
    }else if(dstr_cmp(in, &DSTR_LIT("PREAUTH")) == 0){
        *type = IMAP_RESP_TYPE_PREAUTH;
    }else if(dstr_cmp(in, &DSTR_LIT("BYE")) == 0){
        *type = IMAP_RESP_TYPE_BYE;
    }else if(dstr_cmp(in, &DSTR_LIT("CAPABILITY")) == 0){
        *type = IMAP_RESP_TYPE_CAPABILITY;
    }else if(dstr_cmp(in, &DSTR_LIT("LIST")) == 0){
        *type = IMAP_RESP_TYPE_LIST;
    }else if(dstr_cmp(in, &DSTR_LIT("LSUB")) == 0){
        *type = IMAP_RESP_TYPE_LSUB;
    }else if(dstr_cmp(in, &DSTR_LIT("STATUS")) == 0){
        *type = IMAP_RESP_TYPE_STATUS;
    }else if(dstr_cmp(in, &DSTR_LIT("FLAGS")) == 0){
        *type = IMAP_RESP_TYPE_FLAGS;
    }else if(dstr_cmp(in, &DSTR_LIT("SEARCH")) == 0){
        *type = IMAP_RESP_TYPE_SEARCH;
    }else if(dstr_cmp(in, &DSTR_LIT("EXISTS")) == 0){
        *type = IMAP_RESP_TYPE_EXISTS;
    }else if(dstr_cmp(in, &DSTR_LIT("RECENT")) == 0){
        *type = IMAP_RESP_TYPE_RECENT;
    }else if(dstr_cmp(in, &DSTR_LIT("EXPUNGE")) == 0){
        *type = IMAP_RESP_TYPE_EXPUNGE;
    }else if(dstr_cmp(in, &DSTR_LIT("FETCH")) == 0){
        *type = IMAP_RESP_TYPE_FETCH;
    }else{
        ORIG(E_PARAM, "unrecognized response type");
    }
    return E_OK;
}

derr_t parse_status_type(ixpu_t *ixpu, const dstr_t *in);

derr_t imap_parse_response(ixpu_t *ixpu, const dstr_t *in){
    // set unused to be all of *in
    dstr_t unused = dstr_sub(in, 0, 0);
    bool done;
    while(unused.len > 0){
        // continue parsing where we left off (or start fresh)
        switch(ixpu->response_type){

            case IMAP_RESP_TYPE_PRETAG:
                PROP( parse_tag(&unused, &ixpu->tag, &done) );
                // did we finish the token or do we need another read?
                if(!done) return E_OK;
                // TODO: check if tag is string literal continuation ("+")
                ixpu->response_type = IMAP_RESP_TYPE_UNKNOWN;
                break;

            case IMAP_RESP_TYPE_UNKNOWN: // for when we haven't parsed it yet
                PROP( parse_simple_token(&unused, &ixpu->buffer, &done) );
                if(!done) return E_OK;
                // did we need a number AND find one?
                if(!ixpu->has_num && is_num(&ixpu->buffer)){
                    // store the number
                    PROP( dstr_tou(&ixpu->buffer, &ixpu->num, 10) );
                    ixpu->has_num = true;
                    // get the next token, for the response type
                    ixpu->buffer.len = 0;
                    PROP( parse_simple_token(&unused, &ixpu->buffer, &done) );
                    if(!done) return E_OK;
                }
                // figure out what type of response we have
                dstr_upper(&ixpu->buffer);
                PROP( get_response_type(&ixpu->buffer, &ixpu->response_type) );
                // done with contents of buffer
                ixpu->buffer.len = 0;
                // make sure if response is tagged that it is OK, NO, or BAD
                if(dstr_cmp(&ixpu->tag, &DSTR_LIT("*")) != 0
                        && ixpu->response_type != IMAP_RESP_TYPE_OK
                        && ixpu->response_type != IMAP_RESP_TYPE_NO
                        && ixpu->response_type != IMAP_RESP_TYPE_BAD){
                    ORIG(E_PARAM, "only OK, NO, or BAD can be tagged");
                }
                break;

            // status-type responses are all parsed in the same way
            case IMAP_RESP_TYPE_OK:
            case IMAP_RESP_TYPE_NO:
            case IMAP_RESP_TYPE_BAD:
            case IMAP_RESP_TYPE_PREAUTH:
            case IMAP_RESP_TYPE_BYE:
                PROP( parse_status_type(&unused) );
                break;

            case IMAP_RESP_TYPE_CAPABILITY:
                break;

            case IMAP_RESP_TYPE_LIST:
                break;

            case IMAP_RESP_TYPE_LSUB:
                break;

            case IMAP_RESP_TYPE_STATUS:
                break;

            case IMAP_RESP_TYPE_FLAGS:
                break;

            case IMAP_RESP_TYPE_SEARCH:
                break;

            case IMAP_RESP_TYPE_EXISTS:
                break;

            case IMAP_RESP_TYPE_RECENT:
                break;

            case IMAP_RESP_TYPE_EXPUNGE:
                break;

            case IMAP_RESP_TYPE_FETCH:
                break;
        }
    }
    return E_OK;
}
