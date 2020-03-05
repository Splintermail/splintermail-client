#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "badbadbad_alert.h"
#include "libdstr/logger.h"

#define SOCKFILE "/run/badbadbad/badbadbad.sock"
#define LOGFILE "/var/log/badbadbad/badbadbad.log"

// get 5 random bytes and hexify them
static derr_t get_tag(dstr_t* tag){
    derr_t e = E_OK;
    derr_t e2;

    // open /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0){
        // just return a non-empty trace
        TRACE(&e, "unable to open /dev/urandom: %x\n", FE(&errno));
        ORIG(&e, E_OS, "failed in get_tag");
    }

    // read 5 bytes
    DSTR_VAR(bin, 5);
    ssize_t amnt_read = read(fd, bin.data, bin.size);
    if(amnt_read < 5){
        // just return a non-empty trace
        TRACE(&e, "unable to read from /dev/urandom: %x\n", FE(&errno));
        ORIG(&e, E_OS, "failed in get_tag");
    }
    bin.len = 5;

    // hexify
    DSTR_VAR(hex, 10);
    e2 = bin2hex(&bin, &hex);
    CATCH(e2, E_ANY){
        TRACE(&e2, "failed to hexify tag\n");
        RETHROW(&e, &e2, E_INTERNAL);
    }

    // upper case hex pleases me
    dstr_upper(&hex);

    // make sure we can copy into tag
    if(tag->size < hex.size){
        // just return a non-empty trace
        TRACE(&e, "tag too short to copy to\n");
        ORIG(&e, E_INTERNAL, "failed in get_tag");
    }

    // write to the tag (can't fail, we already checked the length)
    DROP_CMD( dstr_copy(&hex, tag) );
    return e;
}

static derr_t get_timestamp(dstr_t* ts){
    derr_t e = E_OK;
    time_t epoch = time(NULL);
    // get a date string
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        TRACE(&e, "error getting timestamp: %x\n", FE(&errno));
        ORIG(&e, E_OS, "failed in get_timestamp");
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t len;
    len = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(len == 0){
        TRACE(&e, "error formatting time string: %x\n", FE(&errno));
        ORIG(&e, E_INTERNAL, "failed in get_timestamp");
    }

    // make sure we can copy into ts
    if(ts->size < len + 1){
        // just return a non-empty trace
        ORIG(&e, E_INTERNAL, "ts too short to copy to");
    }

    // write to ts (can't fail, we already check the length)
    ts->len = 0;
    DROP_CMD( FMT(ts, "%x", FS(d)) );
    return e;
}

static void badbadbad_write_log(dstr_t* blob){
    derr_t e = E_OK;
    // open file
    int fd = open(LOGFILE, O_WRONLY | O_APPEND);
    if(fd < 0){
        TRACE(&e, LOGFILE ": %x\n", FE(&errno));
        ORIG_GO(&e, E_OPEN, "unable to open file", fail);
    }
    // append to file
    PROP_GO(&e, dstr_write(fd, blob), cleanup);
cleanup:
    close(fd);
fail:
    CATCH(e, E_ANY){
        LOG_ERROR("failed in badbadbad_write_log():\n");
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("continuing\n");
    }
}

static void badbadbad_log(dstr_t* tag, dstr_t* summary, dstr_t* body,
        dstr_t* buff){
    derr_t e = E_OK;
    // get a timestamp for this error
    DSTR_VAR(ts, 128);
    FMT(&ts, "(TIMESTAMP ERROR)");

    IF_PROP(&e, get_timestamp(&ts)){
        // just accumulate messages in the trace
    }

    // build buffer
    buff->len = 0;
    IF_PROP(&e, FMT(buff,
         "---------------------------------------------------\n"
         "%x - %x\n"
         "---------------------------------------------------\n"
         "%x\n"
         "%x",
         FD(&ts), FD(tag), FD(summary), FD(body))
    ){}

    CATCH(e, E_ANY){
        LOG_ERROR("failed in badbadbad_log():\n");
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("continuing\n");
        return;
    }

    // write buffer
    badbadbad_write_log(buff);
}

