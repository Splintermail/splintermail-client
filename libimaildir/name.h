/* only *name is required to be non-NULL, in which case this becomes a
   validation function */
derr_t maildir_name_parse(const dstr_t *name, unsigned long *epoch,
        unsigned int *uid_up, size_t *len, dstr_t *host, dstr_t *info);

/* modded_hostname replaces '/' with "057"
                        and ':' with "072"
   The standard says to preface the codes with a backslash like '\057' but I
   think that may break on windows, so I just dropped that character.
*/
derr_t maildir_name_mod_hostname(const dstr_t* host, dstr_t *out);

// info and flags are allowed to be NULL, but not host
derr_t maildir_name_write(dstr_t *out, unsigned long epoch,
        unsigned int uid_up, size_t len, const dstr_t *host,
        const dstr_t *info);
