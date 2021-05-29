// compatibility with dovecot's build system:
#define UOFF_T_LONG  // gcc reports that off_t is a `long int` on linux 64
#define SSIZE_T_MAX SSIZE_MAX
#define HAVE_SOCKLEN_T
#define HAVE__BOOL
#define HAVE_STRUCT_IOVEC
#define FLEXIBLE_ARRAY_MEMBER  // c99 flexible array is allowed by gcc
#define STATIC_ARRAY static  // c99 static array keyword is honored by gcc

// Let dovecot do it's thing unfettered by our warnings.
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif // __GNUC__

    #include "imap-common.h"
    #include "imap-commands.h"
    #include "imap-arg.h"
    #include "imap-keepalive.h"
    #include "ioloop.h"
    #include "ostream.h"
    #include "istream.h"
    #include "str.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif // __GNUC__

#include "libdstr/libdstr.h"
#include "server/mysql_util/mysql_util.h"
#include "server/libsmsql.h"

bool cmd_xkeysync(struct client_command_context *cmd);
bool cmd_xkeyadd(struct client_command_context *cmd);

void xkey_plugin_init(struct module *module);
void xkey_plugin_deinit(void);