static void send_to_badbadbad_server(dstr_t* tag, dstr_t* summary, dstr_t* body, dstr_t* buff){
    derr_t e = E_OK;
    derr_t e2;
    // open a socket
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    // build the address structure to connect to
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    // safely copy SOCKFILE into sun_path
    strncpy(sun.sun_path, SOCKFILE, sizeof(sun.sun_path));
    // strncpy doesn't guarantee a null terminated copy
    sun.sun_path[sizeof(sun.sun_path)-1] = 0;

    // connect
    int ret = connect(sock, (struct sockaddr*)&sun, sizeof(sun));
    if(ret){
        buff->len = 0;
        IF_PROP(&e, FMT(buff,
            "###################################################\n"
            "alert %x dropped by client due to connect error: %x\n"
            "###################################################\n",
            FD(tag), FE(&errno))){}
        // write buffer
        badbadbad_write_log(buff);
        // can't continue without a connection
        ORIG_GO(&e, E_CONN, "connect error", fail);
    }

    // prep the alert buffer
    buff->len = 0;
    e2 = FMT(buff, "%x\n%x\n%x", FD(tag), FD(summary), FD(body));
    CATCH(e2, E_ANY){
        buff->len = 0;
        IF_PROP(&e, FMT(buff,
            "###################################################\n"
            "alert %x dropped by client due to unexpected error\n"
            "###################################################\n",
            FD(tag))){}
        // write buffer
        badbadbad_write_log(buff);
        RETHROW_GO(&e, &e2, E_OS, fail_sock);
    }

    // send the alert to the badbadbad server
    e2 = dstr_write(sock, buff);
    CATCH(e2, E_ANY){
        buff->len = 0;
        IF_PROP(&e, FMT(buff,
            "###################################################\n"
            "alert %x dropped by client due to write error: %x\n"
            "###################################################\n",
            FD(tag), FE(&errno))){}
        // write buffer
        badbadbad_write_log(buff);
        RETHROW_GO(&e, &e2, E_OS, fail_sock);
    }

    // indicate we are done sending
    ret = shutdown(sock, SHUT_WR);
    if(ret){
        TRACE(&e, "data sent, but failed to shutdown: %x\n", FE(&errno));
        ORIG_GO(&e, E_OS, "shutdown failed", fail_sock);
    }

    // wait for server to close
    char* ignore_buff[10];
    ssize_t amnt_read = read(sock, ignore_buff, sizeof(ignore_buff));
    if(amnt_read < 0){
        TRACE(&e, "data sent, but got error on read: %x\n", FE(&errno));
        ORIG_GO(&e, E_OS, "post-shutdown read failed", fail_sock);
    }

    close(sock);
    return;

fail_sock:
    close(sock);
fail:
    // this is just to make the log files clearer
    CATCH(e, E_ANY){
        TRACE(&e, "failure in send_to_badbadbad_server(), continuing\n");
    }
    DUMP(e);
    DROP_VAR(&e);
}

void badbadbad_alert(dstr_t* summary, dstr_t* body){
    derr_t e = E_OK;

    // allow the body to be null or to contain a null pointer
    DSTR_STATIC(emptybody, "");
    if(body == NULL || body->data == NULL){
        body = &emptybody;
    }

    // first allocate a multipurpose buffer
    dstr_t buff;
    PROP_GO(&e, dstr_new(&buff, 4096), fail);

    // get a tag for this error (length must be 10)
    DSTR_PRESET(tag, "(TAGERROR)");
    IF_PROP(&e, get_tag(&tag)){
        DUMP(e);
        DROP_VAR(&e);
    }

    // write to this program's log file, to enable cross-checking
    LOG_ERROR("logging badbadbad error, tag %x\n", FD(&tag));

    // write to the badbadbad log file
    badbadbad_log(&tag, summary, body, &buff);

    // then communicate to the badbadbad server
    send_to_badbadbad_server(&tag, summary, body, &buff);

    return;

fail:
    TRACE(&e, "aborting\n");
    DUMP(e);
    DROP_VAR(&e);
    return;
}
