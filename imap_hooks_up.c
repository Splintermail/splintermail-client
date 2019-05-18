
/* Some imap hooks must be called in a certain order (otherwise it's a bug).
   Note that errors here indicate errors in the callbacks of the parser, not
   errors in the parsed text.  That is, after fetch_start, no matter what the
   input text, there should always be a fetch_end before any non-fetch hooks */
typedef enum {
    HOOK_DEFAULT,
    HOOK_CAPABILITY,
    HOOK_FETCH,
    HOOK_FETCH_RFC822,
    HOOK_APPEND,
} hook_state_t;

const char *hook_state_to_str(hook_state_t){
    switch(state){
        case HOOK_DEFAULT: return "HOOK_DEFAULT";
        case HOOK_CAPABILITY: return "HOOK_CAPABILITY";
        case HOOK_FETCH: return "HOOK_FETCH";
        case HOOK_FETCH_RFC822: return "HOOK_FETCH_RFC822";
        case HOOK_APPEND: return "HOOK_APPEND";
        default: return "unknown";
    }
}

#define ASSERT_HOOK_STATE(_cond) \
    if(!(id->hook_state _cond)){ \
        TRACE(e, "bad imap hook, got %x while in hook state \"%x\"", \
                FD(st_type_to_dstr(status)), \
                FS(hook_state_to_str(id->hook_state))); \
        ORIG(e, E_INTERNAL, "bad imap hook"); \
    }

#define ASSERT_HOOK_STATE_GO(_cond, label) \
    if(!(id->hook_state _cond)){ \
        TRACE(e, "bad imap hook, got %x while in hook state \"%x\"", \
                FD(st_type_to_dstr(status)), \
                FS(hook_state_to_str(id->hook_state))); \
        ORIG_GO(e, E_INTERNAL, "bad imap hook", label); \
    }

typedef enum {
    /* wait for the server to give hello */
    CLIENT_PREHELLO,
} imap_state_t;

const char *imap_state_to_str(imap_state_t state){
    switch(state){
        case CLIENT_PREHELLO: return "CLIENT_PREHELLO";
        default: return "unknown";
    }
}

// CLIENT_PREHELLO //

static void client_prehello_untagged_ok(imape_data_t *id, dstr_t tag,
        status_type_t status, status_code_t code, unsigned int code_extra,
        dstr_t text){
}

// CLIENT_CAPABILITY //

static derr_t client_capability_capa_start(imape_data_t *id){
    // We don't require any capbilities yet.

    // but it would look something like this:
    // id->server_capabilities = (server_capabilities_t){0}

    return E_OK;
}

static derr_t client_capability_capa(imape_data_t *id, dstr_t capabilitiy){
    // This is where we raise errors on capabilities we can't work with
    dstr_free(&capability);
    return E_OK;
}

static void client_capability_end(imape_data_t *id){
    // This is where we raise errors for capabilities that are missing
}

/////////////////////////////////////////


/* General hooks.  These are called directly by the parser.  Invalid states are
   detected and state-specific hooks are called (this separation lets us have
   all the logic for a single state together). */

// a tagged status-type response means the end of the message.
static void up_tagged_status_type(imape_data_t *id, dstr_t tag,
        status_type_t status status_code_t code, unsigned int code_extra,
        dstr_t text){
}

static void up_untagged_ok(imape_data_t *id, dstr_t tag, status_code_t code,
        unsigned int code_extra, dstr_t text){
    switch(id->imap_state){
        case CLIENT_PREHELLO:
            client_prehello_untagged_ok(
                    id, tag, status, code, code_extra, text);
            break;
        // states like CLIENT_PREHELLO don't allow tagged responses
        default:
            TRACE(e, "invalid untagged OK response while "
                    "client is in state %x with text \"%x\"\n",
                    FD(st_type_to_dstr(state)),
                    FS(imap_state_to_str(id->state)),
                    FD(&text));
            ORIG_GO(e, E_VALUE, "bad imap response", fail);
    }
    break;

}

static void up_tagged_no(imape_data_t *id, dstr_t tag, status_code_t code,
        unsigned int code_extra, dstr_t text){
}

static void up_tagged_bad(imape_data_t *id, dstr_t tag, status_code_t code,
        unsigned int code_extra, dstr_t text){
}

static void up_untagged_ok(imape_data_t *id, dstr_t tag, status_code_t code,
        unsigned int code_extra, dstr_t text){
    derr_t e = E_OK;
    return;

fail:
    dstr_free(&text);
    // TODO ERROR
}

