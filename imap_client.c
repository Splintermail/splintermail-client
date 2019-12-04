#include "logger.h"
#include "imap_client.h"
#include "imap_read.h"
#include "queue.h"
#include "link.h"
#include "imap_expression.h"
#include "imap_expression_print.h"
#include "imap_write.h"

#define IC_TAG_SIZE 16

/* The mutable state of an upwards imap session.  This is allocated by an
   imape_data_t during the call to logic_alloc.  When an event comes in for an
   imape_data_t, the imape_worker_t will call the functions in the imap_logic_t
   interface.  Then we can dereference the imap_client_t struct, update the
   client state, and send any resulting write events.

   The imap client state machine:

   PREGREET      // where you start
   PRECAPA       // before you have received a CAPABILITY response
   PREAUTH      // before your login has been accepted
   // Anything below is logged in
   UNSELECTED / SELECTED // whether or not we are in an imbox

*/
typedef enum {
    // general states
    PREGREET = 0,   /* before receiving the greeting */
    PRECAPA,        /* if you receive a greeting with no capabilities */
    PREAUTH,        /* after login */
    AUTHENTICATED,  /* imap rfc "authenticated" state */
    PRECLOSE,

    // states for LIST command
    LISTING,        /* after sending LIST, before receiving response */

    // states for SET_FOLDER command
    SELECTING,      /* after sending SELECT, before receiving response */
    SEARCHING,      /* listing UIDs from selected state */
    FETCHING,       /* some FETCH commands are queued but not completed */
    CLOSING,        /* after sending CLOSE */

    SELECTED,       /* imap rfc "selected" state */

} imap_client_state_t;
static const dstr_t *imap_client_state_to_dstr(imap_client_state_t state);

// any_of checks if a value is in a given list of enum values
static inline bool any_of(int val, const int *list, size_t list_len){
    for(size_t i = 0; i < list_len; i++){
        if(val == list[i]){
            return true;
        }
    }
    return false;
}
// ANY_OF is a macro for using any_of in a sane way
#define ANY_OF(val, ...) \
    any_of(val, (const int[]){__VA_ARGS__}, \
        sizeof((const int[]){__VA_ARGS__}) / sizeof(int))

struct imap_client_t;
typedef struct imap_client_t imap_client_t;

// a callback for when the OK response comes in to complete a command
typedef derr_t (*cmd_done_t)(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st);

// the imap_client-specific wrapper for an imap_cmd_t
typedef struct {
    imap_cmd_t *cmd;
    link_t link;  // imap_client_t->unwritten or imap_client_t->unresponded
    // completion hooks, only ok is mandatory
    cmd_done_t ok;
    cmd_done_t no;
    cmd_done_t bad;
} ic_cmd_t;
DEF_CONTAINER_OF(ic_cmd_t, link, link_t)

// the imap_client-specific wrapper for an imap_response_t
typedef struct {
    imap_resp_t *resp;
    link_t link;
} ic_resp_t;
DEF_CONTAINER_OF(ic_resp_t, link, link_t)

typedef enum {
    GOAL_NONE,
    GOAL_LOGIN,
    GOAL_LIST,
    GOAL_SYNC,
} ic_goal_t;
static const dstr_t *goal_to_dstr(ic_goal_t goal);

// we don't need the full ie_flags_t, we just need to know about a subset
typedef struct {
    bool answered:1;
    bool flagged:1;
    bool deleted:1;
    bool seen:1;
    bool draft:1;
} supported_flags_t;

// we don't need the full ie_pflags_t, we just need to know about a subset
typedef struct {
    bool answered:1;
    bool flagged:1;
    bool deleted:1;
    bool seen:1;
    bool draft:1;
    bool asterisk:1;  // the "\*" flag
} supported_pflags_t;

typedef struct {
    // responses which may come in between SELECT command and OK response
    supported_flags_t supp_flags;
    supported_pflags_t supp_pflags;
    unsigned int uidvalidity;
    unsigned int uidnext;
    unsigned int unseen;
    unsigned int exists;
    unsigned int recent;
    // keep track of all of the things we expect to see
    struct {
        bool supp_flags:1;
        bool supp_pflags:1;
        bool uidvalidity:1;
        bool uidnext:1;
        bool unseen:1;
        bool exists:1;
        bool recent:1;
    } got;
    size_t fetches_pending;
} folder_sync_t;

struct imap_client_t {
    // the imap_logic_t is exposed to the imape_data_t
    imap_logic_t logic;
    imape_data_t *id;
    const imap_client_spec_t *spec;
    const imap_controller_up_t *controller;
    imap_reader_t reader;
    // write subsystem
    queue_cb_t write_qcb;
    bool write_requested;
    event_t *write_ev;
    size_t write_skip;

    link_t unread; // event_t->link
    link_t unhandled; // ic_resp_t->link
    link_t unwritten;  // ic_cmd_t->link
    link_t unresponded;  // ic_cmd_t->link
    // which folder to be syncing
    dstr_t folder;
    // the state of syncing that folder
    folder_sync_t sync;
    // general IMAP state
    unsigned int next_tag;
    imap_client_state_t imap_state;
    ic_goal_t goal;
    bool saw_capas;
    // accumulated LIST response
    jsw_atree_t folders;
};
DEF_CONTAINER_OF(imap_client_t, logic, imap_logic_t)
DEF_CONTAINER_OF(imap_client_t, write_qcb, queue_cb_t)

static void folder_sync_free(folder_sync_t *sync){
    // zeroize everything
    *sync = (folder_sync_t){0};
}

static derr_t ic_cmd_new(ic_cmd_t **out, imap_cmd_t *cmd, cmd_done_t ok,
        cmd_done_t no, cmd_done_t bad){
    derr_t e = E_OK;
    *out = NULL;

    // only ok is mandatory
    if(ok == NULL){
        ORIG(&e, E_INTERNAL, "unable to create ic_cmd with ok==NULL");
    }

    ic_cmd_t *ic_cmd = malloc(sizeof(*ic_cmd));
    if(ic_cmd == NULL) ORIG(&e, E_NOMEM, "no memory");
    *ic_cmd = (ic_cmd_t){0};

    ic_cmd->cmd = cmd;
    ic_cmd->ok = ok;
    ic_cmd->no = no;
    ic_cmd->bad = bad;
    link_init(&ic_cmd->link);

    *out = ic_cmd;
    return e;
}

