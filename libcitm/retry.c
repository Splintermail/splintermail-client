#include "libcitm/libcitm.h"

void _retry_start(
    retry_t *r, uint64_t limit, uint64_t *backoffs, uint64_t nbackoffs
){
    r->limit = limit;
    if(r->attempts < nbackoffs){
        r->backoff = backoffs[r->attempts];
    }else if(nbackoffs > 0){
        r->backoff = backoffs[nbackoffs - 1];
    }else{
        r->backoff = 0;
    }
    r->attempts += 1;
}

bool retry_check(retry_t *r, derr_t *err){
    if(!is_error(*err)){
        // success case
        retry_free(r);
        return false;
    }

    if(r->attempts < r->limit){
        // retry case
        if(r->attempts > 1){
            DSTR_VAR(buf, 8192);
            // secondary failures: build errors in reverse chronological order
            FMT_QUIET(&buf,
                "The error before that was %x:\n%x",
                FD(error_to_dstr(err->type)),
                FD(err->msg)
            );
            /* use FMT instead of dstr_append for it's "write all that we can"
               behavior when it runs out of space; we're relying on it to
               truncate the message nicely */
            FMT_QUIET(&buf, "%x", FD(r->errs));
            if(buf.len == buf.size){
                buf.len -= 3;
                dstr_append_char(&buf, '.');
                dstr_append_char(&buf, '.');
                dstr_append_char(&buf, '.');
            }
            r->errs.len = 0;
            dstr_append_quiet(&r->errs, &buf);
        }
        DROP_VAR(err);
        return true;
    }

    // failure case, just extend the provided error
    TRACE(err, "%x", FD(r->errs));
    retry_free(r);
    return false;
}

void retry_free(retry_t *r){
    dstr_free(&r->errs);
    *r = (retry_t){0};
}
