#include "libcli/libcli.h"

DEF_CONTAINER_OF(configure_t, advancer, advancer_t)

static const char *tos_explanation =
"As the splintermail service is currently configured, a valid TLS\n"
"certificate is required for the service to run.  In order to obtain a valid\n"
"TLS certificate, splintermail will create an account for you with Let's\n"
"Encrypt (https://letsencrypt.org), a non-profit service for creating free\n"
"TLS certificates.\n"
"\n"
"To proceed as currently configured, you must read and agree to the Let's\n"
"Encrypt Terms of Service.\n"
"\n"
"(Note: creating a Let's Encrypt account is not required; you may instead\n"
"configure splintermail to use TLS certificates which you generate on your\n"
"own, or configure splintermail (and your email client) to not use TLS,\n"
"though that is not recommended unless you know what you're doing.)\n"
"\n"
"Read the Let's Encrypt Terms of Service: %x\n"
"\n"
;

DSTR_STATIC(
    tos_prompt, "Do you agree to the Let's Encrypt Terms of Service? [y/n]:"
);

static const char *inst_explanation =
"The next step is to register a new installation for your splintermail\n"
"account.\n"
"\n"
;

DSTR_STATIC(username_prompt, "Enter your splintermail email address:");
DSTR_STATIC(password_prompt, "Enter your splintermail password:");


void configure_status_client_update(configure_t *c, citm_status_t status){
    citm_status_free(&c->status);
    c->status = status;
    c->status_dirty = true;
    advancer_schedule(&c->advancer, E_OK);
}

void configure_status_client_done(configure_t *c, derr_t e){
    // ignore status_client errors after we decide to close
    if(c->advancer.up_done){
        DROP_VAR(&e);
    }

    advancer_schedule(&c->advancer, e);
}

void configure_add_inst_done(configure_t *c, derr_t e){
    if(is_error(e)) goto done;

    // read the installation
    jspec_t *jspec = JAPI(JINST(&c->inst));
    PROP_GO(&e, jspec_read(jspec, c->json.root), done);

    c->inst_done = true;

done:
    dstr_zeroize(&c->json_text);
    json_free(&c->json);
    advancer_schedule(&c->advancer, e);
}

static string_builder_t acme_path(configure_t *c){
    return sb_append(&c->smdir, SBS("acme"));
}

static string_builder_t inst_path(configure_t *c){
    return sb_append(&c->smdir, SBS("acme/installation.json"));
}

void configure_get_acme_dir_done(configure_t *c, derr_t e){
    if(is_error(e)) goto cu;

    jspec_t *jspec = JOBJ(true,
        JKEY("meta", JOBJ(true,
            JKEY("termsOfService", JDCPY(&c->tos_url)),
        )),
    );

    PROP_GO(&e, json_parse(c->resp, &c->json), cu);
    PROP_GO(&e, jspec_read(jspec, c->json.root), cu);

    c->tos_done = true;

cu:
    json_free(&c->json);
    advancer_schedule(&c->advancer, e);
}

