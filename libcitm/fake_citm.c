#include "libcitm/libcitm.h"
#include "libduv/fake_stream.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

DEF_CONTAINER_OF(fake_citm_conn_t, iface, citm_conn_t)
DEF_CONTAINER_OF(fake_citm_connect_t, iface, citm_connect_i)
DEF_CONTAINER_OF(fake_citm_connect_t, link, link_t)
DEF_CONTAINER_OF(fake_citm_io_t, iface, citm_io_i)
DEF_CONTAINER_OF(fake_keydir_t, iface, keydir_i)

static void fc_free(citm_conn_t *iface){
    fake_citm_conn_t *f = CONTAINER_OF(iface, fake_citm_conn_t, iface);
    if(!f->iface.stream->awaited){
        LOG_FATAL("conn closed without awaiting stream\n");
    }
    f->is_freed = true;
}

citm_conn_t *fake_citm_conn(
    fake_citm_conn_t *f,
    stream_i *stream,
    imap_security_e security,
    SSL_CTX *ctx,
    dstr_t verify_name
){
    *f = (fake_citm_conn_t){
        .iface = {
            .stream = stream,
            .security = security,
            .ctx = ctx,
            .free = fc_free,
            .verify_name = verify_name
        },
    };
    return &f->iface;
}

citm_conn_t *fake_citm_conn_insec(fake_citm_conn_t *f, stream_i *stream){
    return fake_citm_conn(f, stream, IMAP_SEC_INSECURE, NULL, DSTR_LIT(""));
}

derr_t fake_citm_conn_cleanup(
    manual_scheduler_t *m, fake_citm_conn_t *f, fake_stream_t *s
){
    derr_t e = E_OK;
    PROP(&e, fake_stream_cleanup(m, f->iface.stream, s) );
    return e;
}

void _advance_fakes(
    manual_scheduler_t *m, fake_stream_t **f, size_t nf
){
    // continuously run streams until none of them need a fake_stream_done()

run_loop:
    manual_scheduler_run(m);

    for(size_t i = 0; i < nf; i++){
        if(f[i]->iface.awaited || !f[i]->iface.canceled) continue;
        derr_t e_canceled = { .type = E_CANCELED };
        fake_stream_done(f[i], e_canceled);
        goto run_loop;
    }
}


static void fcnct_cancel(citm_connect_i *iface){
    fake_citm_connect_t *fcnct =
        CONTAINER_OF(iface, fake_citm_connect_t, iface);
    fcnct->canceled = true;
}

void fake_citm_connect_prep(fake_citm_connect_t *fcnct){
    *fcnct = (fake_citm_connect_t){
        .iface = {
            .cancel = fcnct_cancel,
        },
    };
}

derr_t fake_citm_connect_finish(
    fake_citm_connect_t *fcnct, citm_conn_t *conn, derr_type_t etype
){
    derr_t e = E_OK;

    if(fcnct->done){
        // test bug
        ORIG_GO(&e, E_INTERNAL, "fake_citm_finish() called twice", cu);
    }

    if(!fcnct->cb){
        // test bug
        ORIG_GO(&e,
            E_INTERNAL,
            "fake_citm_finish() called, but fcnct has not been started",
        cu);
    }

    if(etype == E_NONE && !conn){
        // test bug
        ORIG_GO(&e, E_INTERNAL, "etype = E_NONE but no conn was provided", cu);
    }

    if((etype == E_CANCELED) != fcnct->canceled){
        // not a bug, just a failure
        ORIG_GO(&e,
            E_VALUE,
            "etype = %x but fcnct->canceled = %x",
            cu,
            FD(error_to_dstr(etype)),
            FB(fcnct->canceled)
        );
    }

cu:
    /* always do something valid, even if the test asked for something invalid,
       so whatever owns the citm_connect_i can shut down properly */

    if(!fcnct->cb || fcnct->done) return e;

    fcnct->done = true;
    if(fcnct->canceled){
        fcnct->cb(fcnct->data, NULL, (derr_t){ .type = E_CANCELED });
    }else if(etype != E_NONE){
        fcnct->cb(fcnct->data, NULL, (derr_t){ .type = etype });
    }else if(!conn){
        // cb should be a success but there's no conn to give
        fcnct->cb(fcnct->data, NULL, (derr_t){ .type = E_INTERNAL });
    }else{
        // success case
        fcnct->cb(fcnct->data, conn, (derr_t){0});
    }

    return e;
}

static derr_t fio_connect_imap(
    citm_io_i *iface, citm_conn_cb cb, void *data, citm_connect_i **out
){
    derr_t e = E_OK;
    *out = NULL;

    fake_citm_io_t *fio = CONTAINER_OF(iface, fake_citm_io_t, iface);
    link_t *link = link_list_pop_first(&fio->fcncts);
    if(!link){
        ORIG(&e, E_VALUE, "unexpected call to connect_imap");
    }
    fake_citm_connect_t *fcnct = CONTAINER_OF(link, fake_citm_connect_t, link);

    fcnct->cb = cb;
    fcnct->data = data;
    *out = &fcnct->iface;

    return e;
}

