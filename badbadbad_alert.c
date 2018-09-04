#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "badbadbad_alert.h"
#include "logger.h"

#define SOCKFILE "/run/badbadbad/badbadbad.sock"
#define LOGFILE "/var/log/badbadbad/badbadbad.log"

// get 5 random bytes and hexify them
static void get_tag(dstr_t* tag){
    // open /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if(fd < 0){
        LOG_ERROR("unable to open /dev/urandom: %x\n", FE(&errno));
        return;
    }

    // read 5 bytes
    DSTR_VAR(bin, 5);
    ssize_t amnt_read = read(fd, bin.data, bin.size);
    if(amnt_read < 5){
        LOG_ERROR("unable to read from /dev/urandom: %x\n", FE(&errno));
        return;
    }
    bin.len = 5;

    // hexify
    DSTR_VAR(hex, 10);
    derr_t error = bin2hex(&bin, &hex);
    CATCH(E_ANY){
        LOG_ERROR("failed to hexify tag\n");
        return;
    }

    // upper case hex pleases me
    dstr_upper(&hex);

    // make sure we can copy into tag
    if(tag->size < hex.size){
        LOG_ERROR("tag too short to copy to\n");
        return;
    }

    // write to the tag (can't fail, we already checked the length)
    dstr_copy(&hex, tag);
}

static void get_timestamp(dstr_t* ts){
    time_t epoch = time(NULL);
    // get a date string
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        LOG_ERROR("error getting timestamp: %x\n", FE(&errno));
        return;
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t len;
    len = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(len == 0){
        LOG_ERROR("error formatting time string: %x\n", FE(&errno));
        return;
    }

    // make sure we can copy into ts
    if(ts->size < len + 1){
        LOG_ERROR("ts too short to copy to\n");
        return;
    }

    // write to ts (can't fail, we already check the length)
    ts->len = 0;
    FMT(ts, "%x", FS(d));
}

static void badbadbad_write_log(dstr_t* blob){
    derr_t error;
    // open file
    int fd = open(LOGFILE, O_WRONLY | O_APPEND);
    if(fd < 0){
        LOG_ERROR(LOGFILE ": %x\n", FE(&errno));
        ORIG_GO(E_OPEN, "unable to open file", fail);
    }
    // append to file
    PROP_GO( dstr_write(fd, blob), cleanup);
cleanup:
    close(fd);
fail:
    CATCH(E_ANY){
        LOG_ERROR("failed in badbadbad_write_log(), continuing\n");
    }
}

static void badbadbad_log(dstr_t* tag, dstr_t* summary, dstr_t* body, dstr_t* buff){
    derr_t error;
    // get a timestamp for this error
    DSTR_VAR(ts, 128);
    FMT(&ts, "(TIMESTAMP ERROR)");
    get_timestamp(&ts);
    // build buffer
    buff->len = 0;
    error =  FMT(buff,
                 "---------------------------------------------------\n"
                 "%x - %x\n"
                 "---------------------------------------------------\n"
                 "%x\n"
                 "%x",
                 FD(&ts), FD(tag), FD(summary), FD(body));
    CATCH(E_ANY){
        LOG_ERROR("failed in badbadbad_log(), continuing\n");
        return;
    }

    // write buffer
    badbadbad_write_log(buff);
}

static void send_to_badbadbad_server(dstr_t* tag, dstr_t* summary, dstr_t* body, dstr_t* buff){
    derr_t error;
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
        PROP_GO( FMT(buff,
                     "###################################################\n"
                     "alert %x dropped by client due to connect error: %x\n"
                     "###################################################\n",
                     FD(tag), FE(&errno)), fail);
        // write buffer
        badbadbad_write_log(buff);
        // can't continue without a connection
        goto fail;
    }

    // prep the alert buffer
    buff->len = 0;
    error = FMT(buff, "%x\n%x\n%x", FD(tag), FD(summary), FD(body));
    CATCH(E_ANY){
        buff->len = 0;
        PROP_GO( FMT(buff,
                     "###################################################\n"
                     "alert %x dropped by client due to unexpected error\n"
                     "###################################################\n",
                     FD(tag)), fail_sock);
        // write buffer
        badbadbad_write_log(buff);
        goto fail_sock;
    }

    // send the alert to the badbadbad server
    error = dstr_write(sock, buff);
    CATCH(E_ANY){
        buff->len = 0;
        PROP_GO( FMT(buff,
                     "###################################################\n"
                     "alert %x dropped by client due to write error: %x\n"
                     "###################################################\n",
                     FD(tag), FE(&errno)), fail_sock);
        // write buffer
        badbadbad_write_log(buff);
        goto fail_sock;
    }

    // indicate we are done sending
    ret = shutdown(sock, SHUT_WR);
    if(ret){
        LOG_ERROR("data sent, but failed to shutdown: %x\n", FE(&errno));
        goto fail_sock;
    }

    // wait for server to close
    char* ignore_buff[10];
    ssize_t amnt_read = read(sock, ignore_buff, sizeof(ignore_buff));
    if(amnt_read < 0){
        LOG_ERROR("data sent, but got error on read: %x\n", FE(&errno));
        goto fail_sock;
    }

    close(sock);
    return;

fail_sock:
    close(sock);
fail:
    // this is just to make the log files clearer
    CATCH(E_ANY){}
    LOG_ERROR("failure in send_to_badbadbad_server(), continuing\n");
}

void badbadbad_alert(dstr_t* summary, dstr_t* body){
    derr_t error;

    // allow the body to be null or to contain a null pointer
    DSTR_STATIC(emptybody, "");
    if(body == NULL || body->data == NULL){
        body = &emptybody;
    }

    // first allocate a multipurpose buffer
    dstr_t buff;
    error = dstr_new(&buff, 4096);
    CATCH(E_NOMEM){
        LOG_ERROR("memory failure, aborting.\n");
        return;
    }

    // get a tag for this error (length must be 10)
    DSTR_PRESET(tag, "(TAGERROR)");
    get_tag(&tag);

    // write to this program's log file, to enable cross-checking
    LOG_ERROR("logging badbadbad error, tag %x\n", FD(&tag));

    // write to the badbadbad log file
    badbadbad_log(&tag, summary, body, &buff);

    // then communicate to the badbadbad server
    send_to_badbadbad_server(&tag, summary, body, &buff);
}
