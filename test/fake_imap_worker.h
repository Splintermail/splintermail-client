#ifndef IMAP_WORKER_H
#define IMAP_WORKER_H

#include <uv.h>

#include <imap_engine.h>
#include <libdstr/common.h>

derr_t fake_imap_worker_new(imape_t *imape, imape_worker_t **worker);

#endif // IMAP_WORKER_H
