/* Other dns servers implement rrl based on who is asking and what is being
   asked, but we base our rrl only on who is asking, because there's really
   nothing interesting to ask very many times.

   Also, the only case which might be interesting to ask our dns server
   multiple times would be *.user.splintermail.com (maybe a large number of
   splintermail users are using one resolver) but of course the available
   uniqueness is infinite, so distinguishing those would break the rrl.

   Each bucket is addressed as a direct hash of the recipient address.  Hash
   collisions are unfortunate but we will assume they are rare enough to
   ignore.

   Each bucket is 5 bits of timestamp and 3 bits of counter.  The counter is
   invalid when the timestamp does not match the current time and is zeroized
   when checked again.  Theoretically a client could query ping exactly every
   32 seconds and never reset their timer. */
typedef struct {
    // each bucket is 5 bits of timestamp and 3 bits of counter.
    uint8_t *buckets;
    size_t nbuckets;
} rrl_t;

derr_t rrl_init(rrl_t *rrl, size_t nbuckets);
void rrl_free(rrl_t *rrl);

// returns true if this address is under the limit
bool rrl_check(rrl_t *rrl, const struct sockaddr *sa, xtime_t now);
