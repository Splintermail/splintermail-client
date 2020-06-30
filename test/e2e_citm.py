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
import codecs

TIMEOUT = 0.5

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
            p.wait(TIMEOUT)
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
def _session():
    with run_connection() as (write_q, read_q):
        wait_for_match(read_q, b"\\* OK")

        write_q.put(b"A login test@splintermail.com password\r\n")
        wait_for_match(read_q, b"A OK")

        yield write_q, read_q

        write_q.put(b"Z logout\r\n")
        wait_for_match(read_q, b"\\* BYE")
        wait_for_match(read_q, b"Z OK")


@contextlib.contextmanager
def session(cmd):
    with run_subproc(cmd) as (p, out_q):
        with _session() as stuff:
            yield p, *stuff


def login_logout(cmd):
    with session(cmd) as (p, write_q, read_q):
        pass


@contextlib.contextmanager
def _inbox():
    with _session() as (write_q, read_q):
        write_q.put(b"B select INBOX\r\n")
        wait_for_match(read_q, b"B OK")

        yield write_q, read_q

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


def expunge(cmd):
    with inbox(cmd) as (p, write_q, read_q):
        uid1 = get_uid(1, write_q, read_q)
        uid2 = get_uid(2, write_q, read_q)

        # expunge two messages and confirm they report back in reverse order
        write_q.put(b"1 store 1:2 flags \\Deleted\r\n")
        wait_for_match(read_q, b"1 OK")

        write_q.put(b"2 expunge\r\n")
        wait_for_match(read_q, b"\\* 2 EXPUNGE")
        wait_for_match(read_q, b"\\* 1 EXPUNGE")
        wait_for_match(read_q, b"2 OK")

        # confirm neither UID is still present
        write_q.put(b"3 search UID %s\r\n"%uid1)
        _, ignored = wait_for_match(read_q, b"3 OK")
        ensure_no_match(ignored, b"\\* SEARCH [0-9]*")

        write_q.put(b"4 search UID %s\r\n"%uid2)
        _, ignored = wait_for_match(read_q, b"4 OK")
        ensure_no_match(ignored, b"\\* SEARCH [0-9]*")


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
        with _inbox() as (write_q, read_q):
            uid = get_uid(1, write_q, read_q)

            write_q.put(b"1 store 1 flags \\Deleted\r\n")
            wait_for_match(read_q, b"1 OK")

        with _inbox() as (write_q, read_q):
            write_q.put(b"1 UID search UID %s\r\n"%uid)
            wait_for_match(read_q, b"\\* SEARCH %s*"%uid)
            wait_for_match(read_q, b"1 OK")


def noop(cmd):
    with run_subproc(cmd) as (p, out_q):
        with _inbox() as (w1, r1), _inbox() as (w2, r2):
            # empty flags for a few messages
            w1.put(b"1a store 1:3 flags ()\r\n")
            wait_for_match(r1, b"1a OK")

            # sync to second connection
            w2.put(b"1b NOOP\r\n")
            wait_for_match(r2, b"1b OK")

            # make some updates
            w1.put(b"3a store 1 flags \\Deleted\r\n")
            wait_for_match(r1, b"3a OK")
            w1.put(b"4a store 2 flags \\Answered\r\n")
            wait_for_match(r1, b"4a OK")
            w1.put(b"5a store 3 flags \\Answered\r\n")
            wait_for_match(r1, b"5a OK")
            w1.put(b"6a expunge\r\n")
            wait_for_match(r1, b"6a OK")

            # sync to second connection
            assert r2.empty(), "expected empty queue before NOOP"
            w2.put(b"2b NOOP\r\n")
            wait_for_match(r2, b"\\* 2 FETCH \\(FLAGS \\(\\\\Answered\\)\\)")
            wait_for_match(r2, b"\\* 3 FETCH \\(FLAGS \\(\\\\Answered\\)\\)")
            wait_for_match(r2, b"\\* 1 EXPUNGE")
            wait_for_match(r2, b"2b OK")