static void ic_cmd_free(ic_cmd_t *ic_cmd){
    link_remove(&ic_cmd->link);
    imap_cmd_free(ic_cmd->cmd);
    free(ic_cmd);
}

static derr_t ic_resp_new(ic_resp_t **out, imap_resp_t *resp){
    derr_t e = E_OK;
    *out = NULL;

    ic_resp_t *ic_resp = malloc(sizeof(*ic_resp));
    if(ic_resp == NULL) ORIG(&e, E_NOMEM, "no memory");
    *ic_resp = (ic_resp_t){0};

    ic_resp->resp = resp;
    link_init(&ic_resp->link);

    *out = ic_resp;
    return e;
}

static void ic_resp_free(ic_resp_t *ic_resp){
    imap_resp_free(ic_resp->resp);
    link_remove(&ic_resp->link);
    free(ic_resp);
}

static ie_dstr_t *ic_next_tag(derr_t *e, imap_client_t *ic){
    if(is_error(*e)) goto fail;

    DSTR_VAR(buf, 32);
    PROP_GO(e, FMT(&buf, "t%x", FU(ic->next_tag++)), fail);

    return ie_dstr_new(e, &buf, KEEP_RAW);

fail:
    return NULL;
}

static derr_t login_done(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st){
    derr_t e = E_OK;
    (void)cmd;
    (void)st;

    if(ic->imap_state != PREAUTH){
        ORIG(&e, E_INTERNAL, "arrived at login_done out of PREAUTH state");
    }
    ic->imap_state = AUTHENTICATED;

    if(ic->goal != GOAL_LOGIN){
        ORIG(&e, E_INTERNAL, "arrived at login_done without GOAL_LOGIN");
    }
    ic->goal = GOAL_NONE;

    // tell the controller we finished logging in
    ic->controller->logged_in(ic->controller, ic->id->session);

    return e;
}

static derr_t send_login(imap_client_t *ic){
    derr_t e = E_OK;

    ic->imap_state = PREAUTH;

    // build the login command
    ie_dstr_t *user = ie_dstr_new(&e, &ic->spec->user, KEEP_RAW);
    ie_dstr_t *pass = ie_dstr_new(&e, &ic->spec->pass, KEEP_RAW);
    imap_cmd_arg_t arg = {.login=ie_login_cmd_new(&e, user, pass)};

    ie_dstr_t *tag = ic_next_tag(&e, ic);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_LOGIN, arg);
    CHECK(&e);

    ic_cmd_t *ic_cmd;
    PROP(&e, ic_cmd_new(&ic_cmd, cmd, login_done, NULL, NULL) );

    // add to unwritten commands
    link_list_append(&ic->unwritten, &ic_cmd->link);

    return e;
}

// puke if a needed capability is missing
static derr_t check_capas(ie_dstr_t *capas){
    derr_t e = E_OK;

    bool found_imap4rev1 = false;
    for(ie_dstr_t *capa = capas; capa != NULL; capa = capa->next){
        DSTR_VAR(buf, 32);
        // ignore long capabilities
        if(capa->dstr.len > buf.size) continue;
        // case-insensitive matching
        PROP(&e, dstr_copy(&capa->dstr, &buf) );
        dstr_lower(&buf);
        if(dstr_cmp(&buf, &DSTR_LIT("imap4rev1")) == 0){
            found_imap4rev1 = true;
        }
    }

    bool pass = true;
    if(!found_imap4rev1){
        TRACE(&e, "missing capability: IMAP4rev1\n");
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_RESPONSE, "IMAP server is missing capabilties");
    }
    return e;
}

static derr_t capas_done(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st){
    derr_t e = E_OK;
    (void)cmd;
    (void)st;
    if(!ic->saw_capas){
        ORIG(&e, E_RESPONSE, "never saw capabilities");
    }
    PROP(&e, send_login(ic) );
    return e;
}

static derr_t send_capas(imap_client_t *ic){
    derr_t e = E_OK;

    ic->imap_state = PRECAPA;
    ic->saw_capas = false;

    // issue the capability command
    imap_cmd_arg_t arg = {0};
    ie_dstr_t *tag = ic_next_tag(&e, ic);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_CAPA, arg);
    CHECK(&e);

    ic_cmd_t *ic_cmd;
    PROP(&e, ic_cmd_new(&ic_cmd, cmd, capas_done, NULL, NULL) );

    // add to unwritten commands
    link_list_append(&ic->unwritten, &ic_cmd->link);

    return e;
}

static derr_t list_done(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st){
    (void)cmd;
    (void)st;
    derr_t e = E_OK;

    if(ic->imap_state != LISTING){
        ORIG(&e, E_INTERNAL, "arrived at list_done out of LISTING state");
    }
    ic->imap_state = AUTHENTICATED;

    if(ic->goal != GOAL_LIST){
        ORIG(&e, E_INTERNAL, "arrived at list_done without GOAL_LIST");
    }
    ic->goal = GOAL_NONE;

    ic->controller->folders(ic->controller, ic->id->session, &ic->folders);

    // free folders
    jsw_anode_t *node = jsw_apop(&ic->folders);
    for(; node != NULL; node = jsw_apop(&ic->folders)){
        ie_list_resp_t *list = CONTAINER_OF(node, ie_list_resp_t, node);
        ie_list_resp_free(list);
    }

    return e;
}

static derr_t list_resp(imap_client_t *ic, ie_list_resp_t *list){
    derr_t e = E_OK;

    jsw_ainsert(&ic->folders, &list->node);

    return e;
}

