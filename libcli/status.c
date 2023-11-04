#include "libcli/libcli.h"

DEF_CONTAINER_OF(status_t, advancer, advancer_t)

static void sc_update_cb(void *data, citm_status_t status){
    status_t *s = data;
    citm_status_free(&s->status);
    s->status = status;
    s->have_status = true;
    advancer_schedule(&s->advancer, E_OK);
}

static void sc_done_cb(void *data, derr_t err){
    status_t *s = data;

    // ignore status_client errors after we finished
    if(s->advancer.up_done){
        DROP_VAR(&err);
    }

    advancer_schedule(&s->advancer, err);
}

derr_t status_advance_up(advancer_t *advancer){
    status_t *s = CONTAINER_OF(advancer, status_t, advancer);
    derr_t e = E_OK;

    ONCE(s->init){
        PROP(&e,
            s->sc_init(
                &s->sc,
                &s->root->loop,
                &s->root->scheduler.iface,
                s->status_sock,
                sc_update_cb,
                sc_done_cb,
                s // cb_data
            )
        );
    }

    // wait for the an update_cb
    if(!s->have_status) return e;
    s->have_status = false;

    DSTR_VAR(buf, 4096);

    // print version just once
    ONCE(s->version){
        PROP(&e,
            FMT(
                &buf,
                "splintermail server version: %x.%x.%x\n",
                FI(s->status.version_maj),
                FI(s->status.version_min),
                FI(s->status.version_patch)
            )
        );
    }

    if(s->status.fulldomain.len){
        // print domain just once
        ONCE(s->subdomain){
            PROP(&e, FMT(&buf, "subdomain: %x\n", FD(s->status.fulldomain)) );
        }
    }

    // print maj and min status every time
    PROP(&e, FMT(&buf, "status: %x", FD(s->status.status_maj)) );
    if(s->status.status_min.len){
        PROP(&e, FMT(&buf, ": %x", FD(s->status.status_min)) );
    }
    PROP(&e, FMT(&buf, "\n") );

    if(s->follow) PROP(&e, FMT(&buf, "---\n") );

    PROP(&e, s->print(buf) );

    if(s->follow) return e;

    advancer_up_done(&s->advancer);

    return e;
}

void status_advance_down(advancer_t *advancer, derr_t *e){
    status_t *s = CONTAINER_OF(advancer, status_t, advancer);

    if(s->sc_close(&s->sc)) return;

    advancer_down_done(&s->advancer);

    citm_status_free(&s->status);

    // leave advancer.e alone, let the duv_root_t harvest it
    (void)e;

    return;
}

static derr_t real_print(dstr_t buf){
    return FFMT(stdout, "%x", FD(buf));
}

derr_t status_main(const string_builder_t status_sock, bool follow){
    derr_t e = E_OK;

    duv_root_t root;
    status_t status = {
        .status_sock = status_sock,
        .follow = follow,
        .root = &root,
        .sc_init = status_client_init,
        .sc_close = status_client_close,
        .print = real_print,
    };
    advancer_prep(
        &status.advancer,
        &root.scheduler.iface,
        status_advance_up,
        status_advance_down
    );

    PROP(&e, duv_root_run(&root, &status.advancer ) );

    return e;
}
