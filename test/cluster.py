#!/usr/bin/env python3

"""
Run a test cluster including:
  - mysql
  - dovecot
  - pebble
  - our kvpsend service
  - our dns service
  - our rest api

This test cluster is sufficient to test our full acme logic.
"""

import argparse
import collections
import contextlib
import os.path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading

import dovecot
import mariadb

sys.path.append("server/pysm")
import pysm

HERE = os.path.dirname(__file__)


class ThreadBase(threading.Thread):
    def __init__(self, outcond):
        self._outcond = outcond
        self._incond = threading.Condition()
        self._quit = False
        self._didstart = False
        self._joined = False
        self.ready = False
        self.err = None
        super().__init__()

    def __enter__(self):
        # print(f"{type(self).__name__} starting")
        self.start()
        self._didstart = True
        return self

    def __exit__(self, *args):
        self.join()

    def run(self):
        try:
            self.run_safe()
        except BaseException as e:
            with self._outcond:
                self.err = e
                self._outcond.notify()

    def mark_ready(self):
        with self._outcond:
            self.ready = True
            self._outcond.notify()

    def await_close(self):
        with self._incond:
            while not self._quit:
                self._incond.wait()

    def close(self):
        # print(f"{type(self).__name__} closing")
        with self._incond:
            self._quit = True
            self._incond.notify()

    def join(self):
        if not self._didstart or self._joined:
            return
        # print(f"{type(self).__name__} joining")
        super().join()
        self._joined = True
        # print(f"{type(self).__name__} joined")


class Mariadb(ThreadBase):
    def __init__(self, outcond, tmp):
        super().__init__(outcond)
        self.tmp = tmp
        self.sqlsock = None
        self.runner = None

    def run_safe(self):
        basedir = os.path.join(self.tmp, "mariadb")
        os.mkdir(basedir)
        migrations = os.path.join(HERE, "../server/migrations")
        migmysql_path = "server/migmysql"
        with mariadb.mariadb(
            basedir=basedir,
            migrations=migrations,
            migmysql_path=migmysql_path,
        ) as runner:
            self.runner = runner
            self.sqlsock = runner.sockpath
            self.mark_ready()
            self.await_close()

    def join(self):
        if self._joined:
            return
        super().join()
        self._joined = True


class Dovecot(ThreadBase):
    def __init__(self, outcond, tmp, sqlsock, port=None):
        super().__init__(outcond)
        self._sqlsock = sqlsock
        self.tmp = tmp
        self.port = port

    def run_safe(self):
        basedir = os.path.join(self.tmp, "dovecot")
        os.mkdir(basedir)
        with dovecot.Dovecot(
            basedir=basedir,
            sql_sock=self._sqlsock,
            bind_addr="0.0.0.0",
            imap_port=self.port,
        ) as imap_port:
            # update the port, in case it was None
            self.port = imap_port
            self.mark_ready()
            self.await_close()