static derr_t send_list(imap_client_t *ic){
    derr_t e = E_OK;

    if(ic->imap_state != AUTHENTICATED){
        ORIG(&e, E_INTERNAL,
                "arrived at send_list out of AUTHENTICATED state");
    }

    ic->imap_state = LISTING;

    // issue the list command
    ie_dstr_t *name = ie_dstr_new(&e, &DSTR_LIT(""), KEEP_RAW);
    ie_mailbox_t *ref_name = ie_mailbox_new_noninbox(&e, name);
    ie_dstr_t *pattern = ie_dstr_new(&e, &DSTR_LIT("*"), KEEP_RAW);
    imap_cmd_arg_t arg = { .list=ie_list_cmd_new(&e, ref_name, pattern) };

    ie_dstr_t *tag = ic_next_tag(&e, ic);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_LIST, arg);
    CHECK(&e);

    ic_cmd_t *ic_cmd;
    PROP_GO(&e, ic_cmd_new(&ic_cmd, cmd, list_done, NULL, NULL), fail);

    // add to unwritten commands
    link_list_append(&ic->unwritten, &ic_cmd->link);

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

static derr_t close_done(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st){
    (void)cmd;
    (void)st;
    derr_t e = E_OK;

    if(ic->imap_state != CLOSING){
        ORIG(&e, E_INTERNAL, "arrived at close_done out of CLOSING state");
    }

    ic->imap_state = AUTHENTICATED;

    /* TODO: respond to the controller without having to close the folder.
             That will require allowing new goals to preempt old goals */

    ic->goal = GOAL_NONE;

    ic->controller->uptodate(ic->controller, ic->id->session);

    return e;
}

static derr_t send_close(imap_client_t *ic){
    derr_t e = E_OK;

    ic->imap_state = CLOSING;

    // issue a CLOSE command
    imap_cmd_arg_t arg = {0};
    ie_dstr_t *tag = ic_next_tag(&e, ic);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_CLOSE, arg);
    CHECK(&e);

    ic_cmd_t *ic_cmd;
    PROP_GO(&e, ic_cmd_new(&ic_cmd, cmd, close_done, NULL, NULL), fail);

    // add to unwritten commands
    link_list_append(&ic->unwritten, &ic_cmd->link);

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

static derr_t fetch_done(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st){
    (void)cmd;
    (void)st;
    derr_t e = E_OK;

    if(ic->imap_state != FETCHING){
        ORIG(&e, E_INTERNAL, "arrived at fetch_done out of FETCHING state");
    }

    if(ic->goal != GOAL_SYNC){
        ORIG(&e, E_INTERNAL, "arrived at fetch_done without GOAL_SYNC");
    }

    // one less fetch command in flight
    ic->sync.fetches_pending--;

    if(ic->sync.fetches_pending == 0){
        PROP(&e, send_close(ic) );
    }

    return e;
}

static derr_t fetch_resp(imap_client_t *ic, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;

    // yup
    (void)ic;
    (void)fetch;

    return e;
}

static derr_t send_fetch(imap_client_t *ic, unsigned int uid){
    derr_t e = E_OK;

    if(ic->imap_state != SEARCHING){
        ORIG(&e, E_INTERNAL, "arrived at send_fetch out of SEARCHING state");
    }

    // issue a UID FETCH command
    bool uid_mode = true;
    // fetch a single uid
    ie_seq_set_t *uidseq = ie_seq_set_new(&e, uid, uid);
    // fetch UID, FLAGS, and RFC822
    ie_fetch_attrs_t *attr = ie_fetch_attrs_new(&e);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_UID);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_FLAGS);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_RFC822);
    // build fetch command
    ie_fetch_cmd_t *fetch = ie_fetch_cmd_new(&e, uid_mode, uidseq, attr);
    imap_cmd_arg_t arg = {.fetch=fetch};

    ie_dstr_t *tag = ic_next_tag(&e, ic);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_FETCH, arg);
    CHECK(&e);

    ic_cmd_t *ic_cmd;
    PROP_GO(&e, ic_cmd_new(&ic_cmd, cmd, fetch_done, NULL, NULL), fail);

    // add to unwritten commands
    link_list_append(&ic->unwritten, &ic_cmd->link);

    // one more fetch command queued
    ic->sync.fetches_pending++;

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

static derr_t search_done(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st){
    (void)cmd;
    (void)st;
    derr_t e = E_OK;

    if(ic->imap_state != SEARCHING){
        ORIG(&e, E_INTERNAL, "arrived at search_done out of SEARCHING state");
    }

    if(ic->goal != GOAL_SYNC){
        ORIG(&e, E_INTERNAL, "arrived at search_done without GOAL_SYNC");
    }

    if(ic->sync.fetches_pending){
        /* Transition to FETCHING state, where we will wait until we are
           done fetching */
        ic->imap_state = FETCHING;
    }else{
        PROP(&e, send_close(ic) );
    }

    return e;
}

static derr_t search_resp(imap_client_t *ic, const ie_nums_t *uids){
    derr_t e = E_OK;

    // send a UID search for each uid
    for(const ie_nums_t *uid = uids; uid != NULL; uid = uid->next){
        PROP(&e, send_fetch(ic, uid->num) );
    }

    return e;
}

static derr_t send_search(imap_client_t *ic){
    derr_t e = E_OK;

    ic->imap_state = SEARCHING;

    // issue a `UID SEARCH UID <range>` command
    bool uid_mode = true;
    ie_dstr_t *charset = NULL;
    // "1" is the first message, "0" represents "*" which is the last message
    ie_seq_set_t *range = ie_seq_set_new(&e, 1, 0);
    ie_search_key_t *search_key = ie_search_seq_set(&e, IE_SEARCH_UID, range);
    imap_cmd_arg_t arg = {
        .search=ie_search_cmd_new(&e, uid_mode, charset, search_key)
    };

    ie_dstr_t *tag = ic_next_tag(&e, ic);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_SEARCH, arg);
    CHECK(&e);

    ic_cmd_t *ic_cmd;
    PROP_GO(&e, ic_cmd_new(&ic_cmd, cmd, search_done, NULL, NULL), fail);

    // add to unwritten commands
    link_list_append(&ic->unwritten, &ic_cmd->link);

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