static void up_untagged_no(imape_data_t *id, status_code_t code,
        unsigned int code_extra, dstr_t text){
    derr_t e = E_OK;
    switch(id->imap_state){
        default:
            TRACE(e, "invalid untagged NO response while "
                    "client is in state %x with text \"%x\"\n",
                    FD(st_type_to_dstr(state)),
                    FS(imap_state_to_str(id->state)),
                    FD(&text));
            ORIG_GO(e, E_VALUE, "bad imap response", fail);
    }
    return;

fail:
    dstr_free(&text);
    // TODO ERROR
}

static void up_untagged_bad(imape_data_t *id, status_code_t code,
        unsigned int code_extra, dstr_t text){
    derr_t e = E_OK;
    switch(id->imap_state){
        case CLIENT_PREHELLO:
            client_prehello_untagged_bad(
                    id, tag, status, code, code_extra, text);
            break;
        // TODO: does "invalid BAD response even make sense??"
        default:
            TRACE(e, "invalid untagged BAD response while "
                    "client is in state %x with text \"%x\"\n",
                    FD(st_type_to_dstr(state)),
                    FS(imap_state_to_str(id->state)),
                    FD(&text));
            ORIG_GO(e, E_VALUE, "bad imap response", fail);
    }
    return;

fail:
    dstr_free(&text);
    // TODO ERROR
}

static void up_untagged_preauth(imape_data_t *id, status_code_t code,
        unsigned int code_extra, dstr_t text){
    derr_t e = E_OK;
    switch(id->imap_state){
        case CLIENT_PREHELLO:
            client_prehello_untagged_preauth(
                    id, tag, status, code, code_extra, text);
            break;
        default:
            TRACE(e, "invalid untagged PREAUTH response while "
                    "client is in state %x with text \"%x\"\n",
                    FD(st_type_to_dstr(state)),
                    FS(imap_state_to_str(id->state)),
                    FD(&text));
            ORIG_GO(e, E_VALUE, "bad imap response", fail);
    }
    return;

fail:
    dstr_free(&text);
    // TODO ERROR
}

static void up_untagged_bye(imape_data_t *id, status_code_t code,
        unsigned int code_extra, dstr_t text){
    switch(id->imap_state){
        case CLIENT_PREHELLO:
            client_prehello_untagged_preauth(
                    id, tag, status, code, code_extra, text);
            break;
        default:
            // there is no such thing as an invalid BYE response
            LOG_ERROR("goodbye, server!\n");
            dstr_free(&text);
            // TODO close session
    }
    return;
}

// treat each status type separately
static void up_status_type(void *data, dstr_t tag, status_type_t status,
        status_code_t code, unsigned int code_extra, dstr_t text){
    derr_t e = E_OK;
    imape_data_t *id = data;
    // check hook state
    ASSERT_HOOK_STATE_GO(== HOOK_DEFAULT, fail);
    // Is the status-type response tagged?
    if(tag.data != NULL){
        // tagged type
        switch(status){
            // tagged status responses are not handled separately by type.
            case STATUS_TYPE_OK: // response completed successfully
            case STATUS_TYPE_NO: // response completed unsuccessfully
            case STATUS_TYPE_BAD: // command was invalid, try again
                up_tagged_status_type(id, tag, status, code, code_extra, text);
            // not all status types can be tagged
            default:
                TRACE(e, "invalid tagged \"%x\" response with tag \"%x\" "
                        "while client is in state %x\n",
                        FD(st_type_to_dstr(state)),
                        FD(&tag),
                        FS(imap_state_to_str(id->state)));
                ORIG_GO(e, E_VALUE, "bad imap response", fail);
        }
    }
    // untagged
    else{
        switch(status){
            case STATUS_TYPE_OK:
                // informational message
                up_untagged_ok(id, code, code_extra, text);
                break;
            case STATUS_TYPE_NO:
                // a warning about a command
                up_untagged_no(id, code, code_extra, text);
                break;
            case STATUS_TYPE_BAD:
                // an error not from a command, or not sure from which command
                up_untagged_bad(id, code, code_extra, text);
                break;
            case STATUS_TYPE_PREAUTH:
                // only allowed as a greeting
                up_untagged_preauth(id, code, code_extra, text);
                break;
            case STATUS_TYPE_BYE:
                // we are logging out or server is shutting down.
                up_untagged_bye(id, code, code_extra, text);
                break;
            default:
                TRACE(e, "invalid untagged \"%x\" response while "
                        "client is in state %x\n",
                        FD(st_type_to_dstr(state)),
                        FS(imap_state_to_str(id->state)));
                ORIG_GO(e, E_VALUE, "bad imap response", fail);
        }
    }
    return;

fail:
    dstr_free(&tag);
    dstr_free(&text);
    // TODO ERROR
}

