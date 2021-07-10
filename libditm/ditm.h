static const unsigned int DITM_VERSION_MAJOR = 0;
static const unsigned int DITM_VERSION_MINOR = 2;
static const unsigned int DITM_VERSION_BUILD = 0;

typedef struct {
    LIST(dstr_t) list;
    LIST(bool) seen;
    dstr_t mem;
} ignore_list_t;

typedef struct {
    // dirpath will usually be "~/.ditm"
    char dirpath_buffer[4096];
    dstr_t dirpath;
    // holds uids and indicies for remote server messages
    pop_client_t pc;
    pop_server_t ps;
    // holds uids (indicies are implicit) for local messages
    maildir_t maildir;
    // list which messages have been marked as deleted
    LIST(bool) deletions;
    // in some cases we present an offline interface to the email client
    bool minversion_met;
    bool conn_is_live;
    // values we will have to pass on to the key_tool for API calls
    const char* api_host;
    unsigned int api_port;
    // whether or not we successfully logged in
    bool logged_in;
} ditm_t;

derr_t ditm_new(ditm_t* ditm, ssl_context_t* ctx, connection_t* conn,
                const char* rhost, unsigned int rport, const char* dirpath,
                const char* api_host, unsigned int api_port);

void ditm_free(ditm_t* ditm);

derr_t ditm_mangle_unencrypted(int infd, int outfd, size_t* outlen);
/* throws E_INTERNAL
          E_OS (reading or writing, not opening) */

derr_t ditm_mangle_corrupted(int infd, int outfd, size_t* outlen);
/* throws E_INTERNAL
          E_OS (reading or writing, not opening) */

derr_t ditm_inject_message(ditm_t* ditm, const dstr_t* subj, const dstr_t* msg);

void ditm_signal_handler(int signum);

derr_t ditm_loop(const char* rhost, unsigned int rport,
                 const char* ditm_dir, unsigned int port,
                 const char* api_host, unsigned int api_port,
                 const char* certpath, const char* keypath);

derr_t ignore_list_new(ignore_list_t* list);
void ignore_list_free(ignore_list_t* list);
derr_t ignore_list_load(ignore_list_t* il, const dstr_t* userdir);
/* throws: E_NOMEM
           E_FS (path too long)
           E_INTERNAL (from FMT)
           E_OS (from fread) */

derr_t ignore_list_write(ignore_list_t* il, const dstr_t* userdir);
/* throws: E_NOMEM
           E_FS (path too long, or permissions)
           E_INTERNAL (from FMT)
           E_OS (from fwrite) */

derr_t ignore_list_add(ignore_list_t* il, const dstr_t* uid);
bool ignore_list_should_ignore(ignore_list_t* il, const dstr_t* uid);
