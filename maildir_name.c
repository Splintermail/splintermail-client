#include "maildir_name.h"
#include "logger.h"

// *val is allowed to be NULL, which makes this a validation function
derr_t maildir_name_parse_meta_val(const dstr_t *flags, msg_meta_value_t *val){
    derr_t e = E_OK;

    msg_meta_value_t temp = {0};

    for(size_t i = 0; i < flags->len; i++){
        char c = flags->data[i];
        switch(c){
            case 'A': temp.answered = true; break;
            case 'F': temp.flagged  = true; break;
            case 'S': temp.seen     = true; break;
            case 'D': temp.draft    = true; break;
            case 'X': temp.deleted  = true; break;
            default:
                TRACE(&e, "invalid flag %x\n", FC(c));
                ORIG(&e, E_PARAM, "invalid flag");
        }
    }

    if(val == NULL) return e;

    *val = temp;

    return e;
}

/* only *name is required to be non-NULL, in which case this becomes a
   validation function */
derr_t maildir_name_parse(const dstr_t *name, unsigned long *epoch,
        size_t *len, unsigned int *uid, msg_meta_value_t *val, dstr_t *host,
        dstr_t *info){
    derr_t e = E_OK;

    /* Maildir format: (cr.yp.to/proto/maildir.html)

       maildir filename = UNIQ[:INFO]

       INFO = (controlled by MUA, we just preserve it)

       UNIQ = EPOCH.DELIV_ID.HOST

       EPOCH = epoch seconds

       HOST = hostname modified to not contain '/' or ':'

       // we define our own unique delivery id:
       DELIV_ID = LEN,UID,FLAGS

       LEN = the length of the file

       UID = UID of the message

       // the imap flags of the message, which we control
       FLAGS = [A][F][S][D][X]

       // example:                          variables:
       0123456789.522,3,AS.my.computer:2,
       -------------------------------|--   major_tokens (1 or 2)
       ----------|--------|-----------      minor_tokens (always 3)
                  ---|-|--                  fields (always 3)
    */

    // first just check to make sure we have met a minimum length
    if(name->len < 16){
        // this filename is too short for an maildir name
        return e;
    }

    // split major tokens
    LIST_VAR(dstr_t, major_tokens, 2);
    DSTR_STATIC(colon, ":");
    derr_t e2 = dstr_split(name, &colon, &major_tokens);
    CATCH(e2, E_FIXEDSIZE){
        TRACE(&e2, "too many major tokens");
        RETHROW(&e, &e2, E_PARAM);
    }else PROP(&e, e2);

    // split minor tokens (soft split, so hostname can contain '.'
    LIST_VAR(dstr_t, minor_tokens, 3);
    DSTR_STATIC(dot, ".");
    PROP(&e, dstr_split_soft(&major_tokens.data[0], &dot, &minor_tokens) );

    if(minor_tokens.len != 3){
        ORIG(&e, E_PARAM, "wrong number of minor tokens");
    }

    // split fields
    LIST_VAR(dstr_t, fields, 3);
    DSTR_STATIC(comma, ",");
    e2 = dstr_split(&minor_tokens.data[1], &comma, &fields);
    CATCH(e2, E_FIXEDSIZE){
        TRACE(&e2, "too many fields");
        RETHROW(&e, &e2, E_PARAM);
    }else PROP(&e, e2);

    if(fields.len != 3){
        ORIG(&e, E_PARAM, "wrong number of fields");
    }

    // parse epoch
    unsigned long temp_epoch;
    PROP(&e, dstr_toul(&minor_tokens.data[0], &temp_epoch, 10) );
    if(epoch != NULL) *epoch = temp_epoch;

    // first field is the length
    size_t temp_len;
    PROP(&e, dstr_tosize(&fields.data[0], &temp_len, 10) );

    if(len != NULL) *len = temp_len;

    // second field is uid
    unsigned int temp_uid;
    PROP(&e, dstr_tou(&fields.data[1], &temp_uid, 10) );

    if(uid != NULL) *uid = temp_uid;

    // third field is imap flags (since MUA is responsible for maildir flags)
    PROP(&e, maildir_name_parse_meta_val(&fields.data[2], val) );

    // report hostname if requested
    if(host != NULL){
        PROP(&e, dstr_copy(&minor_tokens.data[2], host) );
    }

    // report info if requested
    if(info != NULL){
        // info is not always present
        if(major_tokens.len == 2){
            PROP(&e, dstr_copy(&major_tokens.data[1], info) );
        }else{
            info->len = 0;
        }
    }

    return e;
}

/* modded_hostname replaces '/' with "057"
                        and ':' with "072"
   The standard says to preface the codes with a backslash like '\057' but I
   think that may break on windows, so I just dropped that character.
*/
derr_t maildir_name_mod_hostname(const dstr_t* host, dstr_t *out){
    derr_t e = E_OK;

    LIST_PRESET(dstr_t, search,  DSTR_LIT("/"),   DSTR_LIT(":")   );
    LIST_PRESET(dstr_t, replace, DSTR_LIT("057"), DSTR_LIT("072") );
    bool append = true;
    PROP(&e, dstr_recode(host, out, &search, &replace, append) );

    return e;
}

// info and val are allowed to be NULL, but not host
derr_t maildir_name_write(dstr_t *out, unsigned long epoch, size_t len,
        unsigned int uid, const msg_meta_value_t *val, const dstr_t *host,
        const dstr_t *info){
    derr_t e = E_OK;

    PROP(&e, FMT(out, "%x.%x,%x,", FU(epoch), FU(len), FU(uid)) );

    if(val){
        if(val->answered){ PROP(&e, dstr_append(out, &DSTR_LIT("A")) ); }
        if(val->draft){    PROP(&e, dstr_append(out, &DSTR_LIT("D")) ); }
        if(val->flagged){  PROP(&e, dstr_append(out, &DSTR_LIT("F")) ); }
        if(val->seen){     PROP(&e, dstr_append(out, &DSTR_LIT("S")) ); }
        if(val->deleted){  PROP(&e, dstr_append(out, &DSTR_LIT("X")) ); }
    }

    PROP(&e, dstr_append(out, &DSTR_LIT(".")) );
    PROP(&e, maildir_name_mod_hostname(host, out) );

    if(info != NULL && info->len > 0){
        PROP(&e, FMT(out, ":%x", FD(info)) );
    }

    return e;
}
