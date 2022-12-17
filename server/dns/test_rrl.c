#include "server/dns/libdns.h"

#include "test/test_utils.h"

static derr_t do_test(rrl_t *rrl){
    derr_t e = E_OK;

    struct sockaddr_storage a[3];
    size_t naddrs = sizeof(a)/sizeof(*a);
    PROP(&e, read_addr(&a[0], "1.2.3.4", 1) );
    PROP(&e, read_addr(&a[1], "1.2.3.5", 1) );
    PROP(&e, read_addr(&a[2], "1111:2222:3333:4444::", 1) );

    // block window is 1 second long, resets after 32 seconds
    xtime_t end;
    for(xtime_t now = 1*SECOND; now < 34*SECOND; now += 1*SECOND){
        // every gets 8 responses for free
        for(size_t i = 0; i < naddrs; i++){
            const struct sockaddr *sa = ss2sa(&a[i]);
            for(size_t j = 0; j < 8; j++){
                EXPECT_B(&e, "rrl_check", rrl_check(rrl, sa, now), true);
            }
            // after that you are blocked
            EXPECT_B(&e, "rrl_check", rrl_check(rrl, sa, now), false);
            EXPECT_B(&e, "rrl_check", rrl_check(rrl, sa, now), false);
            end = now;
        }
    }

    // only the first 64 bits of ipv6 addrs are considered
    struct sockaddr_storage a2;
    PROP(&e, read_addr(&a2, "1111:2222:3333:4444:abcd:efab:cdef:abcd", 1) );
    const struct sockaddr *sa = ss2sa(&a2);
    EXPECT_B(&e, "rrl_check", rrl_check(rrl, sa, end), false);


    return e;
}

static derr_t test_rrl(void){
    derr_t e = E_OK;

    rrl_t rrl = {0};
    // safe to free zeroized
    rrl_free(&rrl);
    PROP(&e, rrl_init(&rrl, 997) );

    PROP_GO(&e, do_test(&rrl), cu);

cu:
    rrl_free(&rrl);
    // safe to double-free
    rrl_free(&rrl);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_rrl(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
