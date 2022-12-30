#define KVPSYNC_MAX_LEN 255

typedef enum {
    KVP_UPDATE_EMPTY = 0,
    KVP_UPDATE_FLUSH = 1,
    KVP_UPDATE_START = 2,
    KVP_UPDATE_INSERT = 3,
    KVP_UPDATE_DELETE = 4,
} kvp_update_type_e;

const char *kvp_update_type_name(kvp_update_type_e type);

typedef struct {
    xtime_t ok_expiry;
    uint32_t sync_id;
    uint32_t update_id;
    kvp_update_type_e type : 8;
    uint32_t resync_id; // only present for start packets
    uint8_t klen; // only present for insert or delete packets
    char key [KVPSYNC_MAX_LEN]; // only present for insert or delete packets
    uint32_t delete_id; // only present for delete packets
    uint8_t vlen; // only present for insert packets
    char val [KVPSYNC_MAX_LEN]; // only present for insert packets
} kvp_update_t;

bool kvpsync_update_read(const dstr_t rbuf, kvp_update_t *out);
derr_t kvpsync_update_write(const kvp_update_t *update, dstr_t *out);

typedef struct {
    uint32_t sync_id;
    uint32_t update_id;
} kvp_ack_t;

bool kvpsync_ack_read(const dstr_t rbuf, kvp_ack_t *out);
derr_t kvpsync_ack_write(const kvp_ack_t *ack, dstr_t *out);
