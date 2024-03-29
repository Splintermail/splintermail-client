#!/usr/bin/env python3

import contextlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import argparse
import tempfile

class ScriptRunner:
    def __init__(self, sockpath):
        self.sockpath = sockpath

    def run(self, script, database):
        cmd = ["mysql", "-S", self.sockpath]
        if database is not None:
            cmd += ["-A", database]
        p = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if isinstance(script, str):
            script = script.encode('utf8')
        out, err = p.communicate(script)
        ret = p.wait()
        if ret != 0:
            print("---- mysql stdout", flush=True)
            os.write(1, out)
            print("---- (end of mysql stdout)")
            print("---- mysql stderr", flush=True)
            os.write(1, err)
            print("---- (end of mysql stderr)")
            raise ValueError("mysql failed")
        return out


def migmysql(
    migmysql_path, migrations, level=None, socket=None, user=None, pwd=None
):
    assert migmysql_path is not None
    assert migrations is not None

    cmd = [migmysql_path, migrations]
    if level is not None:
        cmd += [str(level)]
    if socket is not None:
        cmd += ["--socket", socket]
    if user is not None:
        cmd += ["--user", user]
    if pwd is not None:
        cmd += ["--pass", pwd]
    p = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    out, err = p.communicate()
    ret = p.wait()
    if ret != 0:
        print("---- migmysql stdout", flush=True)
        os.write(1, out)
        print("---- (end of migmysql stdout)")
        print("---- migmysql stderr", flush=True)
        os.write(1, err)
        print("---- (end of migmysql stderr)")
        raise ValueError("migmysql failed")
    return out, err


@contextlib.contextmanager
def rm_on_exception(path):
    try:
        yield
    except:
        shutil.rmtree(path)
        raise


def bootstrap_db(basedir):
    datadir = os.path.join(basedir, "data")
    if os.path.exists(datadir):
        # bootstrap not necessary
        return

    print("bootstrapping mariadb...")
    os.makedirs(basedir, exist_ok=True)
    # on newer installs, mysql_install_db links to mariadb-install-db
    cmd = ["mysql_install_db", "--no-defaults", "--basedir", "."]
    p = subprocess.Popen(
        cmd, cwd=basedir, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    success = True
    try:
        out, err = p.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        success = False
        print("bootstrap timed out, killing process...")
        p.kill()
        out, err = p.communicate()

    if p.poll() != 0:
        success = False
        print("bootstrap process exited %d"%p.poll())

    if not success:
        print("---- bootstrap stdout", flush=True)
        os.write(1, out)
        print("---- (end of bootstrap stdout)")
        print("---- bootstrap stderr", flush=True)
        os.write(1, err)
        print("---- (end of bootstrap stderr)")
        raise ValueError("bootstrap failed")

    print("bootstrapping complete!")


def wait_for_socket(sockpath):
    for i in range(1000):
        if os.path.exists(sockpath):
            break
        time.sleep(0.01)
    else:
        raise ValueError("socket was not created")


@contextlib.contextmanager
def do_mariadb(basedir, migrations, migmysql_path, args):
    assert migrations is None or migmysql_path, \
            "migmysql_path must be provided if migrations is not None"

    with rm_on_exception(basedir):
        bootstrap_db(basedir)

    datadir = os.path.join(basedir, "data")
    sock = os.path.join(basedir, "mariadb.sock")

    cmd = ["mariadbd", "--no-defaults", "--datadir", datadir, "--socket", sock]
    p = subprocess.Popen(cmd + args)

    try:
        wait_for_socket(sock)

        if migrations is not None:
            print("migrating mysql...")
            migmysql(migmysql_path, migrations, socket=sock)
            print("migrating complete!")

        yield ScriptRunner(sock)

    finally:
        print("sending SIGTERM to database")
        p.send_signal(signal.SIGTERM)
        ret = p.wait()


@contextlib.contextmanager
def mariadb(basedir=None, migrations=None, migmysql_path=None, args=None):
    args = args or []
    tempdir = None
    if basedir is None:
        tempdir = tempfile.mkdtemp()
        # make the path to the socket world-accessible
        os.chmod(tempdir, 0o755)
        basedir = tempdir
        print(f"using {tempdir}")

    try:
        with do_mariadb(basedir, migrations, migmysql_path, args) as runner:
            yield runner
    finally:
        if tempfile is not None:
            shutil.rmtree(tempdir)


# When called directly, run the database in a configurable location.
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='run mariadb as a user')
    parser.add_argument(
        "--migrations",
        type=str,
        default=None,
        action="store",
        help="migrations to run after starting the database",
    )
    parser.add_argument(
        "--migmysql",
        type=str,
        default=None,
        action="store",
        help="required with --migrations when not running from build dir",
    )
    parser.add_argument(
        "--persist-to",
        type=str,
        default=None,
        action="store",
        help="use persistent storage",
    )
    parser.add_argument(
        "arg",
        type=str,
        nargs="*",
        help="extra args for mariadb",
    )

    args = parser.parse_args()

    basedir = args.persist_to
    migrations = args.migrations
    migmysql_path = args.migmysql
    if migrations and not migmysql_path:
        # assume we are running in the build directory
        migmysql_path = "server/migmysql"
        if not os.path.exists(migmysql_path):
            raise ValueError(
                "migmysql path could not be guessed and must be provided"
            )
    extra = args.arg

    with mariadb(basedir, migrations, migmysql_path, extra):
        while True:
            time.sleep(1000)
