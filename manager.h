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
*/

struct manager_i;
typedef struct manager_i manager_i;

struct manager_i {
    // report "dying" when you start failing
    void (*dying)(manager_i*, derr_t);
    // report "dead" when you are ready to be freed
    void (*dead)(manager_i*);
};

#endif // MANAGER_H
