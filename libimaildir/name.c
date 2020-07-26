#include "libimaildir.h"

static unsigned int deliv_id_ver = 1;

/* only *name is required to be non-NULL, in which case this becomes a
   validation function */
derr_t maildir_name_parse(const dstr_t *name, unsigned long *epoch,
        unsigned int *uid_up, size_t *len, dstr_t *host, dstr_t *info){
    derr_t e = E_OK;

    /* Maildir format: (cr.yp.to/proto/maildir.html)

       maildir filename = UNIQ[:INFO]

       INFO = (controlled by MUA, we just preserve it)

       UNIQ = EPOCH.DELIV_ID.HOST

       EPOCH = epoch seconds

       HOST = hostname modified to not contain '/' or ':'

       // we define our own unique delivery id:
       DELIV_ID = VER.UIDUP.LEN

       VER = version of the delivery id

       LEN = length of the message body

       UIDUP = uid_up of the message

       // example:                          variables:
       0123456789.1,522,3.my.computer:2,
       ------------------------------|--   major_tokens (1 or 2)
       ----------|-------|-----------      minor_tokens (always 3)
                  -|---|-                  fields (at least 3)
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
    PROP(&e, dstr_split_soft(&minor_tokens.data[1], &comma, &fields) )

    // check the version
    unsigned int version;
    PROP(&e, dstr_tou(&fields.data[0], &version, 10) );
    if(version != deliv_id_ver){
        ORIG(&e, E_INTERNAL, "unallowed delivery identifier version");
    }

    if(fields.len != 3){
        ORIG(&e, E_PARAM, "wrong number of fields");
    }

    // parse epoch
    unsigned long temp_epoch;
    PROP(&e, dstr_toul(&minor_tokens.data[0], &temp_epoch, 10) );
    if(epoch != NULL) *epoch = temp_epoch;

    // second field is uid_up
    unsigned int temp_uid_up;
    PROP(&e, dstr_tou(&fields.data[1], &temp_uid_up, 10) );
    if(uid_up != NULL) *uid_up = temp_uid_up;

    // third field is length
    size_t temp_len;
    PROP(&e, dstr_tosize(&fields.data[2], &temp_len, 10) );
    if(len != NULL) *len = temp_len;

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

// info and flags are allowed to be NULL, but not host
derr_t maildir_name_write(dstr_t *out, unsigned long epoch,
        unsigned int uid_up, size_t len, const dstr_t *host,
        const dstr_t *info){
    derr_t e = E_OK;

    PROP(&e, FMT(out, "%x.%x,%x,%x.",
                FU(epoch), FU(deliv_id_ver), FU(uid_up), FU(len)) );

    PROP(&e, maildir_name_mod_hostname(host, out) );

    if(info != NULL && info->len > 0){
        PROP(&e, FMT(out, ":%x", FD(info)) );
    }

    return e;
}
