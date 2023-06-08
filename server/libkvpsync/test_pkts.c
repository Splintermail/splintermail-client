#include "server/libkvpsync/libkvpsync.h"

#include "test/test_utils.h"

#include <string.h>

static derr_t test_ack(void){
    derr_t e = E_OK;

    DSTR_VAR(buf, 4096);

    // big-endian representation of numbers is ascii "ABCD" and "EFGH"
    kvp_ack_t ack = { .sync_id = 1094861636, .update_id = 1162233672 };
    PROP(&e, kvpsync_ack_write(&ack, &buf) );

    EXPECT_D3(&e, "buf", buf, DSTR_LIT("ABCDEFGH"));

    kvp_ack_t out;
    bool ok = kvpsync_ack_read(buf, &out);

    EXPECT_B(&e, "ok", ok, true);
    EXPECT_U(&e, "out.sync_id", out.sync_id, ack.sync_id);
    EXPECT_U(&e, "out.update_id", out.update_id, ack.update_id);

    return e;
}

static derr_t test_update(void){
    derr_t e = E_OK;

    kvp_update_t update[] = {
        // EMPTY
        {
            .ok_expiry = 99,
            .sync_id = 1111111,
            .update_id = 222222222,
            .type = KVP_UPDATE_EMPTY,
        },
        // FLUSH
        {
            .ok_expiry = 99,
            .sync_id = 1111111,
            .update_id = 222222222,
            .type = KVP_UPDATE_FLUSH,
        },
        // START
        {
            .ok_expiry = 99,
            .sync_id = 1111111,
            .update_id = 1,
            .type = KVP_UPDATE_START,
            .resync_id = 3333,
        },
        // INSERT
        {
            .ok_expiry = 99,
            .sync_id = 1111111,
            .update_id = 222222222,
            .type = KVP_UPDATE_INSERT,
            .klen = 8,
            .key = "abcdefgh",
            .vlen = 8,
            .val = "ABCDEFGH",
        },
        // INSERT
        {
            .ok_expiry = 99,
            .sync_id = 1111111,
            .update_id = 222222222,
            .type = KVP_UPDATE_DELETE,
            .klen = 8,
            .key = "abcdefgh",
            .delete_id = 44444444,
        }
    };

    for(size_t i = 0; i < sizeof(update)/sizeof(*update); i++){
        DSTR_VAR(buf, 4096);
        kvp_update_t out;
        PROP(&e, kvpsync_update_write(&update[i], &buf) );
        bool ok = kvpsync_update_read(buf, &out);
        EXPECT_B(&e, "ok", ok, true);
        int cmp = memcmp(&update[i], &out, sizeof(out));
        EXPECT_I(&e, "memcmp", cmp, 0);
    }

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_ack(), test_fail);
    PROP_GO(&e, test_update(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