citm_io_i *fake_citm_io(fake_citm_io_t *fio){
    *fio = (fake_citm_io_t){ .iface = { .connect_imap = fio_connect_imap } };
    return &fio->iface;
}

static const keypair_t *fkd_mykey(keydir_i *iface){
    fake_keydir_t *fkd = CONTAINER_OF(iface, fake_keydir_t, iface);
    return fkd->mykey;
}

static link_t *fkd_peers(keydir_i *iface){
    fake_keydir_t *fkd = CONTAINER_OF(iface, fake_keydir_t, iface);
    return &fkd->peers;
}

static derr_t fkd_add(keydir_i *iface, const dstr_t pem){
    derr_t e = E_OK;
    fake_keydir_t *fkd = CONTAINER_OF(iface, fake_keydir_t, iface);

    keypair_t *new;
    PROP(&e, keypair_from_pubkey_pem(&new, pem));
    link_list_append(&fkd->peers, &new->link);

    return e;
}

static void fkd_delete(keydir_i *iface, const dstr_t bin_fpr){
    fake_keydir_t *fkd = CONTAINER_OF(iface, fake_keydir_t, iface);
    keypair_t *kp, *temp;
    LINK_FOR_EACH_SAFE(kp, temp, &fkd->peers, keypair_t, link){
        if(dstr_eq(*kp->fingerprint, bin_fpr)){
            link_remove(&kp->link);
            keypair_free(&kp);
            return;
        }
    }
}

