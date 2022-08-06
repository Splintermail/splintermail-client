#include "dns.c"

static const char *respond_fn_name(respond_f respond){
    if(respond == norespond) return "norespond";
    if(respond == respond_notimpl) return "respond_notimpl";
    if(respond == respond_refused) return "respond_refused";
    if(respond == respond_name_error) return "respond_name_error";
    if(respond == respond_root) return "respond_root";
    if(respond == respond_user) return "respond_user";
    if(respond == respond_acme) return "respond_acme";
    return "unknown";
}

static derr_t test_sort_pkt(void){
    derr_t e = E_OK;

    #define ACME "\x0f""_acme-challenge"
    #define X "\x01""x"
    #define USER "\x04""user"
    #define SPLINTERMAIL "\x0c""splintermail"
    #define COM "\x03""com"
    #define A 1
    #define NS 2
    #define SOA 6
    #define TXT 16
    #define AAAA 28
    struct {
        char *name;
        uint8_t qr;          // must be 0
        uint8_t opcode : 4;  // must be 0
        uint8_t qdcount;     // must be 1
        uint8_t qclass;      // must be 1
        uint16_t qtype;      // must be 1 or higher
        respond_f exp;
    } cases[] = {
        // invalid queries
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_root },
        { USER SPLINTERMAIL COM, 1, 0, 1, 1, 1, norespond },
        { USER SPLINTERMAIL COM, 0, 1, 1, 1, 1, norespond },
        { USER SPLINTERMAIL COM, 0, 0, 0, 1, 1, norespond },
        { USER SPLINTERMAIL COM, 0, 0, 1, 0, 1, norespond },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 0, norespond },
        // rejected qtypes
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 17, respond_notimpl },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 27, respond_notimpl },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 29, respond_notimpl },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 255, respond_notimpl },
        // refuse bad names before rejecting qtypes
        {  X   SPLINTERMAIL COM, 0, 0, 1, 1, 255, respond_refused },
        // accepted qtypes
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_root },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 2, respond_root },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 6, respond_root },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 16, respond_root },
        { USER SPLINTERMAIL COM, 0, 0, 1, 1, 28, respond_root },
        // name checking
        {   ACME X USER SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_acme },
        {        X USER SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_user },
        {          USER SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_root },
        {               SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_refused },
        {                    X       COM, 0, 0, 1, 1, 1, respond_refused },
        {                            COM, 0, 0, 1, 1, 1, respond_refused },
        {                             X , 0, 0, 1, 1, 1, respond_refused },
        {   ACME X USER SPLINTERMAIL  X , 0, 0, 1, 1, 1, respond_refused },
        {   ACME X USER      X       COM, 0, 0, 1, 1, 1, respond_refused },
        {   ACME X  X   SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_refused },
        {    X   X USER SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_name_error },
        { X ACME X USER SPLINTERMAIL COM, 0, 0, 1, 1, 1, respond_name_error },
    };

    size_t ncases = sizeof(cases) / sizeof(*cases);
    for(size_t i = 0; i < ncases; i++){
        dns_pkt_t pkt = {
            .hdr = {
                .qr = cases[i].qr,
                .opcode = cases[i].opcode,
                .qdcount = cases[i].qdcount,
            },
            .qstn = {
                .qclass = cases[i].qclass,
                .qtype = cases[i].qtype,
            },
        };
        lstr_t rname[10];
        size_t cap = sizeof(rname) / sizeof(*rname);
        size_t n = labels_read_reverse(cases[i].name, 0, rname, cap);
        pkt.qstn.qtype = cases[i].qtype;
        respond_f got = sort_pkt(pkt, rname, n);
        if(got != cases[i].exp){
            char buf[128];
            int len = 0;
            for(size_t i = 0; i < n; i++){
                lstr_t l = rname[n-i-1];
                len += sprintf(buf + len, "%.*s.", (int)l.len, l.str);
            }
            TRACE(&e,
                "for sort_packet(%x, %x): expected %x but got %x\n",
                FS(buf),
                FU(cases[i].qtype),
                FS(respond_fn_name(cases[i].exp)),
                FS(respond_fn_name(got))
            );
            ORIG(&e, E_VALUE, "failed case");
        }
    }

    #undef ACME
    #undef X
    #undef USER
    #undef SPLINTERMAIL
    #undef COM
    #undef A
    #undef NS
    #undef SOA
    #undef TXT
    #undef AAAA

    return e;
}

int main(void){
    int retval = 0;

    logger_add_fileptr(LOG_LVL_INFO, stdout);

    #define RUN(code) do { \
        derr_t e = code; \
        if(is_error(e)){ \
            DUMP(e); \
            DROP_VAR(&e); \
            retval = 1; \
        } \
    } while(0)

    RUN(test_sort_pkt());

    printf("%s\n", retval ? "FAIL" : "PASS");

    return retval;
}

