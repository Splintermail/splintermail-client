#!/usr/bin/env python3

import contextlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time

# TODO: this is so gross
manage_sql_path = os.path.join(
    os.path.dirname(os.path.realpath(__file__)),
    "../../../idmpt/profiles/mail/mail/mysql"
)
sys.path.append(manage_sql_path)
import manage_sql


@contextlib.contextmanager
def rm_on_exception(path):
    try:
        yield
    except:
        shutil.rmtree(path)
        raise


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


def bootstrap_db(basedir):
    datadir = os.path.join(basedir, "data")
    if os.path.exists(datadir):
        # bootstrap not necessary
        return

    print("bootstrapping mariadb...")
    os.makedirs(basedir, exist_ok=True)
    cmd = ["mariadb-install-db", "--no-defaults", "--basedir", "."]
    p = subprocess.Popen(
        cmd, cwd=basedir, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    success = True
    try:
        out, err = p.communicate(timeout=15)
    except TimeoutExpired:
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


def configure_db(creds, sockpath):
    with open(os.path.join(manage_sql_path, "mysqlconf.json")) as f:
        dbconf = json.load(f)

    patch = manage_sql.build_patch(creds, dbconf)

    if not patch:
        return

    print("configuring database...")
    ScriptRunner(sockpath).run(patch, None)
    print("configuring done!")

    patch = manage_sql.build_patch(creds, dbconf)
    if patch:
        raise ValueError("\n\x1b[31m"+patch+"\x1b[m")


def wait_for_socket(sockpath):
    for i in range(1000):
        if os.path.exists(sockpath):
            break
        time.sleep(0.01)
    else:
        raise ValueError("socket was not created")


@contextlib.contextmanager
def mariadb(basedir):
    with rm_on_exception(basedir):
        bootstrap_db(basedir)

    datadir = os.path.join(basedir, "data")
    sock = "mariadb.sock"
    sockpath = os.path.join(datadir, sock)

    creds = manage_sql.Creds(host="localhost", unix_socket=sockpath)

    cmd = ["mariadbd", "--no-defaults", "--datadir", datadir, "--socket", sock]
    p = subprocess.Popen(cmd)#, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    try:
        wait_for_socket(sockpath)

        configure_db(creds, sockpath)

        yield ScriptRunner(sockpath)

    finally:
        print("sending SIGTERM to database")
        p.send_signal(signal.SIGTERM)
        ret = p.wait()


if __name__ == "__main__":
    with mariadb("./mariadb") as creds:
        while True:
            time.sleep(1000)