def subprocess_helper(args, thread_base, prefix, readymsg, env=None, cwd=None):
    cond = thread_base._incond
    q = collections.deque()

    if isinstance(readymsg, str):
        readymsg = readymsg.encode("utf8")

    def reader(kind, pipe):
        try:
            while True:
                line = pipe.readline()
                if not line:
                    return
                with cond:
                    q.append((kind, line))
                    cond.notify()
        finally:
            with cond:
                q.append((kind, None))
                cond.notify()

    p = subprocess.Popen(
        args, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    tout = threading.Thread(target=reader, args=("out", p.stdout))
    terr = threading.Thread(target=reader, args=("err", p.stderr))

    tout.start()
    terr.start()

    try:
        ready = False
        killed = False
        eofs = 0
        with cond:
            while eofs < 2:
                while not q and (killed or not thread_base._quit):
                    cond.wait()
                if not killed and thread_base._quit:
                    killed = True
                    p.kill()
                while q:
                    kind, line = q.popleft()
                    if not line:
                        eofs += 1
                        continue
                    try:
                        disp = line.decode("utf8")
                    except:
                        disp = line
                    print(f"{prefix}({kind})", disp.strip())
                    if not ready and readymsg in line:
                        ready = True
                        cond.release()
                        thread_base.mark_ready()
                        cond.acquire()
        # eofs == 2
        ret = p.wait()
        if not killed and ret != 0:
            print(f"{args[0]} exited {ret}")
            raise ValueError(f"{args[0]} exited {ret}")
    finally:
        p.kill()
        p.wait()
        tout.join()
        terr.join()


class DNS(ThreadBase):
    def __init__(self, outcond, dns, sync, peer):
        super().__init__(outcond)
        self.dns = dns
        self.sync = sync
        self.peer = peer

    def run_safe(self):
        args = [
            "server/dns/dns",
            "--dns", f"127.0.0.1:{self.dns}",
            "--sync", f"127.0.0.1:{self.sync}",
            "--peer", f"127.0.0.1:{self.peer}",
            "--rrl", "101",
        ]
        print(" ".join(args))
        subprocess_helper(args, self, "dns", "ready")


class KVPSend(ThreadBase):
    def __init__(self, outcond, sqlsock, kvpsock, sync, peer):
        super().__init__(outcond)
        self.sqlsock = sqlsock
        self.kvpsock = kvpsock
        self.sync = sync
        self.peer = peer

    def run_safe(self):
        args = [
            "server/kvpsend/kvpsend",
            "--sql", self.sqlsock,
            "--sock", self.kvpsock,
            "--sync", f"127.0.0.1:{self.sync}",
            "--peer", f"127.0.0.1:{self.peer}",
        ]
        subprocess_helper(args, self, "kvpsend", "ready")

class Pebble(ThreadBase):
    def __init__(self, outcond, pebble, dns):
        super().__init__(outcond)
        self.pebble = pebble
        self.dns = dns
        self.port = 14000

    def run_safe(self):
        args = [
            "./pebble", "-dnsserver", f"127.0.0.1:{self.dns}", "-strict",
        ]
        readymsg = "ACME directory available at:"
        env = {"PEBBLE_VA_NOSLEEP": "1"}
        subprocess_helper(
            args, self, "pebble", readymsg, env=env, cwd=self.pebble
        )


class REST(ThreadBase):
    def __init__(self, outcond, tmp, sqlsock, kvpsock, port=8000):
        super().__init__(outcond)
        self.tmp = tmp
        self.sqlsock = sqlsock
        self.kvpsock = kvpsock
        self.port = port

    def run_safe(self):
        # create our api_config.py
        api_logfile = os.path.join(self.tmp, "api.log")
        with open(os.path.join(self.tmp, "api_config.py",), "w") as f:
            f.write(
                "server_id = 301\n"
                f'kvpsend_sock = "{self.kvpsock}"\n'
                f'sqlsock = "{self.sqlsock}"\n'
                f'logfile = "{api_logfile}"\n'
            )

        # create a dummy badbadbad.py
        with open(os.path.join(self.tmp, "badbadbad.py",), "w") as f:
            f.write(
                "def alert_exc(msg):\n"
                '    print("alert_msg", msg)\n'
            )

        # launch gunicorn
        pysmpath = os.path.abspath("server/pysm")
        args = [
            "gunicorn",
            "--pythonpath", f"{pysmpath},{self.tmp}",
            "--bind", f"0.0.0.0:{self.port}",
            "app:app_wrapper",
        ]
        readymsg = "Listening at:"
        cwd = os.path.join(HERE, "../server/rest")
        subprocess_helper(args, self, "rest", readymsg, cwd=cwd)


class Cluster:
    def __init__(
        self,
        *,
        pebble,
        dnsport=1881,
        dnssync=1882,
        kvpsync=1883,
        imap_port=None,
    ):
        self.pebble = pebble
        self.dnsport = dnsport
        self.dnssync = dnssync
        self.kvpsync = kvpsync
        self.cond = threading.Condition()
        self.db = None
        self.dns = None
        self.pb = None
        self.dc = None
        self.rest = None
        self.kvp = None
        self.tmp = None
        self.imap_port = imap_port
        self.es = contextlib.ExitStack()

    def __enter__(self):
        self.es.__enter__()
        try:
            self.startup()
        except:
            self.__exit__()
            raise
        return self

    def healthy(self):
        err = None
        subs = [self.db, self.dns, self.pb, self.dc, self.rest, self.kvp]
        for sub in subs:
            err = err or (sub and sub.err)
        if err is not None:
            raise ValueError("child failed") from err
        return True

    def startup(self):
        # create some paths
        self.tmp = tempfile.mkdtemp()
        kvpsend_sock = os.path.join(self.tmp, "kvpsend.sock")

        # startup services with no depenencies
        self.db = Mariadb(self.cond, self.tmp)
        self.dns = DNS(self.cond, self.dnsport, self.dnssync, self.kvpsync)
        self.pb = Pebble(self.cond, self.pebble, self.dnsport)
        self.es.enter_context(self.db)
        self.es.enter_context(self.dns)
        self.es.enter_context(self.pb)

        # wait for mariadb to start
        with self.cond:
            while self.healthy() and not self.db.ready:
                self.cond.wait()

        # start dovecot and rest api server which need mariadb
        self.dc = Dovecot(
            self.cond,
            self.tmp,
            self.db.sqlsock,
            port=self.imap_port,
        )
        self.rest = REST(self.cond, self.tmp, self.db.sqlsock, kvpsend_sock)
        self.es.enter_context(self.dc)
        self.es.enter_context(self.rest)

        # wait for dns to be ready
        with self.cond:
            while self.healthy() and not self.dns.ready:
                self.cond.wait()

        # start kvp which needs mariadb and dns
        self.kvp = KVPSend(
            self.cond,
            self.db.sqlsock,
            kvpsend_sock,
            self.kvpsync,
            self.dnssync
        )
        self.es.enter_context(self.kvp)

        # wait for remaining services to be ready
        subs = [self.pb, self.dc, self.rest, self.kvp]
        with self.cond:
            while self.healthy() and not all(sub.ready for sub in subs):
                self.cond.wait()

        print(
            "-------------------------\n"
            f"dovecot available  :{self.dc.port}\n"
            f"pebble available   :{self.pb.port}\n"
            f"rest api available :{self.rest.port}\n"
            "-------------------------"
        )

    def run(self):
        with self.cond:
            while self.healthy():
                self.cond.wait()

    def __exit__(self, *args):
        def maybe_close(thing):
            if thing is not None:
                thing.close()

        # close everything first
        maybe_close(self.db)
        maybe_close(self.dns)
        maybe_close(self.pb)
        maybe_close(self.dc)
        maybe_close(self.rest)
        maybe_close(self.kvp)

        # then join everything
        self.es.__exit__(*args)
        if self.tmp is not None:
            shutil.rmtree(self.tmp)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pebble", required=True)
    args = parser.parse_args()

    with Cluster(pebble=args.pebble) as c:
        c.run()
