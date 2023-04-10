#include "libduv/libduv.h"

/* fake_stream_t presents a stream_i but just collects things passed into it,
   allowing test code to make the stream_i act any way it pleases */
typedef struct {
    stream_i iface;
    link_t reads;
    link_t writes;
    /* how many bufs of the oldest write have been returned; after the last
       buf has been returned the oldest write transitions to writes_popped */
    unsigned int bufs_returned;
    link_t writes_popped;
    stream_shutdown_cb shutdown_cb;
    stream_await_cb await_cb;
} fake_stream_t;
DEF_CONTAINER_OF(fake_stream_t, iface, stream_i)

#define EXPECT_FAKE_STREAM_STATE(e, f, shutdown, canceled)

bool fake_stream_want_read(fake_stream_t *f);
// pukes if no reads are wanted
dstr_t fake_stream_feed_read(fake_stream_t *f, dstr_t input);
// pukes if no reads are wanted OR if there would be any leftovers
void fake_stream_feed_read_all(fake_stream_t *f, dstr_t input);
bool fake_stream_want_write(fake_stream_t *f);
// pukes if no writes are ready
dstr_t fake_stream_pop_write(fake_stream_t *f);
bool fake_stream_want_write_done(fake_stream_t *f);
// pukes if no writes have been popped
void fake_stream_write_done(fake_stream_t *f);
void fake_stream_shutdown(fake_stream_t *f);
void fake_stream_done(fake_stream_t *f, derr_t error);
stream_i *fake_stream(fake_stream_t *f);

// expects exactly one read
derr_t fake_stream_expect_read(
    manual_scheduler_t *m, fake_stream_t *fs, dstr_t exp
);

// expects reads broken up by any boundaries
derr_t fake_stream_expect_read_many(
    manual_scheduler_t *m, fake_stream_t *fs, dstr_t exp
);

// close and cleanup a stream and the fake_stream on which it was built
derr_t fake_stream_cleanup(
    manual_scheduler_t *m, stream_i *s, fake_stream_t *f
);
