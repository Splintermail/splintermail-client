#include "xkey.h"

// happily, our commands are totally independent of mailboxes
static const enum command_flags no_cmd_flags = 0
// | COMMAND_FLAG_USES_SEQS
// | COMMAND_FLAG_BREAKS_SEQS
// | COMMAND_FLAG_USES_MAILBOX
// | COMMAND_FLAG_REQUIRES_SYNC
// | COMMAND_FLAG_USE_NONEXISTENT
;

/* we only need a reference to the module so we can check if it has been added
   or not when modifying the capability string */
static struct module *xkey_module = NULL;
static imap_client_created_func_t *next_hook_client_created;

// add XKEY to the capability list
static void xkey_client_created(struct client **client) {
    if (mail_user_is_plugin_loaded((*client)->user, xkey_module)) {
        str_append((*client)->capability_string, " XKEY");
    }

    if (next_hook_client_created != NULL) {
        next_hook_client_created(client);
    }
}

void xkey_plugin_init(struct module *module){
    // configure libdstr logging
    logger_add_fileptr(LOG_LVL_INFO, stderr);

    // configure mysql library
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        fprintf(stderr, "unable to init mysql library!");
    }

    command_register("XKEYSYNC", cmd_xkeysync, no_cmd_flags);
    command_register("XKEYADD", cmd_xkeyadd, no_cmd_flags);

    next_hook_client_created = imap_client_created_hook_set(
        xkey_client_created
    );

    xkey_module = module;
}

void xkey_plugin_deinit(void){
    logger_clear_outputs();
    mysql_library_end();
    command_unregister("XKEYADD");
    command_unregister("XKEYSYNC");
}
