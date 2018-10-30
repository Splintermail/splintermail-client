#ifndef TLS_ENGINE_H
#define TLS_ENGINE_H

#include <uv.h>

#include "common.h"
#include "ixs.h"


void tlse_read_cb(ixs_t *ixs, bool upwards, dstr_t *in);
void tlse_write_cb(ixs_t *ixs, bool upwards, dstr_t *in);

derr_t tlse_decrypt(ixs_t *ixs, bool upwards, dstr_t *in);
derr_t tlse_encrypt(ixs_t *ixs, bool upwards, dstr_t *in);

#endif // TLS_ENGINE_H
