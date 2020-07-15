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

USER="test@splintermail.com"
PASS="password"

def as_bytes(msg):
    if isinstance(msg, bytes):
        return msg
    return msg.encode("utf8")


class EOFError(Exception):
    pass

class IOThread(threading.Thread):
    def __init__(self, io, q):
        self.io = io
        self.q = q
        self.buffer = []
        self.started = False
        self.closed = False
        super().__init__()

    def full_text(self):
        return b''.join(self.buffer)

    def start(self):
        self.started = True
        super().start()

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, typ, val, trace):
        self.close(failed=typ is not None)

    def close(self, failed=False):
        if not self.started:
            return

        if self.closed:
            return

        self.closed = True
        self.quit(failed)
        self.join()


class ReaderThread(IOThread):
    def __init__(self, closable, *arg):
        self.closable = closable
        super().__init__(*arg)

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

    def quit(self, failed):
        if failed:
            self.closable.close()


class SocketReaderThread(IOThread):
    def __init__(self, closable, *arg):
        self.closable = closable
        super().__init__(*arg)

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

    def quit(self, failed):
        if failed:
            self.closable.close()


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

    def quit(self, _):
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
def run_connection(closable, host="127.0.0.1", port=1993):
    with socket.socket() as sock:
        sock.connect((host, port))
        tls = client_context().wrap_socket(sock, server_hostname=host)

        read_q = queue.Queue()
        write_q = queue.Queue()

        with WriterThread(tls, write_q), \
                SocketReaderThread(closable, tls, read_q):
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
            raise EOFError("did not find \"listener ready\" message")
        if line == b"listener ready\n":
            break


def wait_for_match(q, pattern):
    ignored = []
    while True:
        line = q.get()
        if line is None:
            raise EOFError("EOF")
        match = re.match(pattern, line)
        if match is not None:
            return match, ignored
        ignored.append(line)


def ensure_no_match(ignored, pattern):
    for line in ignored:
        if re.match(pattern, line):
            raise ValueError(f"{line} matched {pattern}")

def wait_for_resp(q, tag, status, require=tuple(), disallow=tuple()):
    """all patterns in require must be present, and none in disallow"""
    tag = as_bytes(tag)
    status = as_bytes(status)
    recvd = []
    while True:
        line = q.get()
        if line is None:
            raise EOFError("EOF")
        # ensure no disallow matches
        for dis in disallow:
            if re.match(dis, line) is not None:
                raise ValueError(f"got disallowed match to ({dis}): {line}")
        recvd.append(line)
        # Is this the last line?
        if line.startswith(tag + b" "):
            # end of command response
            if not line.startswith(tag + b" " + status):
                raise ValueError(f"needed {status}, got: {line}")
            break
    # return the first match of every required pattern
    req_matches = []
    for req in require:
        for line in recvd:
            match = re.match(req, line)
            if match is not None:
                req_matches.append(match)
                break
        else:
            raise ValueError(f"required pattern ({req}) not found")
    return req_matches


def disallow_match(q, pattern):
    while True:
        line = q.get()


class Subproc:
    def __init__(self, cmd):
        self.started = False
        self.closed = False
        self.out_q = queue.Queue()
        self.cmd = cmd

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, typ, *_):
        dump_logs = typ is not None
        try:
            self.close()
        except:
            dump_logs = True

        if dump_logs:
            fmt_failure(self.reader)

    def start(self):
        self.p = subprocess.Popen(
            self.cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE
        )

        self.reader = ReaderThread(self, self.p.stdout, self.out_q)
        self.reader.start()

        self.started = True

        wait_for_listener(self.out_q)

    def close(self, sig=signal.SIGTERM):
        if not self.started:
            return

        if self.closed:
            return
        self.closed = True

        try:
            if sig is not None:
                self.p.send_signal(sig)

            try:
                self.p.wait(TIMEOUT)
            except subprocess.TimeoutExpired:
                self.p.send_signal(signal.SIGKILL)
                self.p.wait()
                raise ValueError("cmd failed to exit promply, sent SIGKILL")

            if self.p.poll() != 0:
                raise ValueError("cmd exited %d"%self.p.poll())
        finally:
            self.reader.close()


