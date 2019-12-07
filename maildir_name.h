#ifndef MAILDIR_NAME_H
#define MAILDIR_NAME_H

#include "imap_maildir.h"

// *meta is allowed to be NULL, which makes this a validation function
derr_t maildir_name_parse_imap_flags(const dstr_t *flags, msg_meta_t *meta);

/* only *name and *valid are required to be non-NULL, in which case this
   becomes a validation function */
derr_t maildir_name_parse(const dstr_t *name, unsigned long *epoch,
        size_t *len, unsigned int *uid, msg_meta_t *meta, dstr_t *host,
        dstr_t *info);

/* modded_hostname replaces '/' with "057"
                        and ':' with "072"
   The standard says to preface the codes with a backslash like '\057' but I
   think that may break on windows, so I just dropped that character.
*/
derr_t maildir_name_mod_hostname(const dstr_t* host, dstr_t *out);

// info and meta are allowed to be NULL, but not host
derr_t maildir_name_write(dstr_t *out, unsigned long epoch, size_t len,
        unsigned int uid, msg_meta_t *meta, const dstr_t *host,
        const dstr_t *info);

#endif // MAILDIR_NAME_H
