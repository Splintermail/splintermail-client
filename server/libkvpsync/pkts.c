#include "server/libkvpsync/libkvpsync.h"

#include <string.h>

static uint64_t read_uint64(const dstr_t rbuf, size_t *pos, bool *ok){
    if(!*ok) return 0;
    uint8_t *udata = (uint8_t*)rbuf.data;
    if(*pos + 8 > rbuf.len){
        *ok = false;
        return 0;
    }
    uint64_t u = 0;
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    u = (u << 8) | (uint64_t)(udata[(*pos)++]);
    return u;
}

static uint32_t read_uint32(const dstr_t rbuf, size_t *pos, bool *ok){
    if(!*ok) return 0;
    uint8_t *udata = (uint8_t*)rbuf.data;
    if(*pos + 4 > rbuf.len){
        *ok = false;
        return 0;
    }
    uint32_t u = 0;
    u = (u << 8) | (uint32_t)(udata[(*pos)++]);
    u = (u << 8) | (uint32_t)(udata[(*pos)++]);
    u = (u << 8) | (uint32_t)(udata[(*pos)++]);
    u = (u << 8) | (uint32_t)(udata[(*pos)++]);
    return u;
}

static uint8_t read_uint8(const dstr_t rbuf, size_t *pos, bool *ok){
    if(!*ok) return 0;
    uint8_t *udata = (uint8_t*)rbuf.data;
    if(*pos + 1 > rbuf.len){
        *ok = false;
        return 0;
    }
    uint8_t u = 0;
    u = (u << 8) | udata[(*pos)++];
    return u;
}

static derr_t write_uint64(uint64_t u, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, dstr_grow(out, out->len + 8) );

    uint8_t *udata = (uint8_t*)out->data;
    udata[out->len++] = (uint8_t)((u >> 56)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 48)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 40)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 32)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 24)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 16)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 8)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 0)&0xFF);

    return e;
}

static derr_t write_uint32(uint32_t u, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, dstr_grow(out, out->len + 4) );

    uint8_t *udata = (uint8_t*)out->data;
    udata[out->len++] = (uint8_t)((u >> 24)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 16)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 8)&0xFF);
    udata[out->len++] = (uint8_t)((u >> 0)&0xFF);

    return e;
}

static derr_t write_uint8(uint8_t u, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, dstr_grow(out, out->len + 1) );

    uint8_t *udata = (uint8_t*)out->data;
    udata[out->len++] = (uint8_t)((u >> 0)&0xFF);

    return e;
}

// returns bool ok
bool kvpsync_update_read(const dstr_t rbuf, kvp_update_t *out){
    bool ok = true;
    *out = (kvp_update_t){0};

    size_t pos = 0;
    out->ok_expiry = read_uint64(rbuf, &pos, &ok);
    out->sync_id = read_uint32(rbuf, &pos, &ok);
    out->update_id = read_uint32(rbuf, &pos, &ok);

    // conditional parsing based on flags
    out->type = read_uint8(rbuf, &pos, &ok);
    if(!ok) return false;
    switch(out->type){
        // empty contents
        case KVP_UPDATE_EMPTY:
        case KVP_UPDATE_FLUSH:
            return true;
        case KVP_UPDATE_START:
            if(out->update_id != 1) return false;
            // read resync_id
            out->resync_id = read_uint32(rbuf, &pos, &ok);
            return ok;
        case KVP_UPDATE_INSERT:
        case KVP_UPDATE_DELETE:
            // continued after switch statement
            break;
        default:
            return false;
    }

    // read key
    out->klen = read_uint8(rbuf, &pos, &ok);
    if(!ok) return false;
    if(pos + out->klen > rbuf.len) return false;
    memcpy(out->key, &rbuf.data[pos], out->klen);
    pos += out->klen;

    if(out->type == KVP_UPDATE_DELETE){
        // read delete_id
        out->delete_id = read_uint32(rbuf, &pos, &ok);
        return ok;
    }

    // read value
    out->vlen = read_uint8(rbuf, &pos, &ok);
    if(!ok) return false;
    if(pos + out->vlen > rbuf.len) return false;
    memcpy(out->val, &rbuf.data[pos], out->vlen);
    pos += out->vlen;

    return true;
}

derr_t kvpsync_update_write(const kvp_update_t *update, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, write_uint64(update->ok_expiry, out) );
    PROP(&e, write_uint32(update->sync_id, out) );
    PROP(&e, write_uint32(update->update_id, out) );
    PROP(&e, write_uint8(update->type, out) );
    switch(update->type){
        // empty contents
        case KVP_UPDATE_EMPTY:
        case KVP_UPDATE_FLUSH:
            return e;
        case KVP_UPDATE_START:
            if(update->update_id != 1){
                ORIG(&e, E_INTERNAL, "invalid update_id in START packet");
            }
            PROP(&e, write_uint32(update->resync_id, out) );
            return e;
        case KVP_UPDATE_INSERT:
        case KVP_UPDATE_DELETE:
            // continued after switch statement
            break;
        default:
            ORIG(&e, E_INTERNAL, "invalid packet type");
    }
    // INSERT and DELETE: write klen and key
    PROP(&e, write_uint8(update->klen, out) );
    PROP(&e, dstr_grow(out, out->len + update->klen) );
    memcpy(&out->data[out->len], update->key, update->klen);
    out->len += update->klen;
    if(update->type == KVP_UPDATE_DELETE){
        // DELETE: write delete_id
        PROP(&e, write_uint32(update->delete_id, out) );
        return e;
    }
    // INSERT: write vlen and val
    PROP(&e, write_uint8(update->vlen, out) );
    PROP(&e, dstr_grow(out, out->len + update->vlen) );
    memcpy(&out->data[out->len], update->val, update->vlen);
    out->len += update->vlen;
    return e;
}

bool kvpsync_ack_read(const dstr_t rbuf, kvp_ack_t *out){
    bool ok = true;
    *out = (kvp_ack_t){0};

    size_t pos = 0;
    out->sync_id = read_uint32(rbuf, &pos, &ok);
    out->update_id = read_uint32(rbuf, &pos, &ok);

    return ok;
}

derr_t kvpsync_ack_write(const kvp_ack_t *ack, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, write_uint32(ack->sync_id, out) );
    PROP(&e, write_uint32(ack->update_id, out) );

    return e;
}