static derr_t up_capa_start(void *data){
    imape_data_t *id = data;
    // check hook state
    ASSERT_HOOK_STATE(== HOOK_DEFAULT);
    switch(id->imap_state){
        case CLIENT_PREHELLO:
            client_prehello_untagged_preauth(
                    id, tag, status, code, code_extra, text);
            break;
        default:
            // there is no such thing as an invalid BYE response
            LOG_ERROR("goodbye, server!\n");
            dstr_free(&text);
            // TODO close session
    }
}
static derr_t up_capa(void *data, dstr_t capability){
    derr_t e = E_OK;
    imape_data_t *id = data;
    // check hook state
    ASSERT_HOOK_STATE(== HOOK_CAPABILITY);
    return e;
}
static void up_capa_end(void *data, bool success){
    derr_t e = E_OK;
    imape_data_t *id = data;
    // check hook state
    ASSERT_HOOK_STATE_GO(== HOOK_CAPABILITY, fail);
    return e;

fail:

}

////////////////////////////

imap_parse_hooks_up_t imap_hooks_up = {
    up_status_type,
    //
    up_capa_start,
    up_capa,
    up_capa_end,
    //
    void (*pflag)(void *data, ie_flag_list_t flags);
    void (*list)(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                 dstr_t mbx);
    void (*lsub)(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                 dstr_t mbx);
    void (*status)(void *data, bool inbox, dstr_t mbx,
                   bool found_messages, unsigned int messages,
                   bool found_recent, unsigned int recent,
                   bool found_uidnext, unsigned int uidnext,
                   bool found_uidvld, unsigned int uidvld,
                   bool found_unseen, unsigned int unseen);
    void (*flags)(void *data, ie_flag_list_t flags);
    void (*exists)(void *data, unsigned int num);
    void (*recent)(void *data, unsigned int num);
    void (*expunge)(void *data, unsigned int num);
    // for FETCH responses
    derr_t (*fetch_start)(void *data, unsigned int num);
    derr_t (*f_flags)(void *data, ie_flag_list_t flags);
    derr_t (*f_rfc822_start)(void *data);
    derr_t (*f_rfc822_literal)(void *data, const dstr_t *raw); // don't free raw
    derr_t (*f_rfc822_qstr)(void *data, const dstr_t *qstr); // don't free qstr
    void (*f_rfc822_end)(void *data, bool success);
    void (*f_uid)(void *data, unsigned int num);
    void (*f_intdate)(void *data, imap_time_t imap_time);
    void (*fetch_end)(void *data, bool success);
};

    void (*status_type)(void *data, dstr_t tag, status_type_t status,
                        status_code_t code, unsigned int code_extra,
                        dstr_t text);
    // for CAPABILITY responses (both normal responses and as status codes)
    derr_t (*capa_start)(void *data);
    derr_t (*capa)(void *data, dstr_t capability);
    void (*capa_end)(void *data, bool success);
    //
    void (*pflag)(void *data, ie_flag_list_t flags);
    void (*list)(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                 dstr_t mbx);
    void (*lsub)(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                 dstr_t mbx);
    void (*status)(void *data, bool inbox, dstr_t mbx,
                   bool found_messages, unsigned int messages,
                   bool found_recent, unsigned int recent,
                   bool found_uidnext, unsigned int uidnext,
                   bool found_uidvld, unsigned int uidvld,
                   bool found_unseen, unsigned int unseen);
    void (*flags)(void *data, ie_flag_list_t flags);
    void (*exists)(void *data, unsigned int num);
    void (*recent)(void *data, unsigned int num);
    void (*expunge)(void *data, unsigned int num);
    // for FETCH responses
    derr_t (*fetch_start)(void *data, unsigned int num);
    derr_t (*f_flags)(void *data, ie_flag_list_t flags);
    derr_t (*f_rfc822_start)(void *data);
    derr_t (*f_rfc822_literal)(void *data, const dstr_t *raw); // don't free raw
    derr_t (*f_rfc822_qstr)(void *data, const dstr_t *qstr); // don't free qstr
    void (*f_rfc822_end)(void *data, bool success);
    void (*f_uid)(void *data, unsigned int num);
    void (*f_intdate)(void *data, imap_time_t imap_time);
    void (*fetch_end)(void *data, bool success);

////////////////////////////////