static void fkd_free(keydir_i *iface){
    fake_keydir_t *fkd = CONTAINER_OF(iface, fake_keydir_t, iface);
    link_t *link;
    keypair_free(&fkd->mykey);
    while((link = link_list_pop_first(&fkd->peers))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
}

static DSTR_PRESET(
    mykey_priv,
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIICXQIBAAKBgQCU9j/irie2dpd2gaiVpEh7LKg6fI2OMab/tBcoZqYvsQkQX1dg\n"
    "i8s9bFXpibycxzuyy4S3DyeVP2Vx8jhNHkTa9BLPBmPmhk7U6qgE2rV9jTdwOaAo\n"
    "Uiv0POWUhJXIITEvejobiYMFcQ9hLCJYddwam9o/UwJpH89DBf32mJduYQIDAQAB\n"
    "AoGAFFTb+WON1hCvsaQWz33hyrYYrAruAzdxtLru4jvIeP/v3cU1lt7duZ98xmhf\n"
    "TwK+ejPfBGFUJMHHZdsKpjP4b7jLVtzYkGYtxlpy4Ioyhozg2vCVPmyg84yY8atw\n"
    "KI7FlNjCpNGkGNheBX7SMYEGaIQGuQ1MVdVDCNjoc3qOZQECQQDE9N/vXheKAnZd\n"
    "I95s5yZKg5kln7KYGfrL77/qCRADtGEJ+E2oaed2tvUD8Sc46woEpWo0O1mBWV8Y\n"
    "MwTvduXRAkEAwZ4Z5i+V9F2j5BybDPTP/nZe+pUjG0AwhIzngpmnvzYgpu8/3Koe\n"
    "DOxUx421mKP6OfyB5QM+ZP8lRLrR+pTTkQJAXQ+vN7TnvmgHcV7fW+mkKBUiKarZ\n"
    "ghDUdcPklDqP/JAgQcu3NdpEac1s2934QGaeJy/ZjLB2TC3kRtTkghlV4QJBAJUN\n"
    "9thLqACxGhvhncgSrBE05Ze5uoYfG3rf0tarHgXJUMfTBfIGEQ5X3kimIrg4/Mkp\n"
    "SIKaxa0Q84r+2+oyKtECQQC0mreZ3jup6aLKIs4ztCNLmemB8wLRnFV36o0eq+iY\n"
    "dRWR9AsEh8gFtNqDvVLMx6OSqMTRLZr6XGIDeQbUcjaZ\n"
    "-----END RSA PRIVATE KEY-----\n"
);
static DSTR_PRESET(
    mykey_pem,
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCU9j/irie2dpd2gaiVpEh7LKg6\n"
    "fI2OMab/tBcoZqYvsQkQX1dgi8s9bFXpibycxzuyy4S3DyeVP2Vx8jhNHkTa9BLP\n"
    "BmPmhk7U6qgE2rV9jTdwOaAoUiv0POWUhJXIITEvejobiYMFcQ9hLCJYddwam9o/\n"
    "UwJpH89DBf32mJduYQIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
);
static DSTR_PRESET(
    mykey_fpr,
    "eefdab7d7d97bf74d16684f803f3e2a4ef7aa181c9940fbbaff4427f1f7dde32"
);


static DSTR_PRESET(
    peer1_pem,
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQClSBGeRBYAqTXg7a7sMCEfDgd8\n"
    "IceNWgahGVMNdrXy6KuhyZpDEktchn1X+bvBxBLTIASKQu6+/qrIG09O5WID8iUn\n"
    "mUBPRXw2Nkq4M0Bl8nEpA4yA/OulzvbOC/lChW6l4avBOFsUgOPS8TTXB1lz48Lc\n"
    "eivlTlzAmryj0k1SvwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
);
static DSTR_PRESET(
    peer1_fpr,
    "3d94f057f427e2ee34bb51733b8d3ee62a8fdaaa50da71d14e4b2d7f44763471"
);

static DSTR_PRESET(
    peer2_pem,
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQC9Xe4XzXbZSD2ng1H1J0EAdgMR\n"
    "ISaZ3jDunhpxAv2dVLb4cnizugsGAZtwzBUO5uJnb1tHfUL3+qMF2dBBQ0q5GxM3\n"
    "6aUpLkZ7/p+kvjZQtDynSxlpbR+BoYIfS2zKyaZ0cZj8Vdl6ljybiWtSHFVm3U5D\n"
    "/2m95oYikN5gLOIB3QIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
);
static DSTR_PRESET(
    peer2_fpr,
    "8c7e72356d46734eeaf2d163302cc560f60b513d7644dae92b390b7d8f28ae95"
);

static DSTR_PRESET(
    peer3_pem,
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQC65r5IdAr3OuK1utQGH4T9uXPo\n"
    "YmNlNJ2RFz0ybnIMAgqAq8FX/ewItmoQBnEKsrLSbT4JCSk/L3bWOIfcVqu+sCsi\n"
    "MJKipNFWKcL+8G3gnxB7ifpKLAg+sMMR8h2etan5z8ijGlpZcJpxob7eFJ5ZbrjH\n"
    "zrzTUEa6FdkdlgsnCwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
);
static DSTR_PRESET(
    peer3_fpr,
    "d99c55626294263cf8dbe54c0286673f666d9ac969b5856ff78558394365f360"
);

derr_t fake_keydir(fake_keydir_t *fkd, const dstr_t mykeypem, keydir_i **out){
    derr_t e = E_OK;
    *out = NULL;

    keypair_t *mykey;
    PROP(&e, keypair_from_private_pem(&mykey, mykeypem) );

    *fkd = (fake_keydir_t){
        .iface = {
            .mykey = fkd_mykey,
            .peers = fkd_peers,
            .add_key = fkd_add,
            .delete_key = fkd_delete,
            .free = fkd_free,
        },
        .mykey = mykey,
    };

    *out = &fkd->iface;

    return e;
}

derr_t fake_keydir_add_peer(fake_keydir_t *fkd, const dstr_t pem){
    derr_t e = E_OK;

    keypair_t *peer;
    PROP(&e, keypair_from_pubkey_pem(&peer, pem) );

    link_list_append(&fkd->peers, &peer->link);

    return e;
}

// libcitm test utilities

derr_t ctx_setup(const char *test_files, SSL_CTX **s_out, SSL_CTX **c_out){
    derr_t e = E_OK;

    *s_out = NULL;
    *c_out = NULL;

    ssl_context_t sctx = {0};
    ssl_context_t cctx = {0};

    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);
    PROP_GO(&e, FMT(&cert, "%x/ssl/good-cert.pem", FS(test_files)), fail);
    PROP_GO(&e, FMT(&key, "%x/ssl/good-key.pem", FS(test_files)), fail);
    PROP_GO(&e, ssl_context_new_server(&sctx, cert.data, key.data), fail);
    PROP_GO(&e, ssl_context_new_client(&cctx), fail);

    *s_out = sctx.ctx;
    *c_out = cctx.ctx;

    return e;

fail:
    ssl_context_free(&sctx);
    ssl_context_free(&cctx);
    return e;
}

// greets the client
derr_t establish_imap_client(manual_scheduler_t *m, fake_stream_t *fs){
    derr_t e = E_OK;

    manual_scheduler_run(m);
    EXPECT_B(&e, "want read greeting", fake_stream_want_read(fs), true);
    fake_stream_feed_read_all(fs, DSTR_LIT("* OK hi\r\n"));
    manual_scheduler_run(m);

    return e;
}

// read server greeting
derr_t establish_imap_server(manual_scheduler_t *m, fake_stream_t *fs){
    derr_t e = E_OK;

    manual_scheduler_run(m);
    EXPECT_B(&e, "want write greeting", fake_stream_want_write(fs), true);
    dstr_t msg = fake_stream_pop_write(fs);
    DSTR_STATIC(exp,
        "* OK [CAPABILITY IMAP4rev1 IDLE AUTH=PLAIN LOGIN] "
        "greetings, friend!\r\n"
    );
    EXPECT_D3(&e, "greeting", msg, exp);
    fake_stream_write_done(fs);
    manual_scheduler_run(m);

    return e;
}
