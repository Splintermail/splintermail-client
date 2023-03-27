#!/usr/bin/env python3

import argparse
import contextlib
import grp
import os
import pwd
import random
import shutil
import signal
import socket
import subprocess
import tempfile
import time

import mariadb

HERE = os.path.dirname(__file__)


class Dovecot:
    def __init__(
        self,
        *args,
        basedir=None,
        sql_sock=None,
        bind_addr=None,
        imaps_port=None,
        plugin_path=None,
    ):
        assert not args, "all args must be kwargs"
        assert basedir, "basedir is required"
        self.basedir = os.path.abspath(basedir)
        self.sql_sock = os.path.abspath(sql_sock)
        self.bind_addr = bind_addr
        # pick a random port if not provided
        self.imaps_port = imaps_port or random.randint(6000, 2**16-1)
        # assume we are in the build directory if not provided
        self.plugin_path = os.path.abspath(plugin_path or "server/xkey")

        # these aren't even configurable
        self.user = pwd.getpwuid(os.getuid()).pw_name
        self.group = grp.getgrgid(os.getgid()).gr_name

        self.p = None

        # use lsof to avoid dovecot printing unhelpful errors
        self.lsof = shutil.which("lsof")

    def dovecot_conf(self):
        return f"""
# A dovecot config for running locally, such as for testing.

base_dir={self.basedir}/run
state_dir={self.basedir}/state

default_internal_user = {self.user}
default_internal_group = {self.group}
default_login_user = {self.user}

mail_plugin_dir = {self.plugin_path}

ssl_cert =<{HERE}/files/ssl/good-cert.pem
ssl_key =<{HERE}/files/ssl/good-key.pem

# don't use the syslog
log_path = /dev/stdout

## dovecot conf for splintermail testing
login_greeting = Dovecot ready DITMv0.2.0

# no chroot
service imap-login {{
    chroot =
    user = $default_internal_user
    # disable non-ssl imap
    inet_listener imap {{
        address = 0.0.0.0
        port = {self.imaps_port+1}
    }}
    inet_listener imaps {{
        address = {self.bind_addr}
        port = {self.imaps_port}
    }}
}}

# no chroot
service pop3-login {{
    chroot =
    user = $default_internal_user
    # disable non-ssl pop3
    inet_listener pop3 {{
        address =
        port = 0
    }}
    inet_listener pop3s {{
        address = 127.0.0.1
        port = 2995
    }}
}}

# no chroot
service stats {{
    chroot =
}}

# no chroot
service anvil {{
    chroot =
}}

ssl = required

# include FSID characters
auth_username_chars = abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-+_@

auth_mechanisms = plain
# case-sensitive usernames (default was %Lu)
auth_username_format=%u

# this passdb hooks into a database
passdb {{
    driver = sql
    args = {self.basedir}/passdb.conf.ext
}}

mail_uid = $default_internal_user
mail_gid = $default_internal_group
mail_home = {self.basedir}/mail/vmail/%d/%n
mail_location = maildir:{self.basedir}/mail/vmail/%d/%n/mail:LAYOUT=fs

userdb {{
    driver = static
    args = # rely on mail_uid, mail_gid, etc
}}

protocol imap {{
    # xkey only works with imap
    mail_plugins = $mail_plugins xkey
}}

protocols = imap # pop3 lmtp

plugin {{
    sql_socket = {self.sql_sock}
}}""".lstrip()

    def passdb_conf(self):
        return f"""
driver = mysql

connect = host={self.sql_sock} dbname=splintermail

default_pass_scheme = SHA512-CRYPT

# Return fsid@x.splintermail.com and password hash for a given email
# The reason to return the uuid with the domain attached is so dovecot can
# direct POP/IMAP logins to look at the correct folders.  The dovecot.conf
# static userdb which tells dovecot where to find a folder requires a domain
# in the username to find the correct folder.
#
# The reason we accept either normal emails (email = '%u') or uuid addresses
# (user_uuid = TO_UUID('%n')) is that the replicator first checks the passdb
# when it is deciding which addresses it will recv replications for.  It will
# fall back to the userdb if it fails but that will result in many obnoxious
# mail.log error messages.
password_query = SELECT TO_FSID(user_uuid) as username, 'x.splintermail.com' as domain, password FROM accounts WHERE email = '%u' OR user_uuid = TO_UUID_SOFT('%n');
""".lstrip()

    def __enter__(self):
        self.start()
        return self.imaps_port

    def __exit__(self, *_,):
        self.close()

    def _ready_check(self, timeout):
        if self.lsof is not None:
            cmd = ["lsof", f"-itcp:{self.imaps_port}"]
            if subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode == 0:
                return True
            else:
                time.sleep(timeout)
                return False

        # fallback to sockets
        with socket.socket() as s:
            start = time.time()
            s.settimeout(.01)
            try:
                s.connect(('localhost', self.imaps_port))
                return True
            except ConnectionRefusedError:
                wait = .1 - (time.time() - start)
                if wait > 0:
                    time.sleep(wait)
        return False

    def start(self):
        with open(os.path.join(self.basedir, "dovecot.conf"), "w") as f:
            f.write(self.dovecot_conf())

        with open(os.path.join(self.basedir, "passdb.conf.ext"), "w") as f:
            f.write(self.passdb_conf())

        cmd = ["dovecot", "-c", "dovecot.conf", "-F"]
        p = subprocess.Popen(cmd, cwd=self.basedir)

        try:
            for i in range(1000):
                # health check
                exit_code = p.poll()
                if exit_code is not None:
                    raise ValueError(f"dovecot failed, exited {exit_code}")
                if self._ready_check(.01):
                    break
            else:
                self.p.send_signal(signal.SIGTERM)
                self.p.wait()
                self.p = None
                raise ValueError("waiting for dovecot timed out!")

            # success, save p to self
            self.p = p
            p = None
        finally:
            if p is not None:
                p.wait()
                p = None

    def close(self):
        if self.p is None:
            return
        print("sending SIGTERM to dovecot")
        self.p.send_signal(signal.SIGTERM)
        ret = self.p.wait()


@contextlib.contextmanager
def tempdir():
    tempdir = tempfile.mkdtemp()
    try:
        print("using", tempdir)
        yield tempdir
    finally:
        shutil.rmtree(tempdir)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='run dovecot as a user')
    parser.add_argument(
        "--imaps-port",
        type=int,
        default=2993,
        action="store",
        help="imaps port",
    )
    parser.add_argument(
        "--build-dir",
        type=str,
        default=".",
        action="store",
        help="path to build dir",
    )

    args = parser.parse_args()

    imaps_port = args.imaps_port
    migrations = os.path.join(HERE, "..", "server", "migrations")
    migmysql_path = os.path.join(args.build_dir, "server", "migmysql")
    if not os.path.exists(migmysql_path):
        raise ValueError(
            "migmysql is not built, or --build-dir must be specified"
        )

    plugin_path = os.path.join(args.build_dir, "server/xkey")

    with mariadb.mariadb(
        migrations=migrations, migmysql_path=migmysql_path
    ) as runner:
        with tempdir() as basedir:
            with Dovecot(
                basedir=basedir,
                sql_sock=runner.sockpath,
                imaps_port=args.imaps_port,
                bind_addr="127.0.0.1",
                plugin_path=plugin_path,
            ):
                while True:
                    time.sleep(1000)
