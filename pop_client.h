#ifndef POP_CLIENT_H
#define POP_CLIENT_H

#include "libdstr/libdstr.h"
#include "networking.h"
#include "fixed_lengths.h"

typedef struct {
    connection_t conn;
    char response_buffer[POP_CLIENT_CHUNK_SIZE];
    dstr_t response;
    LIST(dstr_t) uids;
    LIST(size_t) idxs;
    dstr_t uids_block;
} pop_client_t;


derr_t pop_client_new(pop_client_t* pc);

void pop_client_free(pop_client_t* pc);

derr_t pop_client_connect(pop_client_t* pc, ssl_context_t* ctx,
                          const char* addr, unsigned int port,
                          bool* status_ok, dstr_t* message);

derr_t pop_client_username(pop_client_t* pc, const dstr_t* username,
                           bool* status_ok, dstr_t* message);

derr_t pop_client_password(pop_client_t* pc, const dstr_t* password,
                           bool* status_ok, dstr_t* message);

derr_t pop_client_uidl(pop_client_t* pc, bool* status_ok, dstr_t* message);
/* throws: E_CONN (from send_dstr)
           E_INTERNAL (from pop_client_read)
           E_RESPONSE (invalid server response)
           E_NOMEM (copying to *message, or internally to uids or uids_block)
           E_FIXEDSIZE (copying to *message) */

derr_t pop_client_retrieve(pop_client_t* pc, unsigned int index,
                           bool* status_ok, dstr_t* message);

derr_t pop_client_delete(pop_client_t* pc, unsigned int index,
                         bool* status_ok, dstr_t* message);
/* thows: E_CONN     (from pop_client_send_dstr, pop_client_read)
          E_INTERNAL (from pop_client_read)
          E_RESPONSE (invalid server response)
          E_NOMEM (copying to *message)
          E_FIXEDSIZE (copying to *message) */

derr_t pop_client_reset(pop_client_t* pc, bool* status_ok, dstr_t* message);

derr_t pop_client_quit(pop_client_t* pc, bool* status_ok, dstr_t* message);

derr_t pop_client_send_dstr(pop_client_t* pc, const dstr_t* buffer);
/* thows: E_CONN (straight outta connection_write) */

derr_t pop_client_get_status_and_message(pop_client_t* pc,
                                         bool* status_ok, dstr_t* message);
/* thows: E_CONN     (from pop_client_read)
          E_INTERNAL (from pop_client_read)
          E_RESPONSE (invalid server response)
          E_NOMEM (copying to *message)
          E_FIXEDSIZE (copying to *message) */

// will append another pop3-decoded chunk to body each time it is called
/* first_pass is necessary because for empty responses (".\r\n") the normal
   end pattern ("\r\n.\r\n") does not match. */
derr_t pop_client_get_body(pop_client_t* pc, dstr_t* body,
                           bool first_pass, bool* found_end);
/* throws: E_INTERNAL (from pop_client_read)
           E_CONN     (from pop_client_read)
           E_FIXEDSIZE (dstr_recode into *body arg)
           E_NOMEM     (dstr_recode into *body arg) */

/* The purpose of these two functions is to offer an easy interface for
   encoding and decoding operations on blocks of data from a stream, even if
   block boundaries land in the middle of a search pattern.  The interface is
   very simple; you hand an "in" buffer and an "out" buffer to a function (the
   "out" buffer should either be on the heap or safely larger than the "in"
   buffer) and the function will "consume" all the bytes in the "in" buffer
   that can be fully encoded/decoded, and leave the remainder in the "in"
   buffer.  The pattern matching is limited in that it does not handle
   wildcards or anti-matches, but it is good enough for the tranlations that
   DITM needs. */

derr_t pop3_decode(dstr_t* in, dstr_t* out, bool* found_end);
derr_t pop3_encode(dstr_t* in, dstr_t* out, bool force_end);

#endif //POP_CLIENT_H
