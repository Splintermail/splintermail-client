#include "libcli/libcli.h"

DEF_CONTAINER_OF(uv_configure_t, iface, configure_i)
DEF_CONTAINER_OF(uv_configure_t, http, duv_http_t)
DEF_CONTAINER_OF(uv_configure_t, reader, stream_reader_t)

static void http_close_cb(duv_http_t *http){
    uv_configure_t *uvc = CONTAINER_OF(http, uv_configure_t, http);
    // must be safe to free the api_client_t now
    api_client_free(&uvc->apic);
    advancer_schedule(&uvc->c.advancer, E_OK);
}

static void sc_update_cb(void *data, citm_status_t status){
    uv_configure_t *uvc = data;
    configure_status_client_update(&uvc->c, status);
}

static void sc_done_cb(void *data, derr_t e){
    uv_configure_t *uvc = data;
    configure_status_client_done(&uvc->c, e);
}

static void tos_cb(stream_reader_t *reader, derr_t e){
    uv_configure_t *uvc = CONTAINER_OF(reader, uv_configure_t, reader);
    configure_get_acme_dir_done(&uvc->c, e);
}

static void apic_cb(void *data, derr_t e, json_t *json){
    (void)json;
    uv_configure_t *uvc = data;
    configure_add_inst_done(&uvc->c, e);
}

static derr_t uvc_io_init(configure_i *iface, bool sc){
    derr_t e = E_OK;

    uv_configure_t *uvc = CONTAINER_OF(iface, uv_configure_t, iface);

    // set up http
    PROP(&e,
        duv_http_init(
            &uvc->http, &uvc->root.loop, &uvc->root.scheduler, NULL
        )
    );

    // set up an api_client
    PROP(&e, api_client_init(&uvc->apic, &uvc->http, uvc->baseurl) );

    if(sc){
        PROP(&e,
            status_client_init(
                &uvc->sc,
                &uvc->root.loop,
                &uvc->root.scheduler.iface,
                uvc->status_sock,
                sc_update_cb,
                sc_done_cb,
                uvc // cb_data
            )
        );
    }

    return e;
}

static bool uvc_io_close(configure_i *iface){
    uv_configure_t *uvc = CONTAINER_OF(iface, uv_configure_t, iface);

    // cancel the http object
    if(duv_http_close(&uvc->http, http_close_cb)) return true;
    // close the status client
    if(status_client_close(&uvc->sc)) return true;

    return false;
}

static void uvc_status_client_check(configure_i *iface){
    uv_configure_t *uvc = CONTAINER_OF(iface, uv_configure_t, iface);
    status_client_check(&uvc->sc);
}

static void uvc_get_acme_dir(configure_i *iface, dstr_t *resp){
    uv_configure_t *uvc = CONTAINER_OF(iface, uv_configure_t, iface);
    // GET the acme directory
    rstream_i *r = duv_http_req(
        &uvc->req,
        &uvc->http,
        HTTP_METHOD_GET,
        uvc->acmeurl,
        NULL, // params
        NULL, // hdrs
        (dstr_t){0}, // body
        NULL // hdr_cb
    );
    stream_read_all(&uvc->reader, r, resp, tos_cb);
}

static void uvc_add_inst(
    configure_i *iface, dstr_t user, dstr_t pass, json_t *resp
){
    uv_configure_t *uvc = CONTAINER_OF(iface, uv_configure_t, iface);
    DSTR_STATIC(path, "/api/add_installation");
    dstr_t arg = {0};
    apic_pass(&uvc->apic, path, arg, user, pass, resp, apic_cb, uvc);
}

static void uvc_print(configure_i *iface, const char *fstr, const fmt_i *arg){
    (void)iface;
    const fmt_i *args[] = {arg};
    size_t nargs = !!arg;
    _fmt_quiet(WF(stderr), fstr, args, nargs);
}

static derr_t uvc_prompt_one_of(
    configure_i *iface, dstr_t msg, const char *opts, size_t *ret
){
    (void)iface;
    return prompt_one_of(user_prompt, msg, opts, ret);
}

static derr_t uvc_user_prompt(
    configure_i *iface, dstr_t msg, dstr_t *out, bool hide
){
    (void)iface;
    return user_prompt(msg, out, hide);
}

static derr_t uvc_mkdirs(configure_i *iface, string_builder_t path){
    (void)iface;
    return mkdirs_path(&path, 0700);
}

static derr_t uvc_dir_rw_access(
    configure_i *iface, string_builder_t path, bool *ret
){
    (void)iface;
    return dir_rw_access_path(&path, false, ret);
}

static derr_t uvc_write_inst(
    configure_i *iface, installation_t inst, string_builder_t path
){
    (void)iface;
    return installation_write_path(inst, path);
}

derr_t uv_configure_main(
    const dstr_t baseurl,
    const dstr_t acmeurl,
    const string_builder_t status_sock,
    const string_builder_t smdir,
    const dstr_t user,
    bool force,
    int *retval
){
    derr_t e = E_OK;

    uv_configure_t uvc = {
        .iface = {
            .io_init = uvc_io_init,
            .io_close = uvc_io_close,
            .status_client_check = uvc_status_client_check,
            .get_acme_dir = uvc_get_acme_dir,
            .add_inst = uvc_add_inst,
            .print = uvc_print,
            .prompt_one_of = uvc_prompt_one_of,
            .user_prompt = uvc_user_prompt,
            .mkdirs = uvc_mkdirs,
            .dir_rw_access = uvc_dir_rw_access,
            .write_inst = uvc_write_inst,
        },
        .baseurl = baseurl,
        .status_sock = status_sock,
    };

    PROP(&e, parse_url(&acmeurl, &uvc.acmeurl) );

    PROP(&e,
        configure_init(
            &uvc.c,
            &uvc.iface,
            &uvc.root.scheduler.iface,
            smdir,
            user,
            force,
            retval
        )
    );

    PROP(&e, duv_root_run(&uvc.root, &uvc.c.advancer) );

    return e;
}
