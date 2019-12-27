#ifndef IMAP_MAILDIR_H
#define IMAP_MAILDIR_H

#include <uv.h>

#include "common.h"
#include "hashmap.h"
#include "imap_expression.h"
#include "link.h"
#include "jsw_atree.h"
#include "manager.h"
#include "imap_msg.h"

/*
   What things do we need to cache on the filesystem?
      Which files are present?
        stage 0: always read from disk
        stage 1: store metadata, assume no external edits
        stage 2: on open: read metadata, enable inotify, then double-check FS

      UIDVALIDITY:
        this one's pretty critical, honestly

      SUBSCRIBE status:
        I think always outsourcing this to the main server is best.

    What happens right now when mutt adds a folder to the FS?  How does
    offlineimap handle it?
        It renames them.  I can do that.
*/

struct maildir_i;
typedef struct maildir_i maildir_i;
struct dirmgr_i;
typedef struct dirmgr_i dirmgr_i;
struct accessor_i;
typedef struct accessor_i accessor_i;
struct imaildir_t;
typedef struct imaildir_t imaildir_t;

/* maildir deletion can happen at any time.  The actual deletion will block
   until all registered imaildir_t accessors are notified and have
   deregistered. */
struct accessor_i {
    // a client accessor returns true, a server accessor returns false
    bool (*upwards)(accessor_i *accessor);
    /* release() tells the accessor to let go of the maildir, possibly due to
       an error.  release() also tells the accessor the maildir will not call
       into the accessor again. */
    void (*release)(accessor_i *accessor, derr_t error);
};

/* imalidir_t-provided interface, required by accessors.  Currently only
   provided by the dirmgr_t, since the unregister call must be synchronized.
   All calls are thread-safe. */
struct maildir_i {
    // an accessor submits an updated msg_meta_t to the imaildir
    derr_t (*update_flags)(maildir_i*, unsigned int uid, msg_flags_t old,
            msg_flags_t new, size_t *seq);
    // an accessor submits a new message to the maildir
    derr_t (*new_msg)(maildir_i*, const dstr_t *filename,
            msg_flags_t *flags, size_t *seq);
    // an accessor expunges a message from the maildir
    derr_t (*expunge_msg)(maildir_i*, unsigned int uid, size_t *seq);

    // // create a new temporary file in tmp/
    // derr_t (*fopen_tmp)(maildir_i*, const char *mode, FILE**,
    //         dstr_t *filename);
    // open a message by its uid
    derr_t (*fopen_by_uid)(maildir_i*, unsigned int uid, const char *mode,
            FILE**);
    // tell the maildir we have accepted one or more changes
    void (*reconcile_until)(maildir_i*, size_t seq);
    // // open a temporary message to write to (with corresponding msg_ref_t)
    // derr_t (*new_msg)(maildir_i*, FILE**, msg_ref_t**);

    /*
    There are some problems with this API:
      - no support for STORE with entire sequences of messages
      - there needs to be a request_id because some update requests will have
        to be passed to a client accessor, so the client can find out from the
        server what the decided action is (and only then can the imaildir_t
        respond to the server accessor that requested the update).
      - additional, architectural problems fusee imap_msg.h
    */
};

// dirmgr_t-provided interface, required by imaildir_t
struct dirmgr_i {
    /* if maildir hits a state where it knows it is no longer valid.  Actual
       derr_t's are BROADCASTed to the accessors; dirmgr does not care. */
    void (*maildir_failed)(dirmgr_i*);
    // all_unregistered is called any time the number of acessors goes to zero.
    void (*all_unregistered)(dirmgr_i*);
};

// IMAP maildir
struct imaildir_t {
    dirmgr_i *mgr;
    // path to this maildir on the filesystem
    string_builder_t path;
    unsigned int uid_validity;
    // mailbox flags
    ie_mflags_t mflags;

    // lock ordering (when required) is content, then access

    struct {
        uv_rwlock_t lock;
        // contents of this folder, mapping uid -> imsg_t*
        hashmap_t msgs;
        // msg_meta_t's which are not up-to-date but still used by an accessor
        link_t unreconciled;
        // seq is the id of the most up-to-date change
        size_t seq;
    } content;

    struct {
        uv_mutex_t mutex;
        link_t accessors;  // acc_t->link
        /* The value of naccessors may not match the length of the accessors
           list, particularly during the failing shutdown sequence. */
        size_t naccessors;
        /* failed refers to the whole imaildir_t, but the only race condition
           is related to rejecting incoming accessors, so it falls under the
           same mutex as naccessors */
        bool failed;
    } access;
};

// open a maildir at path, path must be linked to long-lived objects
derr_t imaildir_init(imaildir_t *m, string_builder_t path, dirmgr_i *mgr);
// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m);

// useful if a maildir needs to be deleted but it has accessors or something
void imaildir_close(imaildir_t *m);

// void imaildir_ref_up(imaildir_t *m);
// void imaildir_ref_down(imaildir_t *m);

// register an accessor, provide a maildir_i, and build a view
derr_t imaildir_register(imaildir_t *m, accessor_i *accessor,
        maildir_i **maildir_out, jsw_atree_t *view_out);
/* there is a race condition between imalidir_register and imaildir_unregister;
   generally, if you got a maildir_i* through dirmgr_open, you should close it
   via dirmgr_close */
void imaildir_unregister(maildir_i *maildir);

#endif // IMAP_MAILDIR_H
