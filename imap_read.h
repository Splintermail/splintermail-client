#ifndef IMAP_READ_H
#define IMAP_READ_H

#include "common.h"
#include "imap_read_types.h"

// forward declaration of imap_reader_t
struct imap_reader_t;

derr_t imap_reader_init(imap_reader_t *reader,
                        imap_hooks_dn_t hooks_dn,
                        imap_hooks_up_t hooks_up,
                        void *hook_data);

void imap_reader_free(imap_reader_t *reader);

derr_t imap_read(imap_reader_t *reader, const dstr_t *input);

#endif // IMAP_READ_H
