// "splintermail is..."
#define STATUS_MAJ_MAP(XX) \
    XX(STATUS_MAJ_NO_TLS, 0, "running without tls") \
    XX(STATUS_MAJ_MANUAL_CERT, 1, "running with manual certificate") \
    XX(STATUS_MAJ_NEED_CONF, 2, "running, but requires configuration") \
    XX(STATUS_MAJ_TLS_FIRST, 3, "obtaining initial tls certificate") \
    XX(STATUS_MAJ_TLS_EXPIRED, 4, "certificate expired") \
    XX(STATUS_MAJ_TLS_RENEW, 5, "renewing its tls certificate") \
    XX(STATUS_MAJ_TLS_GOOD, 6, "certificate is ready for use") \

// "splintermail is $maj: ..."
#define STATUS_MIN_MAP(XX) \
    XX(STATUS_MIN_NONE, 0, "") \
    XX(STATUS_MIN_CREATE_ACCOUNT, 1, "creating an acme account") \
    XX(STATUS_MIN_RELOAD, 2, "checking on previous acme order") \
    XX(STATUS_MIN_CREATE_ORDER, 3, "creating an acme order") \
    XX(STATUS_MIN_GET_AUTHZ, 4, "getting the acme authorization") \
    XX(STATUS_MIN_PREPARE_CHALLENGE, 5, "preparing for the acme challenge") \
    XX(STATUS_MIN_COMPLETE_CHALLENGE, 6, "completing the acme challenge") \
    XX(STATUS_MIN_GENERATE_KEY, 7, "generating a new tls key") \
    XX(STATUS_MIN_FINALIZE_ORDER, 8, "finalizing the acme order") \
    XX(STATUS_MIN_RETRY, 9, "retrying again soon") \

#define STATUS_ENUM(name, val, str) name = val,
typedef enum {
    STATUS_MAJ_MAP(STATUS_ENUM)
} status_maj_e;

typedef enum {
    STATUS_MIN_MAP(STATUS_ENUM)
} status_min_e;
#undef STATUS_ENUM

dstr_t status_maj_dstr(status_maj_e maj);
dstr_t status_min_dstr(status_min_e min);

// tristate enum
typedef enum {
    TRI_NO = 0,
    TRI_YES = 1,
    TRI_NA = 2,
} tri_e;

typedef struct {
    jdump_i iface;
    tri_e tri;
} _jdump_tri_t;

derr_type_t _jdump_tri(jdump_i *iface, writer_i *out, int indent, int pos);

#define DTRI(tri) (&(_jdump_tri_t){ { _jdump_tri }, tri }.iface)

typedef struct {
    jspec_t jspec;
    tri_e *out;
} _jspec_tri_t;

derr_t _jspec_tri(jspec_t *jspec, jctx_t *ctx);

#define JTRI(out) (&(_jspec_tri_t){ { _jspec_tri }, out }.jspec)

typedef struct {
    int version_maj;
    int version_min;
    int version_patch;
    dstr_t fulldomain;
    // client only sees strings, not the enums
    dstr_t status_maj;
    dstr_t status_min;
    tri_e configured;
    tri_e tls_ready;
} citm_status_t;

// copies input strings
derr_t citm_status_init(
    citm_status_t *status,
    int version_maj,
    int version_min,
    int version_patch,
    dstr_t fulldomain,
    dstr_t status_maj,
    dstr_t status_min,
    tri_e configured,
    tri_e tls_ready
);

void citm_status_free(citm_status_t *status);
