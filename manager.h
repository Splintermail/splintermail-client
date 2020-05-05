#ifndef MANAGER_H
#define MANAGER_H

/* General interface for two-phase shutdown.  This is useful for hierarchical
   ownership structures in multithreaded situations.  The general rules are:
    1. On error, tell your children to die, and report "dying" to your manager
    2. After you report "dying", wait for all children to report "dead"
    3. Wait for any floating references to be released
    4. Report "dead" to your manager
    5. As your manager frees you, you also free your children

   Corrolaries are:
    1. Hitting an error, your child saying "dying", or your manager saying "die"
       are often equivalent (unless you can gracefully handle a child dying).
    2. Your child releasing a reference or reporting "dead" are equivalent.
    3. It doesn't make sense for your manager to hold a reference of yours...
    4. ... instead, your manager should keep its own reference count.

   Conclusions are:
    1. Most engine_data_t's already behave similar to this, except that only
       one of them (the imape_data_t) actually needs the final free step.

   Alternate solution:
       object is freed on the last ref-down, but its owner also has a
       reference.  The "dying" call happens the same, but for its owner that
       just means "downref the object when you'd like to".  So it's more of a
       "dying" call in one direction and an "allowed to die" in the other.

       This is how GObject reference counts work.  My concern is:
         - This will lead to a necessity of reference counting everywhere
         - This will make callback-based objects hard to write.

       I think these are valid concerns: I think the fetch_controller would
       have to give a reference to the imap_client, and the imap client would
       have to downref the fetch controller when it was done making calls in
       order to be sure that the imap_client can't make one last call into a
       freed fetch_controller.  The strategy I've outlined is designed such
       that an object's lifetime must envelop the lifetimes of all the objects
       it owns, and I'm comfortable with that limitation.  Perhaps such a
       limitation is invalid for a cross-language codebase like GObject, but
       it should be fine in my C library. It will save me from having to deal
       with bidirectional reference counts when I prefer hierarchical object
       layouts anyway.

       Update: I don't think these are valid concerns anymore.  But there is
       one thing which is not well addressed here: how do you tell your peers
       that you are dying?  That case is not handled at all in the framework
       outlined above.  In a way, all of the GObject items are peers of each
       other, which makes for a nice symmetry.  But in that case, how does
       error handling work?

   Possible types of calls:
     - I'm dying and I'm entrusting this error to you
     - I'm dying, but you I'm not duplicating my error for the likes of you
     - I'm your parent object and you must die now

   Possible solutions:
     - You could SPLIT your error for every "dying" call, and free the original
       when your refs go to zero.  That fixes the symmetry problem but it would
       result in weird stack traces.  I guess you could special case it so that
       if you only had one referrer you didn't SPLIT at all.
     - You could not send your error at all, but your manager could take you
       instead.  This fixes the arbitrary SPLITs in the stack traces, but it
       doesn't address the problem of legitmate needs for SPLITs.  This is sort
       of a pull strategy for errors, but I think the push strategy is more
       flexible here.
           The problem with the push strategy is that the child object tells
           the parent object when to die, when it should be the parent who
           decides if it can handle the error or not.  A "dying" call that
           sends errors to all the right people seems best still.

     -  But with a generic pushing strategy then you have the two-sided
        interfaces everywhere, because for every call to "dying", the caller
        needs to know which callee gets which error, and the callee needs to
        know which referenced object is dying, so it can decide what to do.

   Conclusion

       I think that the current strategy in the imap_session, which is
       essentially a once-off error handling flow (all the actors are named,
       each one knows the rules for shutting down the system), is probably the
       most scalable.  Any error handling framework that was general enough to
       handle any situation would probably be uselessly complicated.

       The manager interface is really good for hierarchichal error flows, and
       maybe we introduce one additional flow, which can be an accessor error
       flow, where a shared resource should BROADCAST its error to every
       accessor.

UPDATE:
    more thoughts, or "wow this seems so much harder than it ought to be"

    First, the manager_i model is surprisingly similar to the "structured" part
    of CSP, where a subprocess always executes within the lifetime of its
    parent process.  I like this model.

    There's a hiccup with the manager_i interface and libuv: uv_async_t's can
    only be closed asynchronously after they are created.  That has led to a
    forked initialization API, where you call thing_init(), then EITHER
    thing_start() follwed by the normal API or thing_cancel(), where cancel()
    denotes that the rest of the normal manager_i calls will be ignored.

    This is obnoxious but it keeps the intermediate error states to a minimum.

    The really obnoxious part is that it changes who frees the memory at the
    end; in the canceled case, a thing needs to free itself, potentially
    asynchronously, whereas in the normal case a thing is freed by its owner
    after it calls mgr->dead().

    Maybe all actors always free themselves?  They're the only things that
    have this weird behavior, why not let them do that?

    The owner of the actor would just have to hold a reference to keep it from
    transitioning from "dying" to "dead", and it would be freed right after
    "dead".

Another problem:
    If you use reference counts, then you have to hold references:
      - on each thing you own
      - on yourself for each thing you own

    That's kinda weird.  I guess normally you would own references for all of
    your peers, but in the hierarchical setup of manager_i you track the
    references of yourself on behalf of the things you own.

    The really obnoxious part is that you have to store them separately, since
    you can't downref yourself for things you own until they call mgr->dead(),
    but they can't all mgr->dead() until you release them, which you will never
    do unless you distinguish between the two types of refs.

    No... this isn't true.  As soon as you decide to close, you are safe to
    ref_dn the things you own; but you are required to not make additional
    calls into those things.  That's the semantic meaning of ref_dn anyway.

Conclusion of update:
    Your problems with ref counting were because of your hesitancy to guard all
    calls into a thing against asynchronous errors.  It's easy enough with
    actors, since it can be handled neatly by .close_onthread(), but for
    multi-threaded things, you basically have to wrap every external call in a
    mutex lock to ensure close() isn't called while you are in the middle of
    processing some other call.

    The only alternative would be to use a reference count *just* to decide
    when you should release the things you own, and do a check-quit/ref-up
    dance at the entrance to every external call.  That sounds awful.
*/

struct manager_i;
typedef struct manager_i manager_i;

struct manager_i {
    // report "dying" when you start failing
    void (*dying)(manager_i*, void *caller, derr_t);
    // report "dead" when you are ready to be freed
    void (*dead)(manager_i*, void *caller);
};

// is the .dead call always useful?  I think not
static inline void noop_mgr_dead(manager_i *mgr, void *caller){
    (void)mgr;
    (void)caller;
}

#endif // MANAGER_H
