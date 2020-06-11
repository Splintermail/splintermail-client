#!/usr/bin/env python3

import tempfile
import contextlib
import shutil
import sys
import traceback
import signal
import os
import queue
import socket
import re
import subprocess
import threading
import ssl


class IOThread(threading.Thread):
    def __init__(self, io, q):
        self.io = io
        self.q = q
        self.buffer = []
        super().__init__()

    def full_text(self):
        return b''.join(self.buffer)

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_):
        self.quit()
        self.join()


class ReaderThread(IOThread):
    def run(self):
        try:
            while True:
                data = self.io.readline()
                if len(data) == 0:
                    break
                self.buffer.append(data)
                self.q.put(data)
        finally:
            self.q.put(None)

    def quit(self):
        self.io.close()


class SocketReaderThread(IOThread):
    def run(self):
        buf = b""
        try:
            while True:
                data = self.io.recv(4096)
                # check for EOF
                if len(data) == 0:
                    break
                buf += data
                # check for full lines
                lines = buf.split(b"\n")
                for full_line in lines[:-1]:
                    full_line += b"\n"
                    self.buffer.append(full_line)
                    self.q.put(full_line)
                buf = lines[-1]
        finally:
            self.q.put(None)

    def quit(self):
        self.io.close()


class WriterThread(IOThread):
    def run(self):
        try:
            while True:
                data = self.q.get()
                if data is None:
                    break
                self.buffer.append(data)
                self.io.write(data)
        finally:
            self.io.close()

    def quit(self):
        self.q.put(None)


_client_context = None

def client_context(cafile=None):
    global _client_context

    if _client_context is None:
        _client_context = ssl.create_default_context()
        if cafile is not None:
            _client_context.load_verify_locations(cafile=cafile)
        # on some OS's ssl module loads certs automatically
        if len(_client_context.get_ca_certs()) == 0:
            # on others it doesn't
            import certifi
            _client_context.load_verify_locations(cafile=certifi.where())

    return _client_context


@contextlib.contextmanager
def run_connection(host="127.0.0.1", port=1993):
    with socket.socket() as sock:
        sock.connect((host, port))
        tls = client_context().wrap_socket(sock, server_hostname=host)

        read_q = queue.Queue()
        write_q = queue.Queue()

        with WriterThread(tls, write_q), SocketReaderThread(tls, read_q):
            yield write_q, read_q


def fmt_failure(reader):
    print("log from failed test:", file=sys.stderr)
    print("=====================", file=sys.stderr)
    if reader is not None:
        os.write(sys.stderr.fileno(), reader.full_text())
    print("=====================", file=sys.stderr)
    print("(end of log)", file=sys.stderr)


def wait_for_listener(q):
    while True:
        line = q.get()
        if line is None:
            raise ValueError("did not find \"listener ready\" message")
        if line == b"listener ready\n":
            break


def wait_for_match(q, pattern):
    ignored = []
    while True:
        line = q.get()
        if line is None:
            raise ValueError("EOF")
        match = re.match(pattern, line)
        if match is not None:
            return match, ignored
        ignored.append(line)


def ensure_no_match(ignored, pattern):
    for line in ignored:
        if re.match(pattern, line):
            raise ValueError(f"{line} matched {pattern}")


@contextlib.contextmanager
def run_subproc(cmd):
    # print(" ".join(cmd))
    out_q = queue.Queue()
    p = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE
    )
    dump_logs= False
    try:
        with ReaderThread(p.stdout, out_q) as reader:
            wait_for_listener(out_q)

            yield p, out_q

            # in the no-error case, expect a clean and near-instant exit
            p.send_signal(signal.SIGTERM)
            p.wait(0.1)
    except:
        dump_logs= True
        raise
    finally:
        # expect that the user has already waited the process
        if p.poll() is None:
            p.send_signal(signal.SIGKILL)
            p.wait()
            fmt_failure(reader)
            raise ValueError("cmd failed to exit promply, sent SIGKILL")
        if p.poll() != 0:
            fmt_failure(reader)
            raise ValueError("cmd exited %d"%p.poll())
        if dump_logs:
            fmt_failure(reader)


def start_kill(cmd):
    with run_subproc(cmd) as (p, out_q):
        pass


@contextlib.contextmanager
def _session(p):
    with run_connection() as (write_q, read_q):
        wait_for_match(read_q, b"\\* OK")

        write_q.put(b"A login test@splintermail.com password\r\n")
        wait_for_match(read_q, b"A OK")

        yield p, write_q, read_q

        write_q.put(b"Z logout\r\n")
        wait_for_match(read_q, b"\\* BYE")
        wait_for_match(read_q, b"Z OK")


@contextlib.contextmanager
def session(cmd):
    with run_subproc(cmd) as (p, out_q):
        with _session(p) as stuff:
            yield stuff


def login_logout(cmd):
    with session(cmd) as (p, write_q, read_q):
        pass


@contextlib.contextmanager
def inbox(cmd):
    with session(cmd) as (p, write_q, read_q):
        write_q.put(b"B select INBOX\r\n")
        wait_for_match(read_q, b"B OK")

        yield p, write_q, read_q


