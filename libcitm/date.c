#include "libcitm/libcitm.h"

// always returns a date, falling back to a static string for Jan 1, 1970
// if epoch is (time_t)-1 this function will call dtime() as well
const char *get_date_field(char *buf, size_t len, time_t epoch){
    const char *fallback = "Thu, 1 Jan 1970 00:00:00 +0000";
    derr_t e = E_OK;

    if(epoch == (time_t)-1){
        // get the current time
        e = dtime(&epoch);
        CATCH(e, E_ANY){
            // log error, but ignore it
            TRACE(&e, "ignoring failure of dtime");
            DUMP(e);
            DROP_VAR(&e);
            return fallback;
        }
    }

    struct tm tnow;
    e = dlocaltime(epoch, &tnow);
    CATCH(e, E_ANY){
        // log error, but ignore it
        TRACE(&e, "ignoring failure of dlocaltime");
        DUMP(e);
        DROP_VAR(&e);
        return fallback;
    }

    size_t dlen = strftime(buf, len, "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(dlen == 0){
        LOG_WARN(
            "error formatting time string: strftime: %x\n", FE(&errno)
        );
        return fallback;
    }

    return buf;
}

// returns 1 Jan 1970 on error
// if epoch is (time_t)-1 this function will call dtime() as well
imap_time_t imap_time_now(time_t epoch){
    derr_t e = E_OK;
    imap_time_t fallback = {
        .year = 1970,
        .month = 1,
        .day = 1,
        .hour = 0,
        .min = 0,
        .sec = 0,
        .z_hour = 0,
        .z_min = 0,
    };

    if(epoch == (time_t)-1){
        // get the current time
        e = dtime(&epoch);
        CATCH(e, E_ANY){
            // log error, but ignore it
            TRACE(&e, "ignoring failure of dtime");
            DUMP(e);
            DROP_VAR(&e);
            return fallback;
        }
    }

    struct tm tnow;
    IF_PROP(&e, dlocaltime(epoch, &tnow) ){
        // if this fails, return zeroized time
        DROP_VAR(&e);
        return fallback;
    }

    // get the timezone, sets extern long timezone to a signed second offset
    long int tz;
    IF_PROP(&e, dtimezone(&tz) ){
        // if this fails, just use zero
        tz = 0;
        DROP_VAR(&e);
    }

    // libc defines timezone as negative of what it should be
    long int tz_hour = -(int)(tz/60/60);
    // do mod-math on the positive-signed value
    long int tz_pos = tz * (tz > 0 ? 1 : -1);
    long int tz_min = (int)((tz_pos % 60*60) / 60);

    return (imap_time_t){
        .year = tnow.tm_year + 1900,
        .month = tnow.tm_mon + 1,
        .day = tnow.tm_mday,
        .min = tnow.tm_min,
        .sec = tnow.tm_sec,
        .z_hour = (int)tz_hour,
        .z_min = (int)tz_min,
    };
}
