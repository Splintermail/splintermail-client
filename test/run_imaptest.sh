#!/bin/sh

# default settings
tests=""
host=" host=127.0.0.1"
port=" port=143"
user=" user=\"test@splintermail.com\""
pass=" pass=\"password\""

print_help () {
    echo "usage: run_imaptest.sh path/to/tests [OPTIONS]"
    echo "   where OPTIONS are one of:"
    echo "      --host HOST"
    echo "      --port PORT"
    echo "      --user USER"
    echo "      --pass PASS"
    echo "      --help"
}

# Check options
while [ "$#" -gt 0 ] ; do
    case "$1" in
    (--host) host=" host=\"$2\""; shift;;
    (--port) port=" port=\"$2\""; shift;;
    (--user) user=" user=\"$2\""; shift;;
    (--help) print_help; exit 0;;
    (--) shift; break;;
    (-*) echo "$0: error - unrecognized option $1" 1>&2; exit 1;;
    (*)  break;;
    esac
    shift
done

[ -z "$1" ] && print_help && exit 1
tests=" test=\"$1\"$host$port$user$pass"

# download example mailbox for the test
mbox="dovecot-crlf"
if [ ! -e "$mbox" ] ; then
    curl -L 'http://www.dovecot.org/tmp/dovecot-crlf' > "$mbox"
fi

# see https://imapwiki.org/ImapTest/Running

# The most important parameters (and their defaults) are:
#
#   host=127.0.0.1:
#       Host name/IP address where to connect.
args="$args$host"
#   port=143:
#       Port what to use.
args="$args$port"
#   user=$USER:
#       Username template. You can use multiple random users and domains by
#       giving %d in the template. So for example user%d returns user1..user100
#       and user%d@domain%d.org returns user1..100@domain1..100.org. The upper
#       limit for users can be set with the "users=#" parameter (default 100),
#       and the upper limit for domains can be set with the "domains=#"
#       parameter (default 100).
args="$args$user"
#   userfile=FILE:
#       Read usernames from given file, one user per line. It's also possible
#       to give passwords for users in username:password format.
#   pass=pass:
#       Password to use for all users. There's currently no way to use
#       different passwords for different users.
args="$args$pass"
#   mbox=~/mail/dovecot-crlf:
#       Path to mbox file where to append messages from. See below for how this
#       is used.
args="$args mbox=\"$mbox\""

# Other useful parameters include:
#
#   clients=10:
#       Number of simultaneous client connections to use.
#   secs=n:
#       Run imaptest n seconds and then exit. Setting this to less than the
#       time to run all scripts with tests=dir will lead to spurious test
#       failures.
#   msgs=30:
#       Try to keep the mailbox size around this many messages.
#   box=INBOX:
#       Mailbox to use for testing. INBOX is the default.
#   copybox=n:
#       When testing COPY command, this specifies the destination mailbox.
#   seed=n:
#       Seed to use for random generator. Setting this to some specific value
#       makes repeated benchmarks a bit more reliable, because the used
#       commands should be the same then.
#   disconnect_quit:
#       If a client gets disconnected, quit. Use logout=0 parameter with this
#       one. This is useful when debugging some problem in the server.
#   no_pipelining:
#       Don't send multiple commands at once to server.
#   rawlog:
#       Write rawlog.* files for all connections containing their input and
#       output.
#   master=USER:
#       Use master user logins.
#
# Selecting what to test:
#
#   <state>=<probability>[,<probability2>]:
#       State probabilities to use.
#   checkpoint=n:
#       Run a checkpoint every n seconds. See below.
#   own_msgs:
#       Assigns an owner client for each message. Complain if flags for a
#       message is changed by a non-owner session.
#   own_flags:
#       Assigns an owner client for each flag and keyword. Complain if they're
#       changed by another session.
#   random:
#       Switch randomly between states.
#   no_tracking:
#       Don't track and complain about IMAP state. Makes it use a bit less CPU.
#   test=dir:
#       Run scripted tests from given directory instead of doing stress
#       testing.
args="$args$tests"

echo "imaptest $args"
eval "imaptest $args"
