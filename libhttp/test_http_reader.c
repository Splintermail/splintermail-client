#include <stdlib.h>

#include "libdstr/libdstr.h"
#include "libparsing/libparsing.h"
#include "liburl/liburl.h"
#include "libhttp/libhttp.h"

#include "test/test_utils.h"

#define ASSERT(code) if(!(code)) ORIG(&e, E_VALUE, "assertion failed: " #code)

static derr_t test_http_reader(void){
    derr_t e = E_OK;

    // some output of a `GET /` on splintermail.com
    DSTR_STATIC(resp,
        "HTTP/1.1 200 OK\r\n"
        "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
        "Server: Yo mamma\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "X-Frame-Options: DENY\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-XSS-Protection: 1; mode=block\r\n"
        "Content-Security-Policy: default-src 'self'\r\n"
        "Strict-Transport-Security: preload\r\n"
        "Expires: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Vary: Accept-Encoding\r\n"
        "Connection: close\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "\r\n"
        "body bytes\r\n"
    );

    size_t eoh_len = resp.len - strlen("body bytes\r\n");

    // read every sublength between 0 and resp.len
    size_t i;
    for(size_t l = 0; l <= resp.len; l++){
        dstr_t sub = dstr_sub2(resp, 0, l);

        // LOG_INFO("sub = %x\n", FD_DBG(&sub));

        http_reader_t r;
        http_reader_init(&r, &sub);

        i = 0;
        while(true){
            http_pair_t hdr;
            int status;
            PROP(&e, http_read(&r, &hdr, &status) );
            switch(status){
                case -2: // incomplete read
                    // expected when l < resp.len
                    ASSERT(l < resp.len);
                    goto next_sublength;

                case -1: // found a header
                    ASSERT(r.code == 200);
                    ASSERT(dstr_cmp2(r.reason, DSTR_LIT("OK")) == 0);
                    // LOG_INFO("READ %x: %x.\n", FD(&hdr.key), FD(&hdr.value));
                    i++;
                    break;

                default: // end of headers
                    // expected any time l >= eoh_len
                    EXPECT_U_GE(&e, "l", l, eoh_len);
                    EXPECT_U(&e, "status", (size_t)status, eoh_len);
                    EXPECT_U(&e, "header count", i, 15);
                    goto next_sublength;
            }
        }

    next_sublength:
        continue;
    }

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_http_reader(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
