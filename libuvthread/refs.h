struct refs_t;
typedef struct refs_t refs_t;

typedef void (*finalizer_t)(refs_t*);

struct refs_t {
    dmutex_t mutex;
    int count;
    // call finalize when count goes to zero, must not be NULL
    finalizer_t finalize;
    // only useful for opportunistic error printing
    bool freed;
};

derr_t refs_init(refs_t *refs, int starting_count, finalizer_t finalizer);
// refs_t should normally be called by the finalizer_t
void refs_free(refs_t *refs);

void ref_up(refs_t *refs);
void ref_dn(refs_t *refs);