static derr_t capa_start(void *data){
    imape_data_t *id = data;
    // make sure we are not expecting any particular hooks
    if(id->hook_state != HOOK_DEFAULT){
        TRACE(e, "bad imap hook, got CAPABILITY while in hook state \"%x\"",
                FS(hook_state_to_str(id->hook_state)));
        ORIG(E_INTERNAL, "bad imap hook");
    }
    id->hook_state = HOOK_CAPABILITY;

    return E_OK;
}

static derr_t capa(void *data, dstr_t capability){
    (void)data;
    LOG_ERROR("CAPABILITY: %x\n", FD(&capability));
    dstr_free(&capability);
    return E_OK;
}

static void capa_end(void *data, bool success){
    (void)data;
    LOG_ERROR("CAPABILITY END (%x)\n", FS(success ? "success" : "fail"));
}

//

static void pflag_resp(void *data, ie_flag_list_t flags){
    (void)data;
    LOG_ERROR("PERMANENTFLAGS (");
    print_flag_list(flags);
    LOG_ERROR(")\n");
    ie_flag_list_free(&flags);
}

//

static void list_resp(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                      dstr_t mbx){
    (void)data;
    LOG_ERROR("LIST (");
    print_mflag_list(mflags);
    LOG_ERROR(") '%x' '%x' (%x)\n", FC(sep), FD(&mbx), FU(inbox));
    ie_mflag_list_free(&mflags);
    dstr_free(&mbx);
}

//

static void lsub_resp(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                      dstr_t mbx){
    (void)data;
    LOG_ERROR("LSUB (");
    print_mflag_list(mflags);
    LOG_ERROR(") '%x' '%x' (%x)\n", FC(sep), FD(&mbx), FU(inbox));
    ie_mflag_list_free(&mflags);
    dstr_free(&mbx);
}

//

static void status_resp(void *data, bool inbox, dstr_t mbx,
                        bool found_messages, unsigned int messages,
                        bool found_recent, unsigned int recent,
                        bool found_uidnext, unsigned int uidnext,
                        bool found_uidvld, unsigned int uidvld,
                        bool found_unseen, unsigned int unseen){
    (void)data;
    LOG_ERROR("STATUS '%x' (%x)", FD(&mbx), FU(inbox));
    if(found_messages) LOG_ERROR(" messages: %x", FU(messages));
    if(found_recent) LOG_ERROR(" recent: %x", FU(recent));
    if(found_uidnext) LOG_ERROR(" uidnext: %x", FU(uidnext));
    if(found_uidvld) LOG_ERROR(" uidvld: %x", FU(uidvld));
    if(found_unseen) LOG_ERROR(" unseen: %x", FU(unseen));
    LOG_ERROR("\n");
    dstr_free(&mbx);
}

//

static void flags_resp(void *data, ie_flag_list_t flags){
    (void)data;
    LOG_ERROR("FLAGS (");
    print_flag_list(flags);
    LOG_ERROR(")\n");
    ie_flag_list_free(&flags);
}

//

static void exists_hook(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("EXISTS %x\n", FU(num));
}

//

static void recent_hook(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("RECENT %x\n", FU(num));
}

//

static void expunge_hook(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("EXPUNGE %x\n", FU(num));
}

//

static derr_t fetch_start(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("FETCH START (%x)\n", FU(num));
    return E_OK;
}

static derr_t f_flags(void *data, ie_flag_list_t flags){
    (void)data;
    LOG_ERROR("FETCH FLAGS (");
    print_flag_list(flags);
    LOG_ERROR(")\n");
    ie_flag_list_free(&flags);
    return E_OK;
}

static derr_t f_rfc822_start(void *data){
    (void)data;
    LOG_ERROR("FETCH RFC822 START\n");
    return E_OK;
}

static derr_t f_rfc822_qstr(void *data, const dstr_t *qstr){
    (void)data;
    LOG_ERROR("FETCH QSTR '%x'\n", FD(qstr));
    return E_OK;
}

static void f_rfc822_end(void *data, bool success){
    (void)data;
    LOG_ERROR("FETCH RFC822 END (%x)\n", FS(success ? "success" : "fail"));
}

static void f_uid(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("FETCH UID %x\n", FU(num));
}

static void f_intdate(void *data, imap_time_t imap_time){
    (void)data;
    LOG_ERROR("FETCH INTERNALDATE %x-%x-%x\n",
              FI(imap_time.year), FI(imap_time.month), FI(imap_time.day));
}

static void fetch_end(void *data, bool success){
    (void)data;
    LOG_ERROR("FETCH END (%x)\n", FS(success ? "success" : "fail"));
}

extern imap_parse_hooks_up_t imape_hooks_up{
}
