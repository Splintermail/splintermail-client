/* retry_t usage example:

    typedef struct {
        retry_t retry;
        time_t backoff_until;
        bool thing_sent;
        bool thing_done;
    } state_t;

    void advance_state(state_t *state, time_t now){

        if(!state->thing_sent){
            // check if we are in a backoff time
            if(now < state->backoff_until) return;

            // start the attempt
            state->thing_sent = true;
            thing_do(..., thing_cb, state);

            // configure 10 retries with an exponential backoff
            retry_start(&state->retry, 10, 1, 2, 4, 8, 15, 30, 60);
        }
        if(!state->thing_done) return;
    }

    void thing_cb(void *data, derr_t err){
        state_t *state = data;

        // we want to guarantee one call to retry_check() or retry_free()
        // after each call to retry_start(), so we can piggyback off of the
        // exactly-once semantics of thing_cb() by placing the retry logic at
        // the top of the thing_cb().

        // first, optionally, filter errors you want to retry
        if(err_is_retryable(err.type)){
            // decide if we should retry
            bool again = retry_check(&state->retry, &err);
            if(again){
                // time to retry
                time_t now = ...;
                // capture and remember the backoff time
                state->backoff_until = now + state->retry.backoff;
                // configure a delayed wakeup
                delayed_wakeup(state, state->backoff_until);
                // clear flags so that we actually retry
                state->thing_sent = false;
                goto done;
            }
        }else{
            // success, or non-retryable error case
            retry_free(&state->retry);
        }

        // check for error cases
        MULTIPROP_VAR_GO(&state->err, done, &err);

        // success logic
        state->thing_done = true;

    done:
        schedule(state);
    }
*/

typedef struct {
    // errors we've hit so far
    dstr_t errs;
    // total number of attempts to make
    uint64_t limit;
    // total number of attempts so far
    uint64_t attempts;
    // the next backoff time, if we fail
    uint64_t backoff;
} retry_t;

// limit is the number of tries to do
/* backoffs is how long to backoff each time; if the provided backoffs array
   is too short the final value is reused repeatedly */
// every call to retry_start MUST have a call to retry_check() or retry_free()
void _retry_start(
    retry_t *r, uint64_t limit, uint64_t *backoffs, uint64_t nbackoffs
);
// retry_start(r, COUNT, BACKOFF...)
#define retry_start(r, l, ...) \
    _retry_start( \
        r, \
        l, \
        (uint64_t[]){__VA_ARGS__}, \
        sizeof((uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t) \
    )

// retry check returns true if you should retry
/* On input, you have three cases:
   - err is E_NONE: this just calls retry_free and returns false
   - err, but limit not reached: copy and clear err and return true
   - err and limit reached: copy and clear err and return true

   After calling you have three cases:
   - success case: reset_t is cleared and you should proceed
   - retry case: reset_t stores info, and you should take retry steps
   - failure case: reset_t is cleared and you should take failure steps */
bool retry_check(retry_t *r, derr_t *err);

void retry_free(retry_t *r);