def select_logout(cmd):
    with inbox(cmd) as (p, write_q, read_q):
        pass


def select_close(cmd):
    with inbox(cmd) as (p, write_q, read_q):
        write_q.put(b"1 close\r\n")
        wait_for_match(read_q, b"1 OK")


def select_select(cmd):
    with inbox(cmd) as (p, write_q, read_q):
        write_q.put(b"1 select \"Test Folder\"\r\n")
        wait_for_match(read_q, b"1 OK")


def store(cmd):
    with inbox(cmd) as (p, write_q, read_q):
        write_q.put(b"1 store 1 flags \\Seen\r\n")
        wait_for_match(read_q, b"1 OK")

        write_q.put(b"2 fetch 1 flags\r\n")
        wait_for_match(read_q, b"\\* 1 FETCH \\(FLAGS \\(\\\\Seen\\)\\)")
        wait_for_match(read_q, b"2 OK")

        write_q.put(b"3 store 1 -flags \\Seen\r\n")
        wait_for_match(read_q, b"3 OK")

        write_q.put(b"4 fetch 1 flags\r\n")
        wait_for_match(read_q, b"\\* 1 FETCH \\(FLAGS \\(\\)\\)")
        wait_for_match(read_q, b"4 OK")

        # noop store (for seq num = UINT_MAX-1)
        write_q.put(b"5 store 4294967294 flags \\Seen\r\n")
        _, ignored = wait_for_match(read_q, b"5 OK noop STORE")
        ensure_no_match(ignored, b"\\* [0-9]* FLAGS")


def get_uid(seq_num, write_q, read_q):
    write_q.put(b"U fetch %d UID\r\n"%seq_num)
    match, _ = wait_for_match(
        read_q, b"\\* %d FETCH \\(UID ([0-9]*)\\)"%seq_num
    )
    wait_for_match(read_q, b"U OK")
    return match[1]


def expunge_on_close(cmd):
    with inbox(cmd) as (p, write_q, read_q):
        uid = get_uid(1, write_q, read_q)

        write_q.put(b"1 store 1 flags \\Deleted\r\n")
        wait_for_match(read_q, b"1 OK")

        write_q.put(b"2 close\r\n")
        wait_for_match(read_q, b"2 OK")

        write_q.put(b"3 select INBOX\r\n")
        wait_for_match(read_q, b"3 OK")

        write_q.put(b"4 search UID %s\r\n"%uid)
        _, ignored = wait_for_match(read_q, b"4 OK")
        ensure_no_match(ignored, b"\\* SEARCH [0-9]*")


def no_expunge_on_logout(cmd):
    with run_subproc(cmd) as (p, out_q):
        with _session(p) as (p, write_q, read_q):
            write_q.put(b"1 select INBOX\r\n")
            wait_for_match(read_q, b"1 OK")

            uid = get_uid(1, write_q, read_q)

            write_q.put(b"2 store 1 flags \\Deleted\r\n")
            wait_for_match(read_q, b"2 OK")

        with _session(p) as (p, write_q, read_q):
            write_q.put(b"1 select INBOX\r\n")
            wait_for_match(read_q, b"1 OK")

            write_q.put(b"2 UID search UID %s\r\n"%uid)
            wait_for_match(read_q, b"\\* SEARCH %s*"%uid)
            wait_for_match(read_q, b"2 OK")


def terminate_with_open_connection(cmd):
    with run_subproc(cmd) as (p, out_q):
        with run_connection() as (write_q, read_q):
            wait_for_match(read_q, b"\\* OK")

            p.send_signal(signal.SIGTERM)
            p.wait(0.1)


# Prepare a subdirectory
@contextlib.contextmanager
def maildir_root_copy(maildir_root_originals):
    tempdir = tempfile.mkdtemp()
    try:
        dst = os.path.join(tempdir, os.path.basename(maildir_root_originals))
        shutil.copytree(maildir_root_originals, dst)
        yield dst
    finally:
        shutil.rmtree(tempdir)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: %s /path/to/test/files"%(sys.argv[0]), file=sys.stderr)
        sys.exit(1)

    test_files = sys.argv[1]

    tests = [
        start_kill,
        login_logout,
        select_logout,
        select_close,
        select_select,
        store,
        expunge_on_close,
        no_expunge_on_logout,
        terminate_with_open_connection,
    ]

    for test in tests:
        with maildir_root_copy(
            os.path.join(test_files, "e2e_citm", "maildir_root")
        ) as maildir_root:
            cmd = [
                "citm/citm",
                # "--local-host", "127.0.0.1"
                "--local-port", "1993",
                # "--remote-host", "127.0.0.1"
                "--remote-port", "993",
                "--tls-key", os.path.join(test_files, "ssl/good-key.pem"),
                "--tls-cert", os.path.join(test_files, "ssl/good-cert.pem"),
                "--tls-dh", os.path.join(test_files, "ssl/dh_4096.pem"),
                "--mail-key", os.path.join(test_files, "key_tool/key_m.pem"),
                "--maildirs", maildir_root,
            ]
        test(cmd)

    print("PASS")
