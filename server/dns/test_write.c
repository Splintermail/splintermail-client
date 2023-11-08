#include "server/dns/libdns.h"

#define ASSERT(code) if(!(code)) ORIG(&e, E_VALUE, "assertion failed: " #code)

static derr_t test_predicted_lengths(void){
    derr_t e = E_OK;

    char buf[1024];
    size_t cap = sizeof(buf);

    lstr_t labels[] = {LSTR("hostmaster"), LSTR("splintermail"), LSTR("com")};
    size_t nlabels = sizeof(labels) / sizeof(*labels);
    ASSERT(namelen(labels, nlabels) == put_name(labels, nlabels, buf, 0));
    lstr_t secret = LSTR("top-secret-text");

    ASSERT(write_soa(NULL, 0, buf, 0, 0) == write_soa(NULL, 0, buf, cap, 0));
    ASSERT(write_a(NULL, 0, buf, 0, 0) == write_a(NULL, 0, buf, cap, 0));
    ASSERT(write_ns1(NULL, 0, buf, 0, 0) == write_ns1(NULL, 0, buf, cap, 0));
    ASSERT(write_ns2(NULL, 0, buf, 0, 0) == write_ns2(NULL, 0, buf, cap, 0));
    ASSERT(write_aaaa(NULL, 0, buf, 0, 0) == write_aaaa(NULL, 0, buf, cap, 0));
    ASSERT(write_caa(NULL, 0, buf, 0, 0) == write_caa(NULL, 0, buf, cap, 0));

    ASSERT(
        write_secret(&secret, 0, buf, 0, 0)
        == write_secret(&secret, 0, buf, cap, 0)
    );

    ASSERT(write_edns(buf, 0, 0) == BARE_EDNS_SIZE);
    ASSERT(write_edns(buf, cap, 0) == BARE_EDNS_SIZE);

    char temp[] =
        "\x01" "a" "\x02" "bb" "\x03" "ccc" "\x00"
        "\x01" "z" "\xC0\x00"
    ;
    size_t templen = sizeof(temp)-1;

    dns_qstn_t qstn = {
        .ptr = temp,
        // qname = a.bb.ccc
        .off = 0,
        .len = templen,
        .qdcount = 1,
        .qtype = 0,
        .qclass = 0,
    };
    ASSERT(write_qstn(qstn, buf, 0, 0) == 14);
    ASSERT(write_qstn(qstn, buf, cap, 0) == 14);

    // qname = z.a.bb.ccc
    qstn.off = 10;
    ASSERT(write_qstn(qstn, buf, 0, 0) == 16);
    ASSERT(write_qstn(qstn, buf, cap, 0) == 16);

    return e;
}

int main(void){
    int retval = 0;

    logger_add_fileptr(LOG_LVL_INFO, stdout);

    #define RUN(code) do { \
        derr_t e = code; \
        if(is_error(e)){ \
            DUMP(e); \
            DROP_VAR(&e); \
            retval = 1; \
        } \
    } while(0)

    RUN(test_predicted_lengths());

    printf("%s\n", retval ? "FAIL" : "PASS");

    return retval;
}