static derr_t select_done(imap_client_t *ic, imap_cmd_t *cmd,
        const ie_st_resp_t *st){
    (void)cmd;
    (void)st;
    derr_t e = E_OK;

    if(ic->imap_state != SELECTING){
        ORIG(&e, E_INTERNAL, "arrived at select_done out of SELECTING state");
    }

    if(ic->goal != GOAL_SYNC){
        ORIG(&e, E_INTERNAL, "arrived at select_done without GOAL_SYNC");
    }

    if(!ic->sync.got.supp_flags)
        ORIG(&e, E_INTERNAL, "SELECT response missing FLAGS");
    if(!ic->sync.got.supp_pflags)
        ORIG(&e, E_INTERNAL, "SELECT response missing PFLAGS");
    if(!ic->sync.got.uidvalidity)
        ORIG(&e, E_INTERNAL, "SELECT response missing UIDVALIDITY");
    // These are listed as REQUIRED in the spec but we can do without
    // if(!ic->sync.got.uidnext)
    //     ORIG(&e, E_INTERNAL, "SELECT response missing UIDNEXT");
    // if(!ic->sync.got.unseen)
    //     ORIG(&e, E_INTERNAL, "SELECT response missing UNSEEN");
    // if(!ic->sync.got.exists)
    //     ORIG(&e, E_INTERNAL, "SELECT response missing EXISTS");
    // if(!ic->sync.got.recent)
    //     ORIG(&e, E_INTERNAL, "SELECT response missing RECENT");

    PROP(&e, send_search(ic) );

    return e;
}

static derr_t select_flags(imap_client_t *ic, const ie_flags_t *flags){
    derr_t e = E_OK;

    if(ic->sync.got.supp_flags){
        ORIG(&e, E_INTERNAL, "got FLAGS more than once");
    }
    ic->sync.got.supp_flags = true;

    ic->sync.supp_flags.answered = flags->answered;
    ic->sync.supp_flags.flagged = flags->flagged;
    ic->sync.supp_flags.deleted = flags->deleted;
    ic->sync.supp_flags.seen = flags->seen;
    ic->sync.supp_flags.draft = flags->draft;

    return e;
}
static derr_t select_pflags(imap_client_t *ic, const ie_pflags_t *pflags){
    derr_t e = E_OK;

    if(ic->sync.got.supp_pflags){
        ORIG(&e, E_INTERNAL, "got FLAGS more than once");
    }
    ic->sync.got.supp_pflags = true;

    ic->sync.supp_pflags.answered = pflags->answered;
    ic->sync.supp_pflags.flagged = pflags->flagged;
    ic->sync.supp_pflags.deleted = pflags->deleted;
    ic->sync.supp_pflags.seen = pflags->seen;
    ic->sync.supp_pflags.draft = pflags->draft;
    ic->sync.supp_pflags.asterisk = pflags->asterisk;

    return e;
}
static derr_t select_exists(imap_client_t *ic, unsigned int num){
    derr_t e = E_OK;

    if(ic->sync.got.exists){
        ORIG(&e, E_INTERNAL, "got EXISTS more than once");
    }
    ic->sync.got.exists = true;

    ic->sync.exists = num;
    return e;
}
static derr_t select_recent(imap_client_t *ic, unsigned int num){
    derr_t e = E_OK;

    if(ic->sync.got.recent){
        ORIG(&e, E_INTERNAL, "got RECENT more than once");
    }
    ic->sync.got.recent = true;

    ic->sync.recent = num;
    return e;
}
static derr_t select_uidnext(imap_client_t *ic, unsigned int num){
    derr_t e = E_OK;

    if(ic->sync.got.uidnext){
        ORIG(&e, E_INTERNAL, "got UIDNEXT more than once");
    }
    ic->sync.got.uidnext = true;

    ic->sync.uidnext = num;
    return e;
}
static derr_t select_uidvalidity(imap_client_t *ic, unsigned int num){
    derr_t e = E_OK;

    if(ic->sync.got.uidvalidity){
        ORIG(&e, E_INTERNAL, "got UIDVLD more than once");
    }
    ic->sync.got.uidvalidity = true;

    ic->sync.uidvalidity = num;
    return e;
}
static derr_t select_unseen(imap_client_t *ic, unsigned int num){
    derr_t e = E_OK;

    if(ic->sync.got.unseen){
        ORIG(&e, E_INTERNAL, "got UNSEEN more than once");
    }
    ic->sync.got.unseen = true;

    ic->sync.unseen = num;
    return e;
}

