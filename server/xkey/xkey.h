// Let dovecot do it's thing unfettered by our warnings.
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wstrict-prototypes"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  ifdef __clang__
#    pragma GCC diagnostic ignored "-Wextra-semi"
#    pragma GCC diagnostic ignored "-Wcast-qual"
#  else
#    pragma GCC diagnostic ignored "-Wformat="
#  endif // __clang__
#endif // __GNUC__

    #include "config.h"
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
#include "server/badbadbad_alert.h"

bool cmd_xkeysync(struct client_command_context *cmd);
bool cmd_xkeyadd(struct client_command_context *cmd);

void xkey_plugin_init(struct module *module);
void xkey_plugin_deinit(void);
