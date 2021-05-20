static void print_help(void){
    printf(
"splintermail: client application for Splintermail.com users\n"
"\n"
"  The `splintermail` application serves two purposes:\n"
"    1) It implements the \"decrypter-in-the-middle\" (DITM) model, which is the\n"
"       glue that connects an email client on your computer to the fully\n"
"       encrypted email stored on the Splintermail.com server.  In DITM mode this\n"
"       application will host a POP3 server on your local machine which your\n"
"       email client can connect to.\n"
"    2) It is a tool for controlling your account on the command line, instead of\n"
"       going to the Splintermail.com website.\n"
"\n"
"usage: splintermail ditm [OPTIONS]\n"
"       splintermail API_COMMAND [OPTIONS]\n"
"\n"
"\n"
"OPTIONS:\n"

"\n"

"  -h --help            Print help information and usage.                   \n"
"  -v --version         Print version.                                      \n"
"  -D --debug           Show more verbose output. Note that this option will\n"
"                       print passwords to stdout in plain text.            \n"
"  -c --config FILE     Override default config file loading sequence.      \n"
"                       Default: (see FILES section)                        \n"
"     --dump-conf       Dump config options to stdout.                      \n"

"\n"

"OPTIONS, ditm-specific:\n"

"\n"

"     --pop-port PORT   Port number for the local POP3 server.             \n"
"                       Default: 1995                                      \n"
"  -d --ditm-dir DIR    Where to store decrypted emails and account info.  \n"
"                       Default: /var/lib/splintermail                     \n"
"  -l --logfile FILE    Location of the log file.                          \n"
"                       Default: ${ditm-dir}/ditm_log                      \n"
"  -L --no-logfile      No log file should be kept. Good for systemd, which\n"
"                       keeps logs based on stdin and stdout.              \n"
"     --cert CERT       TLS certificate for the local POP3 server.         \n"
"                       Default: ${ditm-dir}/ditm-127.0.0.1-cert.pem       \n"
"     --key KEY         TLS key for the local POP3 server.                 \n"
"                       Default: ${ditm-dir}/ditm-127.0.0.1-key.pem        \n"

"\n"

"OPTIONS, api_command-specific:\n"

"\n"

"  -u --user EMAIL      Which account should be used for API_COMMAND. This\n"
"                       will take the form of a full email address.  Not  \n"
"                       necessary unless you use multiple Splintermail    \n"
"                       accounts on this computer.                        \n"
"  -a --account-dir DIR Where per-account API tokens are stored.          \n"
"                       Default: (see FILES section)                      \n"

"\n"

"API_COMMANDS:\n"

"\n"

"  list_aliases            Show a list of all aliases on your account.     \n"
"  add_random_alias        Generate a new random alias.                    \n"
"  add_primary_alias ALIAS Request ALIAS as a new primary alias.           \n"
"  delete_alias ALIAS      Delete ALIAS from your account.                 \n"
"  list_devices            List devices keys registered to your account.   \n"
"                          Splintermail encrypts your stored emails to each\n"
"                          device registered to your account.              \n"
"  delete_device FPR       Unregister a device from your account.          \n"
"  list_tokens             List API Tokens registered to your account.  API\n"
"                          Tokens are used to authorize changes to your    \n"
"                          account.                                        \n"
"  delete_token TOKEN      Revoke an API Token.                            \n"
"  account_info            View a summary of your account information.     \n"
"  change_password         Change your Splintermail account password.      \n"

"\n"

"FILES:\n"

"    Config Files:\n"

"        By default, `splintermail` will parse the following config files in\n"
"        order, giving higher precedence to values found earlier in the search:\n"
"\n"
"            $XDG_CONFIG_HOME/splintermail.conf\n"
"            $HOME/.splintermail.conf\n"
"            /etc/splintermail.conf\n"
"\n"
"        If a specific config file is provided via the the `--config`, the\n"
"        default config files are not read.\n"

"\n"

"    DITM Email Storage:\n"

"        By default, `splintermail` will parse the following config files in\n"
"        order, giving higher precedence to values found earlier in the search:\n"
"\n"
"            $XDG_CONFIG_HOME/splintermail.conf\n"
"            $HOME/.splintermail.conf\n"
"            /etc/splintermail.conf\n"
"\n"
"        If a specific config file is provided via the the `--config`, the\n"
"        default config files are not read.\n"

"\n"

"    Per-User API Tokens:\n"

"        By default, `splintermail` will parse the following config files in\n"
"        order, giving higher precedence to values found earlier in the search:\n"
"\n"
"            $XDG_CONFIG_HOME/splintermail.conf\n"
"            $HOME/.splintermail.conf\n"
"            /etc/splintermail.conf\n"
"\n"
"        If a specific config file is provided via the the `--config`, the\n"
"        default config files are not read.\n"
);
}