static derr_t send_select(imap_client_t *ic){
    derr_t e = E_OK;

    if(ic->imap_state != AUTHENTICATED){
        ORIG(&e, E_INTERNAL,
                "arrived at send_select out of AUTHENTICATED state");
    }

    ic->imap_state = SELECTING;

    // reset the folder_sync_t
    ic->sync = (folder_sync_t){0};

    // issue the select command
    ie_dstr_t *folder = ie_dstr_new(&e, &ic->folder, KEEP_RAW);
    imap_cmd_arg_t arg = { .select=ie_mailbox_new_noninbox(&e, folder) };

    ie_dstr_t *tag = ic_next_tag(&e, ic);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_SELECT, arg);
    CHECK(&e);

    ic_cmd_t *ic_cmd;
    PROP_GO(&e, ic_cmd_new(&ic_cmd, cmd, select_done, NULL, NULL), fail);

    // add to unwritten commands
    link_list_append(&ic->unwritten, &ic_cmd->link);

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(imap_client_t *ic, const ie_st_code_t *code,
        const dstr_t *text){
    derr_t e = E_OK;

    // The very first message is treated specially
    if(ic->imap_state == PREGREET){
        if(code != NULL && code->type == IE_ST_CODE_CAPA){
            PROP(&e, check_capas(code->arg.dstr) );
            PROP(&e, send_login(ic) );
        }else{
            PROP(&e, send_capas(ic) );
        }
        return e;
    }

    // Handle responses where the status code is what defines the behavior
    if(code != NULL){
        switch(code->type){
            // handle codes which are independent of state
            case IE_ST_CODE_ALERT:
                LOG_ERROR("server ALERT message: %x\n", FD(text));
                return e;

            // handle codes which are specific to SELECTING state
            case IE_ST_CODE_UIDNEXT:
                if(ic->imap_state != SELECTING){
                    ORIG(&e, E_INTERNAL, "got UIDNEXT outside of SELECTING");
                }
                select_uidnext(ic, code->arg.num);
                return e;
            case IE_ST_CODE_UIDVLD:
                if(ic->imap_state != SELECTING){
                    ORIG(&e, E_INTERNAL, "got UIDVLD outside of SELECTING");
                }
                select_uidvalidity(ic, code->arg.num);
                return e;
            case IE_ST_CODE_UNSEEN:
                if(ic->imap_state != SELECTING){
                    ORIG(&e, E_INTERNAL, "got UNSEEN outside of SELECTING");
                }
                select_unseen(ic, code->arg.num);
                return e;
            case IE_ST_CODE_PERMFLAGS:
                if(ic->imap_state != SELECTING){
                    ORIG(&e, E_INTERNAL, "got PERMFLAGS outside of SELECTING");
                }
                select_pflags(ic, code->arg.pflags);
                return e;

            default:
                break;
        }
    }

    switch(ic->imap_state){
        case PREGREET: /* not possible, already handled */
            break;

        // states which currently can't handle untagged messages at all
        /* TODO: this should not throw an error eventually, but in development
                 it will be helpful to not silently drop them */
        case PRECAPA:
        case PREAUTH:
        case AUTHENTICATED:
        case PRECLOSE:
        case LISTING:
        case SELECTING:
        case SEARCHING:
        case FETCHING:
        case CLOSING:
            TRACE(&e, "unhandled * OK status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;

        // TODO: this *certainly* should not throw an error
        case SELECTED:
            TRACE(&e, "unhandled * OK status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
    }

    return e;
}

static derr_t status_type(imap_client_t *ic, const ie_st_resp_t *st){
    derr_t e = E_OK;

    // tagged responses
    if(st->tag != NULL){
        // peek at the first command we need a response to
        link_t *link = ic->unresponded.next;
        if(link == NULL){
            TRACE(&e, "got tag %x with no commands in flight\n",
                    FD(&st->tag->dstr));
            ORIG(&e, E_RESPONSE, "bad status type response");
        }
        // make sure the tag matches
        ic_cmd_t *ic_cmd = CONTAINER_OF(link, ic_cmd_t, link);
        if(dstr_cmp(&st->tag->dstr, &ic_cmd->cmd->tag->dstr) != 0){
            TRACE(&e, "got tag %x but expected %x\n",
                    FD(&st->tag->dstr), FD(&ic_cmd->cmd->tag->dstr));
            ORIG(&e, E_RESPONSE, "bad status type response");
        }
        switch(st->status){
            case IE_ST_OK:
                // response completed successfully, call the hook
                PROP(&e, ic_cmd->ok(ic, ic_cmd->cmd, st) );
                break;

            case IE_ST_NO: // response completed unsuccessfully
                if(ic_cmd->no){
                    PROP(&e, ic_cmd->no(ic, ic_cmd->cmd, st) );
                }else{
                    TRACE(&e, "command of type %x with tag \"%x\" failed with "
                            "message \"%x: %x\"\n",
                            FD(imap_cmd_type_to_dstr(ic_cmd->cmd->type)),
                            FD(&ic_cmd->cmd->tag->dstr),
                            FD(ie_status_to_dstr(st->status)),
                            FD(&st->text->dstr));
                    ORIG(&e, E_INTERNAL, "failed imap command");
                }
                break;

            case IE_ST_BAD: // command was invalid
                if(ic_cmd->bad){
                    PROP(&e, ic_cmd->bad(ic, ic_cmd->cmd, st) );
                }else{
                    TRACE(&e, "command of type %x with tag \"%x\" failed with "
                            "message \"%x: %x\"\n",
                            FD(imap_cmd_type_to_dstr(ic_cmd->cmd->type)),
                            FD(&ic_cmd->cmd->tag->dstr),
                            FD(ie_status_to_dstr(st->status)),
                            FD(&st->text->dstr));
                    ORIG(&e, E_INTERNAL, "failed imap command");
                }
                break;

            default:
                // not all responses are allowed to be tagged
                TRACE(&e, "invalid tagged %x response with tag \"%x\"\n",
                        FD(ie_status_to_dstr(st->status)), FD(&st->tag->dstr));
                ORIG(&e, E_RESPONSE, "bad status type response");
        }

        // we are done with this command
        link_remove(&ic_cmd->link);
        ic_cmd_free(ic_cmd);
    }

    // untagged responses
    else{
        switch(st->status){
            case IE_ST_OK:
                // informational message
                PROP(&e, untagged_ok(ic, st->code, &st->text->dstr) );
                break;
            case IE_ST_NO:
                // a warning about a command
                // TODO: handle this
                TRACE(&e, "unhandled * NO status message\n");
                ORIG(&e, E_INTERNAL, "unhandled message");
                break;
            case IE_ST_BAD:
                // an error not from a command, or not sure from which command
                // TODO: handle this
                TRACE(&e, "unhandled * BAD status message\n");
                ORIG(&e, E_INTERNAL, "unhandled message");
                break;
            case IE_ST_PREAUTH:
                // only allowed as a greeting
                // TODO: handle this
                TRACE(&e, "unhandled * PREAUTH status message\n");
                ORIG(&e, E_INTERNAL, "unhandled message");
                break;
            case IE_ST_BYE:
                // we are logging out or server is shutting down.
                // TODO: handle this
                TRACE(&e, "unhandled * BYE status message\n");
                ORIG(&e, E_INTERNAL, "unhandled message");
                break;
            default:
                TRACE(&e, "invalid status of unknown type %x\n", FU(st->status));
                ORIG(&e, E_INTERNAL, "bad imap parse");
        }
    }

    return e;
}

static void resp_cb(void *cb_data, imap_resp_t *resp){
    imap_client_t *ic = cb_data;
    derr_t e = E_OK;

    // print the response
    {
        DSTR_VAR(buf, 4096);
        size_t skip = 0;
        size_t want = 1;
        while(want > 0){
            buf.len = 0;
            derr_t e2 = E_OK;
            IF_PROP(&e2, imap_resp_write(resp, &buf, &skip, &want) ){
                DUMP(e2);
                DROP_VAR(&e2);
                break;
            }
        }
        LOG_DEBUG("read: %x", FD(&buf));
    }

    // wrap the response
    ic_resp_t *ic_resp;
    PROP_GO(&e, ic_resp_new(&ic_resp, resp), fail);

    // append to unhandled
    link_list_append(&ic->unhandled, &ic_resp->link);

    return;

fail:
    ic->id->session->close(ic->id->session, e);
    PASSED(e);
}

// state machine functions

// we use the callback form, queue_pop_first_cb(), to request write events
static void ic_prewait_cb(queue_cb_t *qcb){
    /* upref the session while we wait for a write event.  When we receive one,
       we will just reuse this reference */
    imap_client_t *ic = CONTAINER_OF(qcb, imap_client_t, write_qcb);
    ic->id->ref_up(ic->id->session, IMAPE_REF_WRITE);
    // mark that we have already requested a write
    ic->write_requested = true;
}

// this function happens on the main imape_t thread!
static void ic_new_data_cb(queue_cb_t *qcb, link_t *link){
    // dereference args
    imap_client_t *ic = CONTAINER_OF(qcb, imap_client_t, write_qcb);
    event_t *ev = CONTAINER_OF(link, event_t, link);
    // mark session (upref happened already, in prewait)
    ev->session = ic->id->session;
    ev->ev_type = EV_WRITE_DONE;
    // add it to the imape_data_t so an imape_worker can deal with it
    imape_add_event_to_imape_data(ic->id->imape, ic->id, ev);
}

// try writing a single event
static derr_t try_write(imap_client_t *ic){
    derr_t e = E_OK;

    // do we have something to write?
    if(link_list_isempty(&ic->unwritten)){
        return e;
    }

    link_t *link = ic->unwritten.next;
    ic_cmd_t *ic_cmd = CONTAINER_OF(link, ic_cmd_t, link);

    // do we need to request a write event?
    if(ic->write_ev == NULL && !ic->write_requested){
        queue_cb_set(&ic->write_qcb, ic_prewait_cb, ic_new_data_cb);
        link = queue_pop_first_cb(&ic->id->imape->write_events,
                &ic->write_qcb);
        // do we need to set the session?
        if(link != NULL){
            ic->write_ev = CONTAINER_OF(link, event_t, link);
            ic->write_ev->session = ic->id->session;
            ic->id->ref_up(ic->id->session, IMAPE_REF_WRITE);
        }
    }

    // do we have a write event?
    if(ic->write_ev == NULL){
        return e;
    }

    // actually do the write
    size_t want;
    ic->write_ev->buffer.len = 0;
    PROP(&e, imap_cmd_write(ic_cmd->cmd, &ic->write_ev->buffer,
                &ic->write_skip, &want) );
    // did we finish writing that command?
    if(want == 0){
        ic->write_skip = 0;
        // move it to the unresponded list
        link_remove(&ic_cmd->link);
        link_list_append(&ic->unresponded, &ic_cmd->link);
    }
    // send the write event
    ic->write_ev->ev_type = EV_WRITE;
    {
        LOG_ERROR("send: %x", FD(&ic->write_ev->buffer));
    }
    ic->id->imape->upstream->pass_event(ic->id->imape->upstream, ic->write_ev);
    // done with event
    ic->write_ev = NULL;

    return e;
}

// try reading a single event
static derr_t try_read(imap_client_t *ic){
    derr_t e = E_OK;

    link_t *link = link_list_pop_first(&ic->unread);
    if(link == NULL){
        return e;
    }

    event_t *ev = CONTAINER_OF(link, event_t, link);

    // for now, the only thing that goes into ic->unread is READ events
    if(ev->ev_type != EV_READ){
        LOG_ERROR("Invalid event type (%x) in imap_client", FU(ev->ev_type));
        ORIG_GO(&e, E_INTERNAL, "Invalid event type in imap_client",
                return_read);
    }

    // check for EOF
    if(ev->buffer.len == 0){
        ORIG_GO(&e, E_CONN, "Received unexpected EOF", return_read);
    }

    LOG_INFO("recv: %x", FD(&ev->buffer));
    PROP_GO(&e, imap_read(&ic->reader, &ev->buffer), return_read);

return_read:
    ev->ev_type = EV_READ_DONE;
    ic->id->imape->upstream->pass_event(ic->id->imape->upstream, ev);

    return e;
}

static derr_t process_one_unhandled(imap_client_t *ic, ic_resp_t *ic_resp){
    derr_t e = E_OK;

    imap_resp_arg_t stolen;

    switch(ic_resp->resp->type){
        case IMAP_RESP_STATUS_TYPE:
            PROP_GO(&e, status_type(ic, ic_resp->resp->arg.status_type), cu);
            break;
        case IMAP_RESP_CAPA:
            PROP_GO(&e, check_capas(ic_resp->resp->arg.capa), cu);
            break;
        case IMAP_RESP_LIST:
            stolen.list = ic_resp->resp->arg.list;
            ic_resp->resp->arg.list = NULL;
            // list_resp() is responsible for passing or freeing the resp
            PROP_GO(&e, list_resp(ic, stolen.list), cu);
            break;
        case IMAP_RESP_LSUB:
            ORIG_GO(&e, E_VALUE, "got LSUB response\n", cu);
            break;
        case IMAP_RESP_STATUS:
            ORIG_GO(&e, E_VALUE, "got STATUS response\n", cu);
            break;
        case IMAP_RESP_FLAGS:
            if(ic->imap_state != SELECTING){
                ORIG_GO(&e, E_VALUE, "got FLAGS outside of SELECTING\n", cu);
            }
            PROP_GO(&e, select_flags(ic, ic_resp->resp->arg.flags), cu);
            break;
        case IMAP_RESP_SEARCH:
            PROP_GO(&e, search_resp(ic, ic_resp->resp->arg.search), cu);
            break;
        case IMAP_RESP_EXISTS:
            if(ic->imap_state != SELECTING){
                ORIG_GO(&e, E_VALUE, "got EXISTS outside of SELECTING\n", cu);
            }
            PROP_GO(&e, select_exists(ic, ic_resp->resp->arg.exists), cu);
            break;
        case IMAP_RESP_EXPUNGE:
            ORIG_GO(&e, E_VALUE, "got EXPUNGE response\n", cu);
            break;
        case IMAP_RESP_RECENT:
            if(ic->imap_state != SELECTING){
                ORIG_GO(&e, E_VALUE, "got RECENT outside of SELECTING\n", cu);
            }
            PROP_GO(&e, select_recent(ic, ic_resp->resp->arg.recent), cu);
            break;
        case IMAP_RESP_FETCH:
            if(ic->imap_state != FETCHING){
                ORIG_GO(&e, E_VALUE, "got FETCH outside of FETCHIN\n", cu);
            }
            PROP_GO(&e, fetch_resp(ic, ic_resp->resp->arg.fetch), cu);
            break;
        default:
            TRACE(&e, "got response of unknown type %x\n",
                    FU(ic_resp->resp->type));
            ORIG_GO(&e, E_INTERNAL, "unprintable response: unknown type", cu);
    }
cu:
    ic_resp_free(ic_resp);
    return e;
}

static derr_t process_all_unhandled(imap_client_t *ic){
    derr_t e = E_OK;

    while(!link_list_isempty(&ic->unhandled)){
        link_t *link = link_list_pop_first(&ic->unhandled);
        ic_resp_t *ic_resp = CONTAINER_OF(link, ic_resp_t, link);

        PROP(&e, process_one_unhandled(ic, ic_resp) );
    }

    return e;
}

static derr_t ic_set_goal(imap_client_t *ic, event_t *ev){
    derr_t e = E_OK;
    cmd_event_t *cmd_ev = CONTAINER_OF(ev, cmd_event_t, ev);

    if(ic->goal != GOAL_NONE){
        TRACE(&e, "unable to accept %x command while in goal state %x\n",
                FD(imap_client_command_type_to_dstr(cmd_ev->cmd_type)),
                FD(goal_to_dstr(ic->goal)));
        ORIG_GO(&e, E_INTERNAL, "invalid command issued by controller", fail);
    }

    // TODO: allow for changing the goal at arbitrary times
    if(ic->imap_state != AUTHENTICATED){
        TRACE(&e, "unable to accept %x command while in state %x\n",
                FD(imap_client_command_type_to_dstr(cmd_ev->cmd_type)),
                FD(imap_client_state_to_dstr(ic->imap_state)));
        ORIG_GO(&e, E_INTERNAL, "invalid command issued by controller", fail);
    }

    switch(cmd_ev->cmd_type){
        case IMAP_CLIENT_CMD_LIST_FOLDERS:
            ic->goal = GOAL_LIST;
            PROP_GO(&e, send_list(ic), fail);
            break;
        case IMAP_CLIENT_CMD_SET_FOLDER:
            PFMT("selecting %x\n", FD(&ev->buffer));
            ic->goal = GOAL_SYNC;
            PROP_GO(&e, dstr_copy(&ev->buffer, &ic->folder), fail);
            PROP_GO(&e, send_select(ic), fail);
            break;
        case IMAP_CLIENT_CMD_CLOSE:
            ORIG_GO(&e, E_VALUE, "can't handle that command yet!", fail);
        default:
            ORIG_GO(&e, E_VALUE, "invalid goal from imap_controller_up", fail);
    }

fail:
    ev->returner(ev);

    return e;
}

// imap_logic_t implementation

static derr_t new_event(imap_logic_t *logic, event_t *ev){
    derr_t e = E_OK;
    imap_client_t *ic = CONTAINER_OF(logic, imap_client_t, logic);

    switch(ev->ev_type){
        case EV_READ:
            // a READ came in for the session
            link_list_append(&ic->unread, &ev->link);
            break;
        case EV_COMMAND:
            // a command came in for the session from the imap_controller_t
            PROP(&e, ic_set_goal(ic, ev) );
            break;
        case EV_MAILDIR:
            // a maildir event command came in for the session
            LOG_ERROR("aaaaaaaahh wtf do i dooo??\n");
            break;
        case EV_WRITE_DONE:
            /* a write buffer has become available.  ev->session will already
               be set. */
            if(ic->write_ev != NULL){
                LOG_ERROR("imap_client received a write buffer, but it "
                        "already has one!\n");
                // only keep one write buffer
                ic->write_ev->ev_type = EV_WRITE_DONE;
                ic->id->imape->engine.pass_event(&ic->id->imape->engine, ev);
            }
            // this is the write event we requested
            ic->write_requested = false;
            ic->write_ev = ev;
            break;
        default:
            LOG_ERROR("unexpected event type in imap client, ev = %x\n",
                      FP(ev));
    }

    return e;
}

static derr_t do_work(imap_logic_t *logic){
    derr_t e = E_OK;

    imap_client_t *ic = CONTAINER_OF(logic, imap_client_t, logic);

    PROP_GO(&e, try_read(ic), cu_write_ev);

    PROP_GO(&e, process_all_unhandled(ic), cu_write_ev);

    PROP_GO(&e, try_write(ic), cu_write_ev);

cu_write_ev:
    // Always return an unused write event
    if(ic->write_ev != NULL){
        ic->write_ev->ev_type = EV_WRITE_DONE;
        ic->id->imape->engine.pass_event(&ic->id->imape->engine, ic->write_ev);
    }
    return e;
}

static bool more_work(imap_logic_t *logic){
    imap_client_t *ic = CONTAINER_OF(logic, imap_client_t, logic);

    return !link_list_isempty(&ic->unread)
        || !link_list_isempty(&ic->unhandled)
        || !link_list_isempty(&ic->unwritten);
}

// imap_client create/destroy functions

static void imap_client_free(imap_logic_t *logic){
    imap_client_t *ic = CONTAINER_OF(logic, imap_client_t, logic);

    // empty unread
    while(!link_list_isempty(&ic->unread)){
        link_t *link = link_list_pop_first(&ic->unread);
        event_t *ev = CONTAINER_OF(link, event_t, link);
        ev->returner(ev);
    }

    // empty unhandled
    while(!link_list_isempty(&ic->unhandled)){
        link_t *link = link_list_pop_first(&ic->unhandled);
        ic_resp_t *ic_resp = CONTAINER_OF(link, ic_resp_t, link);
        ic_resp_free(ic_resp);
    }

    // empty unwritten
    while(!link_list_isempty(&ic->unwritten)){
        link_t *link = link_list_pop_first(&ic->unwritten);
        ic_cmd_t *ic_cmd = CONTAINER_OF(link, ic_cmd_t, link);
        ic_cmd_free(ic_cmd);
    }

    // empty unresponded
    while(!link_list_isempty(&ic->unresponded)){
        link_t *link = link_list_pop_first(&ic->unresponded);
        ic_cmd_t *ic_cmd = CONTAINER_OF(link, ic_cmd_t, link);
        ic_cmd_free(ic_cmd);
    }

    // return write_ev
    if(ic->write_ev != NULL){
        ic->write_ev->returner(ic->write_ev);
        ic->write_ev = NULL;
    }

    // empty folders, if we have to close mid-list-response
    jsw_anode_t *node = jsw_apop(&ic->folders);
    for(; node != NULL; node = jsw_apop(&ic->folders)){
        ie_list_resp_t *list = CONTAINER_OF(node, ie_list_resp_t, node);
        ie_list_resp_free(list);
    }

    // free the folder_sync substate
    folder_sync_free(&ic->sync);

    imap_reader_free(&ic->reader);

    dstr_free(&ic->folder);

    free(ic);
}

derr_t imap_client_logic_alloc(imap_logic_t **out, void *arg_void,
        imape_data_t *id){
    derr_t e = E_OK;
    imap_client_t *ic = malloc(sizeof(*ic));
    if(ic == NULL){
        *out = NULL;
        ORIG(&e, E_NOMEM, "no memory for malloc");
    }
    *ic = (imap_client_t){0};

    PROP_GO(&e, dstr_new(&ic->folder, 256), fail_malloc);

    imap_parser_cb_t parser_cb = {.resp=resp_cb};
    PROP_GO(&e, imap_reader_init(&ic->reader, parser_cb, ic), fail_folder);

    ic->id = id;
    ic->logic.new_event = new_event;
    ic->logic.do_work = do_work;
    ic->logic.more_work = more_work;
    ic->logic.free = imap_client_free;

    imap_client_alloc_arg_t *arg = arg_void;
    ic->spec = arg->spec;
    ic->controller = arg->controller;

    ic->imap_state = PREGREET;
    link_init(&ic->unread);
    link_init(&ic->unhandled);
    link_init(&ic->unwritten);
    link_init(&ic->unresponded);
    ic->next_tag = 1;

    // state machine variables
    ic->goal = GOAL_LOGIN;
    ic->saw_capas = false;

    jsw_ainit(&ic->folders, ie_list_resp_cmp, ie_list_resp_get);

    *out = &ic->logic;

    return e;

fail_folder:
    dstr_free(&ic->folder);
fail_malloc:
    free(ic);
    *out = NULL;
    return e;
}

DSTR_STATIC(unknown_dstr, "unknown");

DSTR_STATIC(GOAL_NONE_dstr, "NONE");
DSTR_STATIC(GOAL_LOGIN_dstr, "LOGIN");
DSTR_STATIC(GOAL_LIST_dstr, "LIST");
DSTR_STATIC(GOAL_SYNC_dstr, "SYNC");
static const dstr_t *goal_to_dstr(ic_goal_t goal){
    switch(goal){
        case GOAL_NONE: return &GOAL_NONE_dstr;
        case GOAL_LOGIN: return &GOAL_LOGIN_dstr;
        case GOAL_LIST: return &GOAL_LIST_dstr;
        case GOAL_SYNC: return &GOAL_SYNC_dstr;
    }
    return &unknown_dstr;
}

DSTR_STATIC(PREGREET_dstr, "PREGREET");
DSTR_STATIC(PRECAPA_dstr, "PRECAPA");
DSTR_STATIC(PREAUTH_dstr, "PREAUTH");
DSTR_STATIC(AUTHENTICATED_dstr, "AUTHENTICATED");
DSTR_STATIC(PRECLOSE_dstr, "PRECLOSE");
DSTR_STATIC(LISTING_dstr, "LISTING");
DSTR_STATIC(SELECTING_dstr, "SELECTING");
DSTR_STATIC(SEARCHING_dstr, "SEARCHING");
DSTR_STATIC(FETCHING_dstr, "FETCHING");
DSTR_STATIC(CLOSING_dstr, "CLOSING");
DSTR_STATIC(SELECTED_dstr, "SELECTED");
static const dstr_t *imap_client_state_to_dstr(imap_client_state_t state){
    switch(state){
        case PREGREET: return &PREGREET_dstr;
        case PRECAPA: return &PRECAPA_dstr;
        case PREAUTH: return &PREAUTH_dstr;
        case AUTHENTICATED: return &AUTHENTICATED_dstr;
        case PRECLOSE: return &PRECLOSE_dstr;
        case LISTING: return &LISTING_dstr;
        case SELECTING: return &SELECTING_dstr;
        case SEARCHING: return &SEARCHING_dstr;
        case FETCHING: return &FETCHING_dstr;
        case CLOSING: return &CLOSING_dstr;
        case SELECTED: return &SELECTED_dstr;
    }
    return &unknown_dstr;
}

DSTR_STATIC(CMD_LIST_FOLDERS_dstr, "LIST_FOLDERS");
DSTR_STATIC(CMD_SET_FOLDER_dstr, "SET_FOLDER");
DSTR_STATIC(CMD_CLOSE_dstr, "CLOSE");
const dstr_t *imap_client_command_type_to_dstr(imap_client_command_type_t t){
    switch(t){
        case IMAP_CLIENT_CMD_LIST_FOLDERS: return &CMD_LIST_FOLDERS_dstr;
        case IMAP_CLIENT_CMD_SET_FOLDER: return &CMD_SET_FOLDER_dstr;
        case IMAP_CLIENT_CMD_CLOSE: return &CMD_CLOSE_dstr;
    }
    return &unknown_dstr;
}