def up_transition(cmd):
    with run_subproc(cmd) as (p, out_q):
        with _session() as (w1, r1):
            with _session() as (w2, r2):
                # let the second connection be the primary up_t
                w2.put(b"1b select INBOX\r\n")
                wait_for_match(r2, b"1b OK")

                w1.put(b"1a select INBOX\r\n")
                wait_for_match(r1, b"1a OK")

                # Make sure everything is working
                w1.put(b"2a STORE 1 flags ()\r\n")
                wait_for_match(r1, b"2a OK")
                w2.put(b"2b STORE 1 flags \\Answered\r\n")
                wait_for_match(r2, b"2b OK")

            # Make sure the first connection still works
            w1.put(b"3a STORE 1 flags ()\r\n")
            wait_for_match(r1, b"\\* 1 FETCH \\(FLAGS \\(\\)\\)")
            wait_for_match(r1, b"3a OK")

def do_passthru_test(p, write_q, read_q):
    write_q.put(b"1 LIST \"\" *\r\n")
    wait_for_match(read_q, b"\\* LIST \\(.*\\) \"/\" INBOX")
    wait_for_match(read_q, b"1 OK")

    write_q.put(b"2 LSUB \"\" *\r\n")
    wait_for_match(read_q, b"2 OK")

    write_q.put(b"3 STATUS INBOX (MESSAGES)\r\n")
    wait_for_match(read_q, b"\\* STATUS INBOX \\(MESSAGES [0-9]*\\)")
    wait_for_match(read_q, b"3 OK")

    # test SUBSCRIBE and UNSUBSCRIBE
    write_q.put(b"4 SUBSCRIBE INBOX\r\n")
    wait_for_match(read_q, b"4 OK")

    write_q.put(b"5 LSUB \"\" *\r\n")
    wait_for_match(read_q, b"\\* LSUB \\(.*\\) \"/\" INBOX")
    wait_for_match(read_q, b"5 OK")

    write_q.put(b"6 UNSUBSCRIBE INBOX\r\n")
    wait_for_match(read_q, b"6 OK")

    write_q.put(b"7 LSUB \"\" *\r\n")
    _, ignored = wait_for_match(read_q, b"7 OK")
    ensure_no_match(ignored, b"\\* LSUB \\(.*\\) \"/\" INBOX")

    # test CREATE and DELETE
    name = codecs.encode(b"deleteme_" + os.urandom(5), "hex_codec")

    write_q.put(b"8 CREATE %s\r\n"%name)
    wait_for_match(read_q, b"8 OK")

    write_q.put(b"9 LIST \"\" *\r\n")
    wait_for_match(read_q, b"\\* LIST \\(.*\\) \"/\" %s"%name)
    wait_for_match(read_q, b"9 OK")

    write_q.put(b"10 DELETE %s\r\n"%name)
    wait_for_match(read_q, b"10 OK")

    write_q.put(b"11 LIST \"\" *\r\n")
    _, ignored = wait_for_match(read_q, b"11 OK")
    ensure_no_match(ignored, b"\\* LIST \\(.*\\) \"/\" %s"%name)


def passthru_unselected(cmd):
    with session(cmd) as (p, write_q, read_q):
        do_passthru_test(p, write_q, read_q)


def passthru_selected(cmd):
    with inbox(cmd) as (p, write_q, read_q):
        do_passthru_test(p, write_q, read_q)


def terminate_with_open_connection(cmd):
    with run_subproc(cmd) as (p, out_q):
        with run_connection() as (write_q, read_q):
            wait_for_match(read_q, b"\\* OK")

            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


def terminate_with_open_session(cmd):
    with run_subproc(cmd) as (p, out_q):
        with run_connection() as (write_q, read_q):
            wait_for_match(read_q, b"\\* OK")

            write_q.put(b"1 login test@splintermail.com password\r\n")
            wait_for_match(read_q, b"1 OK")

            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


def terminate_with_open_mailbox(cmd):
    with run_subproc(cmd) as (p, out_q):
        with run_connection() as (write_q, read_q):
            wait_for_match(read_q, b"\\* OK")

            write_q.put(b"1 login test@splintermail.com password\r\n")
            wait_for_match(read_q, b"1 OK")

            write_q.put(b"2 select INBOX\r\n")
            wait_for_match(read_q, b"2 OK")

            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


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
        expunge,
        expunge_on_close,
        no_expunge_on_logout,
        noop,
        up_transition,
        passthru_unselected,
        passthru_selected,
        terminate_with_open_connection,
        terminate_with_open_session,
        terminate_with_open_mailbox,
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