/*
   splintermail configure:
    - make sure splintermail directory exists
    - make sure we have read/write permissions to splintermail directory
    - if not configured:
        - configure things
    - wait for configuration to take

   splintermail configure --force
    - mkdir -p splintermail/acme directory
    - configure things
    - refuse to --wait
*/
static derr_t configure_advance_up(advancer_t *advancer){
    derr_t e = E_OK;
    configure_t *c = CONTAINER_OF(advancer, configure_t, advancer);
    configure_i *ci = c->ci;
    bool ok;

    ONCE(c->init){
        // configure a json_t that we can zeroize secrets from
        DSTR_WRAP_ARRAY(c->json_text, c->_json_text);
        size_t nnodes = sizeof(c->json_nodes)/sizeof(*c->json_nodes);
        json_prep_preallocated(
            &c->json, &c->json_text, c->json_nodes, nnodes, true
        );

        // by default: check for proper file access
        // with --force: just make the directories needed
        if(c->force){
            // make the sm_dir if it doesn't exist
            PROP(&e, ci->mkdirs(ci, acme_path(c)) );
        }else{
            // is there a smdir that we can read and write to?
            PROP(&e, ci->dir_rw_access(ci, c->smdir, &ok) );
            if(!ok){
                ci->print(ci,
                    "--splintermail-dir either does not exist or we don't "
                    "have read/write access to it: %x\n",
                    FSB(c->smdir)
                );
                *c->retval = 19;
                advancer_up_done(&c->advancer);
                return e;
            }
        }

        // only the non --force codepath wants a status_client
        bool want_sc = !c->force;
        PROP(&e, ci->io_init(ci, want_sc) );
    }

    // default only: check our current status
    if(!c->force && !c->sc_check){
        // wait for first update
        if(!c->status_dirty) return e;
        c->status_dirty = false;
        if(c->status.configured == TRI_NA){
            // don't need configuring!
            ci->print(ci,
                "splintermail does not need an ACME configuration; it either "
                "is configured to not use TLS or it is configured with "
                "explicit certificate and key, so no ACME is necessary\n",
                NULL
            );
            *c->retval = 20;
            advancer_up_done(&c->advancer);
            return e;
        }
        if(c->status.configured == TRI_YES){
            if(c->status.tls_ready == TRI_YES){
                ci->print(ci, "server already ready.\n", NULL);
                *c->retval = 0;
                advancer_up_done(&c->advancer);
                return e;
            }
            // configured, but not ready yet
            ci->print(ci, "server already configured.\n", NULL);
            c->configure_done = true;
        }
        c->sc_check = true;
    }

    // default and --force: fetch the tos url and get user agreement
    if(!c->configure_done){
        // fetch the terms of service URL from the acme directory
        ONCE(c->tos_started){
            ci->get_acme_dir(ci, &c->resp);
        }
        if(!c->tos_done) return e;

        // explain the tos, get user to agree, and get user/pass
        ONCE(c->agreement){
            // get tos agreement
            ci->print(ci, tos_explanation, FD(c->tos_url));
            size_t ret = 99;
            PROP(&e, ci->prompt_one_of(ci, tos_prompt, "yn", &ret) );
            if(ret != 0){
                // "no"
                ci->print(ci, "configuration canceled\n", NULL);
                *c->retval = 22;
                advancer_up_done(&c->advancer);
                return e;
            }
            // get username and password
            ci->print(ci, inst_explanation, NULL);
            if(c->user.len == 0){
                // user may have been provided on command line
                PROP(&e,
                    ci->user_prompt(ci, username_prompt, &c->user, false)
                );
            }
            PROP(&e, ci->user_prompt(ci, password_prompt, &c->pass, true) );
        }

        // create the installation
        ONCE(c->inst_started){
            ci->print(ci, "registering installation...\n", NULL);
            ci->add_inst(ci, c->user, c->pass, &c->json);
        }
        if(!c->inst_done) return e;

        // write the installation file
        ci->print(ci, "saving installation file...\n", NULL);
        PROP(&e, ci->write_inst(ci, c->inst, inst_path(c)) );

        // kick the server
        if(!c->force){
            ci->status_client_check(ci);
        }

        c->configure_done = true;
    }

    // wait for the installation to take
    if(!c->force){
        ONCE(c->wait_msg_sent){
            ci->print(ci, "waiting for server to become ready...\n", NULL);
        }
        // have we seen an update?
        if(!c->status_dirty) return e;
        c->status_dirty = false;
        // hae we reached the ready state yet?
        if(c->status.tls_ready != TRI_YES){
            DSTR_VAR(buf, 256);
            FMT_QUIET(&buf, "splintermail is %x", FD(c->status.status_maj));
            if(c->status.status_min.len){
                FMT_QUIET(&buf, ": %x", FD(c->status.status_min));
            }
            ci->print(ci, "%x\n", FD(buf));
            return e;
        }
        // success!
        ci->print(ci, "server configured.\n", NULL);
    }

    *c->retval = 0;
    advancer_up_done(&c->advancer);

    return e;
}

static void configure_advance_down(advancer_t *advancer, derr_t *e){
    configure_t *c = CONTAINER_OF(advancer, configure_t, advancer);
    configure_i *ci = c->ci;

    // close our resources
    if(ci->io_close(ci)) return;

    advancer_down_done(&c->advancer);

    citm_status_free(&c->status);
    dstr_free(&c->user);
    dstr_free0(&c->pass);
    dstr_free0(&c->resp);
    dstr_zeroize(&c->json_text);
    json_free(&c->json);
    installation_free0(&c->inst);
    dstr_free(&c->tos_url);

    // leave e for the duv_root_t
    (void)e;

    return;
}

derr_t configure_init(
    configure_t *c,
    configure_i *ci,
    scheduler_i *scheduler,
    const string_builder_t smdir,
    const dstr_t user,
    bool force,
    int *retval
){
    derr_t e = E_OK;

    *c = (configure_t){
        .ci = ci,
        .smdir = smdir,
        .force = force,
        .retval = retval,
    };
    advancer_prep(
        &c->advancer, scheduler, configure_advance_up, configure_advance_down
    );

    // user was maybe been provided, maybe not
    if(user.len) PROP(&e, dstr_copy2(user, &c->user) );

    return e;
}
