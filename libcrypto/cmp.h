/* compare two strings in constant time, where the timing does not leak length
   information about the secret string */
bool dstr_eq_consttime(const dstr_t *provided, const dstr_t *secret);
