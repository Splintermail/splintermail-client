// web linking defined in rfc8288, used in Link: headers

typedef struct {
    dstr_t key;
    dstr_t value;
} weblink_param_t;

typedef struct {
    dstr_t *text;
    dstr_t buf;
    char _buf[1024];
    dstr_t errbuf;
    char _errbuf[512];
    web_scanner_t scanner;
    web_call_t callstack[WEB_WEBLINK_MAX_CALLSTACK];
    web_sem_t semstack[WEB_WEBLINK_MAX_SEMSTACK];
    web_parser_t p;
    // feedback from parser
    size_t nurl;
    url_t url;
    size_t nlabel;
    weblink_param_t param;
    // state
    bool done;
    derr_type_t etype;
} weblinks_t;

url_t *weblinks_iter(weblinks_t *p, const dstr_t *in);
url_t *weblinks_next(weblinks_t *p);
weblink_param_t *weblinks_next_param(weblinks_t *p);
// after iterating, check for errors
derr_type_t weblinks_status(weblinks_t *p);
// in error cases, get the error buffer
dstr_t weblinks_errbuf(weblinks_t *p);

// example usage:
// void hdr_cb(duv_http_req_t *req, const http_pair_t hdr){ (void)req;
//     if(!dstr_ieq(hdr.key, DSTR_LIT("Link"))) return;
//     weblinks_t p;
//     for(weblink_t *w = weblink_iter(&p, hdr.val); w; w = weblink_next(&p)){
//         ...
//         weblink_param_t *param;
//         while((param = weblinks_next_param(&p))){
//             ...
//         }
//     }
//     // check for iteration errors
//     derr_type_t etype =  weblinks_status(&p);
// }