@contextlib.contextmanager
def _session(closable):
    with run_connection(closable) as (write_q, read_q):
        wait_for_match(read_q, b"\\* OK")

        write_q.put(
            b"A login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8'))
        )
        wait_for_match(read_q, b"A OK")

        yield write_q, read_q

        write_q.put(b"Z logout\r\n")
        wait_for_match(read_q, b"\\* BYE")
        wait_for_match(read_q, b"Z OK")



@contextlib.contextmanager
def session(cmd):
    with Subproc(cmd) as subproc:
        with _session(subproc) as stuff:
            yield stuff


@contextlib.contextmanager
def _inbox(closable):
    with _session(closable) as (write_q, read_q):
        write_q.put(b"B select INBOX\r\n")
        wait_for_match(read_q, b"B OK")

        yield write_q, read_q

@contextlib.contextmanager
def inbox(cmd):
    with session(cmd) as (write_q, read_q):
        write_q.put(b"B select INBOX\r\n")
        wait_for_match(read_q, b"B OK")

        yield write_q, read_q


#### tests


def test_start_kill(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        pass


def test_login_logout(cmd, maildir_root):
    with session(cmd) as (write_q, read_q):
        pass


def test_select_logout(cmd, maildir_root):
    with inbox(cmd) as (write_q, read_q):
        pass


def test_select_close(cmd, maildir_root):
    with inbox(cmd) as (write_q, read_q):
        write_q.put(b"1 close\r\n")
        wait_for_resp(read_q, "1", "OK")


def test_select_select(cmd, maildir_root):
    with inbox(cmd) as (write_q, read_q):
        write_q.put(b"1 select \"Test Folder\"\r\n")
        wait_for_resp(read_q, "1", "OK")


def test_store(cmd, maildir_root):
    with inbox(cmd) as (write_q, read_q):
        write_q.put(b"1 store 1 flags \\Seen\r\n")
        wait_for_resp(read_q, "1", "OK")

        write_q.put(b"2 fetch 1 flags\r\n")
        wait_for_resp(
            read_q,
            "2",
            "OK",
            require=[b"\\* 1 FETCH \\(FLAGS \\(\\\\Seen\\)\\)"],
        )

        write_q.put(b"3 store 1 -flags \\Seen\r\n")
        wait_for_resp(read_q, "3", "OK")

        write_q.put(b"4 fetch 1 flags\r\n")
        wait_for_resp(
            read_q,
            "4",
            "OK",
            require=[b"\\* 1 FETCH \\(FLAGS \\(\\)\\)"],
        )

        # noop store (for seq num = UINT_MAX-1)
        write_q.put(b"5 store 4294967294 flags \\Seen\r\n")
        wait_for_resp(
            read_q,
            "5",
            "OK",
            require=[b"5 OK noop STORE"],
            disallow=[b"\\* [0-9]* FLAGS"],
        )


def get_uid(seq_num, write_q, read_q):
    write_q.put(b"U fetch %d UID\r\n"%seq_num)
    matches = wait_for_resp(
        read_q,
        "U",
        "OK",
        require=[b"\\* %d FETCH \\(UID ([0-9]*)\\)"%seq_num],
    )
    return matches[0][1]


def test_expunge(cmd, maildir_root):
    with inbox(cmd) as (write_q, read_q):
        uid1 = get_uid(1, write_q, read_q)
        uid2 = get_uid(2, write_q, read_q)

        # expunge two messages and confirm they report back in reverse order
        write_q.put(b"1 store 1:2 flags \\Deleted\r\n")
        wait_for_resp(read_q, "1", "OK")

        write_q.put(b"2 expunge\r\n")
        wait_for_resp(
            read_q,
            "2",
            "OK",
            require=[
                b"\\* 2 EXPUNGE",
                b"\\* 1 EXPUNGE",
            ],
        )

        # confirm neither UID is still present
        write_q.put(b"3 search UID %s\r\n"%uid1)
        wait_for_resp(
            read_q,
            "3",
            "OK",
            disallow=[b"\\* SEARCH [0-9]*"],
        )

        write_q.put(b"4 search UID %s\r\n"%uid2)
        wait_for_resp(
            read_q,
            "4",
            "OK",
            disallow=[b"\\* SEARCH [0-9]*"],
        )


def test_expunge_on_close(cmd, maildir_root):
    with inbox(cmd) as (write_q, read_q):
        uid = get_uid(1, write_q, read_q)

        write_q.put(b"1 store 1 flags \\Deleted\r\n")
        wait_for_resp(read_q, "1", "OK")

        write_q.put(b"2 close\r\n")
        wait_for_resp(read_q, "2", "OK")

        write_q.put(b"3 select INBOX\r\n")
        wait_for_resp(read_q, "3", "OK")

        write_q.put(b"4 search UID %s\r\n"%uid)
        wait_for_resp(
            read_q,
            "4",
            "OK",
            disallow=[b"\\* SEARCH [0-9]*"],
        )


def test_no_expunge_on_logout(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as (write_q, read_q):
            uid = get_uid(1, write_q, read_q)

            write_q.put(b"1 store 1 flags \\Deleted\r\n")
            wait_for_resp(read_q, "1", "OK")

        with _inbox(subproc) as (write_q, read_q):
            write_q.put(b"1 UID search UID %s\r\n"%uid)
            wait_for_resp(
                read_q,
                "1",
                "OK",
                require=[b"\\* SEARCH %s*"%uid],
            )


def test_noop(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as (w1, r1), _inbox(subproc) as (w2, r2):
            # empty flags for a few messages
            w1.put(b"1a store 1:3 flags ()\r\n")
            wait_for_resp(r1, "1a", "OK")

            # sync to second connection
            w2.put(b"1b NOOP\r\n")
            wait_for_resp(r2, "1b", "OK")

            # make some updates
            w1.put(b"3a store 1 flags \\Deleted\r\n")
            wait_for_resp(r1, "3a", "OK")
            w1.put(b"4a store 2 flags \\Answered\r\n")
            wait_for_resp(r1, "4a", "OK")
            w1.put(b"5a store 3 flags \\Answered\r\n")
            wait_for_resp(r1, "5a", "OK")
            w1.put(b"6a expunge\r\n")
            wait_for_resp(r1, "6a", "OK")

            # sync to second connection
            assert r2.empty(), "expected empty queue before NOOP"
            w2.put(b"2b NOOP\r\n")
            wait_for_resp(
                r2,
                "2b",
                "OK",
                require=[
                    b"\\* 2 FETCH \\(FLAGS \\(\\\\Answered\\)\\)",
                    b"\\* 3 FETCH \\(FLAGS \\(\\\\Answered\\)\\)",
                    b"\\* 1 EXPUNGE"
                ],
            )


def test_up_transition(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _session(subproc) as (w1, r1):
            with _session(subproc) as (w2, r2):
                # let the second connection be the primary up_t
                w2.put(b"1b select INBOX\r\n")
                wait_for_resp(r2, "1b", "OK")

                w1.put(b"1a select INBOX\r\n")
                wait_for_resp(r1, "1a", "OK")

                # Make sure everything is working
                w1.put(b"2a STORE 1 flags ()\r\n")
                wait_for_resp(r1, "2a", "OK")
                w2.put(b"2b STORE 1 flags \\Answered\r\n")
                wait_for_resp(r2, "2b", "OK")

            # Make sure the first connection still works
            w1.put(b"3a STORE 1 flags ()\r\n")
            wait_for_resp(
                r1,
                "3a",
                "OK",
                require=[b"\\* 1 FETCH \\(FLAGS \\(\\)\\)"],
            )


def do_passthru_test(write_q, read_q):
    write_q.put(b"1 LIST \"\" *\r\n")
    wait_for_resp(
        read_q,
        "1",
        "OK",
        require=[b"\\* LIST \\(.*\\) \"/\" INBOX"],
    )

    write_q.put(b"2 LSUB \"\" *\r\n")
    wait_for_resp(read_q, "2", "OK")

    write_q.put(b"3 STATUS INBOX (MESSAGES)\r\n")
    wait_for_resp(
        read_q,
        "3",
        "OK",
        require=[b"\\* STATUS INBOX \\(MESSAGES [0-9]*\\)"],
    )

    # test SUBSCRIBE and UNSUBSCRIBE
    write_q.put(b"4 SUBSCRIBE INBOX\r\n")
    wait_for_resp(read_q, "4", "OK")

    write_q.put(b"5 LSUB \"\" *\r\n")
    wait_for_resp(
        read_q,
        "5",
        "OK",
        require=[b"\\* LSUB \\(.*\\) \"/\" INBOX"],
    )

    write_q.put(b"6 UNSUBSCRIBE INBOX\r\n")
    wait_for_resp(read_q, "6", "OK")

    write_q.put(b"7 LSUB \"\" *\r\n")
    wait_for_resp(
        read_q,
        "7",
        "OK",
        disallow=[b"\\* LSUB \\(.*\\) \"/\" INBOX"],
    )

    # test CREATE and DELETE
    name = codecs.encode(b"deleteme_" + os.urandom(5), "hex_codec")

    write_q.put(b"8 CREATE %s\r\n"%name)
    wait_for_resp(read_q, "8", "OK")

    write_q.put(b"9 LIST \"\" *\r\n")
    wait_for_resp(
        read_q,
        "9",
        "OK",
        require=[b"\\* LIST \\(.*\\) \"/\" %s"%name],
    )

    write_q.put(b"10 DELETE %s\r\n"%name)
    wait_for_resp(read_q, "10", "OK")

    write_q.put(b"11 LIST \"\" *\r\n")
    wait_for_resp(
        read_q,
        "11",
        "OK",
        disallow=[b"\\* LIST \\(.*\\) \"/\" %s"%name],
    )


def test_passthru_unselected(cmd, maildir_root):
    with session(cmd) as (write_q, read_q):
        do_passthru_test(write_q, read_q)


def test_passthru_selected(cmd, maildir_root):
    with inbox(cmd) as (write_q, read_q):
        do_passthru_test(write_q, read_q)


def test_terminate_with_open_connection(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as (write_q, read_q):
            wait_for_match(read_q, b"\\* OK")

            p = subproc.p
            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


def test_terminate_with_open_session(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as (write_q, read_q):
            wait_for_match(read_q, b"\\* OK")

            write_q.put(
                b"1 login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8'))
            )
            wait_for_match(read_q, b"1 OK")

            p = subproc.p
            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


def test_terminate_with_open_mailbox(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as (write_q, read_q):
            wait_for_match(read_q, b"\\* OK")

            write_q.put(
                b"1 login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8'))
            )
            wait_for_match(read_q, b"1 OK")

            write_q.put(b"2 select INBOX\r\n")
            wait_for_match(read_q, b"2 OK")

            p = subproc.p
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
        test_start_kill,
        test_login_logout,
        test_select_logout,
        test_select_close,
        test_select_select,
        test_store,
        test_expunge,
        test_expunge_on_close,
        test_no_expunge_on_logout,
        test_noop,
        test_up_transition,
        test_passthru_unselected,
        test_passthru_selected,
        test_terminate_with_open_connection,
        test_terminate_with_open_session,
        test_terminate_with_open_mailbox,
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
        test(cmd, maildir_root)

    print("PASS")
