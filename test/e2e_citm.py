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
import time

all_tests = []

def register_test(fn):
    all_tests.append(fn)
    return fn

TIMEOUT = 0.5

USER="test@splintermail.com"
PASS="password"

def as_bytes(msg):
    if isinstance(msg, bytes):
        return msg
    return msg.encode("utf8")

class DisallowedError(Exception):
    pass

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
        if failed and self.closable is not None:
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
        if failed and self.closable is not None:
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


# reader-writer
class RW:
    def __init__(self, r, w):
        self.r = r
        self.w = w

    def put(self, blob):
        self.w.put(blob)

    def wait_for_match(self, pattern):
        ignored = []
        while True:
            line = self.r.get()
            if line is None:
                raise EOFError("EOF")
            match = re.match(pattern, line)
            if match is not None:
                return match, ignored
            ignored.append(line)

    def wait_for_resp(self, tag, status, require=tuple(), disallow=tuple()):
        """all patterns in require must be present, and none in disallow"""
        tag = as_bytes(tag)
        status = as_bytes(status)
        recvd = []
        while True:
            line = self.r.get()
            if line is None:
                raise EOFError("EOF")
            # ensure no disallow matches
            for dis in disallow:
                if re.match(dis, line) is not None:
                    raise DisallowedError(
                        f"got disallowed match to ({dis}): {line}"
                    )
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
def run_connection(closable, host=None, port=None):
    host = host or "127.0.0.1"
    port = port or 1993
    with socket.socket() as sock:
        sock.connect((host, port))
        tls = client_context().wrap_socket(sock, server_hostname=host)

        read_q = queue.Queue()
        write_q = queue.Queue()

        with WriterThread(tls, write_q), \
                SocketReaderThread(closable, tls, read_q):
            yield RW(read_q, write_q)


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
        try:
            self.close()
        except:
            if typ is None:
                # enforce clean exit
                fmt_failure(self.reader)
                raise

        if typ is not None:
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
def _session(closable, host=None, port=None):
    with run_connection(closable, host=host, port=port) as rw:
        rw.wait_for_match(b"\\* OK")

        rw.put(b"A login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8')))
        rw.wait_for_match(b"A OK")

        yield rw

        rw.put(b"Z logout\r\n")
        rw.wait_for_match(b"\\* BYE")
        rw.wait_for_match(b"Z OK")



@contextlib.contextmanager
def session(cmd):
    with Subproc(cmd) as subproc:
        with _session(subproc) as stuff:
            yield stuff


@contextlib.contextmanager
def _inbox(closable):
    with _session(closable) as rw:
        rw.put(b"B select INBOX\r\n")
        rw.wait_for_match(b"B OK")

        yield rw

@contextlib.contextmanager
def inbox(cmd):
    with session(cmd) as rw:
        rw.put(b"B select INBOX\r\n")
        rw.wait_for_match(b"B OK")

        yield rw


#### tests

@register_test
def test_start_kill(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        pass


@register_test
def test_login_logout(cmd, maildir_root):
    with session(cmd) as rw:
        pass


@register_test
def test_select_logout(cmd, maildir_root):
    with inbox(cmd) as rw:
        pass


@register_test
def test_select_select(cmd, maildir_root):
    with inbox(cmd) as rw:
        rw.put(b"1 select INBOX\r\n")
        rw.wait_for_resp("1", "OK")


@register_test
def test_select_close(cmd, maildir_root):
    with inbox(cmd) as rw:
        rw.put(b"1 close\r\n")
        rw.wait_for_resp("1", "OK")


@register_test
def test_select_select(cmd, maildir_root):
    with inbox(cmd) as rw:
        rw.put(b"1 select \"Test Folder\"\r\n")
        rw.wait_for_resp("1", "OK")


@register_test
def test_store(cmd, maildir_root):
    with inbox(cmd) as rw:
        rw.put(b"1 store 1 flags \\Seen\r\n")
        rw.wait_for_resp("1", "OK")

        rw.put(b"2 fetch 1 flags\r\n")
        rw.wait_for_resp(
            "2",
            "OK",
            require=[b"\\* 1 FETCH \\(FLAGS \\(\\\\Seen\\)\\)"],
        )

        rw.put(b"3 store 1 -flags \\Seen\r\n")
        rw.wait_for_resp("3", "OK")

        rw.put(b"4 fetch 1 flags\r\n")
        rw.wait_for_resp(
            "4",
            "OK",
            require=[b"\\* 1 FETCH \\(FLAGS \\(\\)\\)"],
        )

        # noop store (for seq num = UINT_MAX-1)
        rw.put(b"5 store 4294967294 flags \\Seen\r\n")
        rw.wait_for_resp(
            "5",
            "OK",
            require=[b"5 OK noop STORE"],
            disallow=[b"\\* [0-9]* FLAGS"],
        )


def get_uid(seq_num, rw):
    rw.put(b"U fetch %s UID\r\n"%str(seq_num).encode("ascii"))
    matches = rw.wait_for_resp(
        "U",
        "OK",
        require=[b"\\* [0-9]* FETCH \\(UID ([0-9]*)\\)"],
    )
    return matches[0][1]


@register_test
def test_expunge(cmd, maildir_root):
    with inbox(cmd) as rw:
        uid1 = get_uid(1, rw)
        uid2 = get_uid(2, rw)

        # expunge two messages and confirm they report back in reverse order
        rw.put(b"1 store 1:2 flags \\Deleted\r\n")
        rw.wait_for_resp("1", "OK")

        rw.put(b"2 expunge\r\n")
        rw.wait_for_resp(
            "2",
            "OK",
            require=[
                b"\\* 2 EXPUNGE",
                b"\\* 1 EXPUNGE",
            ],
        )

        # confirm neither UID is still present
        rw.put(b"3 search UID %s\r\n"%uid1)
        rw.wait_for_resp(
            "3",
            "OK",
            disallow=[b"\\* SEARCH [0-9]*"],
        )

        rw.put(b"4 search UID %s\r\n"%uid2)
        rw.wait_for_resp(
            "4",
            "OK",
            disallow=[b"\\* SEARCH [0-9]*"],
        )


@register_test
def test_expunge_on_close(cmd, maildir_root):
    with inbox(cmd) as rw:
        uid = get_uid(1, rw)

        rw.put(b"1 store 1 flags \\Deleted\r\n")
        rw.wait_for_resp("1", "OK")

        rw.put(b"2 close\r\n")
        rw.wait_for_resp("2", "OK")

        rw.put(b"3 select INBOX\r\n")
        rw.wait_for_resp("3", "OK")

        rw.put(b"4 search UID %s\r\n"%uid)
        rw.wait_for_resp(
            "4",
            "OK",
            disallow=[b"\\* SEARCH [0-9]*"],
        )


@register_test
def test_no_expunge_on_logout(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as rw:
            uid = get_uid(1, rw)

            rw.put(b"1 store 1 flags \\Deleted\r\n")
            rw.wait_for_resp("1", "OK")

        with _inbox(subproc) as rw:
            rw.put(b"1 UID search UID %s\r\n"%uid)
            rw.wait_for_resp(
                "1",
                "OK",
                require=[b"\\* SEARCH %s*"%uid],
            )


@register_test
def test_noop(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as rw1, _inbox(subproc) as rw2:
            # empty flags for a few messages
            rw1.put(b"1a store 1:3 flags ()\r\n")
            rw1.wait_for_resp("1a", "OK")

            # sync to second connection
            rw2.put(b"1b NOOP\r\n")
            rw2.wait_for_resp("1b", "OK")

            # make some updates
            rw1.put(b"3a store 1 flags \\Deleted\r\n")
            rw1.wait_for_resp("3a", "OK")
            rw1.put(b"4a store 2 flags \\Answered\r\n")
            rw1.wait_for_resp("4a", "OK")
            rw1.put(b"5a store 3 flags \\Answered\r\n")
            rw1.wait_for_resp("5a", "OK")
            rw1.put(b"6a expunge\r\n")
            rw1.wait_for_resp("6a", "OK")

            # sync to second connection
            assert rw2.r.empty(), "expected empty queue before NOOP"
            rw2.put(b"2b NOOP\r\n")
            rw2.wait_for_resp(
                "2b",
                "OK",
                require=[
                    b"\\* 2 FETCH \\(FLAGS \\(\\\\Answered\\)\\)",
                    b"\\* 3 FETCH \\(FLAGS \\(\\\\Answered\\)\\)",
                    b"\\* 1 EXPUNGE"
                ],
            )


@register_test
def test_up_transition(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _session(subproc) as rw1:
            with _session(subproc) as rw2:
                # let the second connection be the primary up_t
                rw2.put(b"1b select INBOX\r\n")
                rw2.wait_for_resp("1b", "OK")

                rw1.put(b"1a select INBOX\r\n")
                rw1.wait_for_resp("1a", "OK")

                # Make sure everything is working
                rw1.put(b"2a STORE 1 flags ()\r\n")
                rw1.wait_for_resp("2a", "OK")
                rw2.put(b"2b STORE 1 flags \\Answered\r\n")
                rw2.wait_for_resp("2b", "OK")

            # Make sure the first connection still works
            rw1.put(b"3a STORE 1 flags ()\r\n")
            rw1.wait_for_resp(
                "3a",
                "OK",
                require=[b"\\* 1 FETCH \\(FLAGS \\(\\)\\)"],
            )


def do_passthru_test(rw):
    rw.put(b"1 LIST \"\" *\r\n")
    rw.wait_for_resp(
        "1",
        "OK",
        require=[b"\\* LIST \\(.*\\) \"/\" INBOX"],
    )

    rw.put(b"2 LSUB \"\" *\r\n")
    rw.wait_for_resp("2", "OK")

    rw.put(b"3 STATUS INBOX (MESSAGES)\r\n")
    rw.wait_for_resp(
        "3",
        "OK",
        require=[b"\\* STATUS INBOX \\(MESSAGES [0-9]*\\)"],
    )

    # test SUBSCRIBE and UNSUBSCRIBE
    rw.put(b"4 SUBSCRIBE INBOX\r\n")
    rw.wait_for_resp("4", "OK")

    rw.put(b"5 LSUB \"\" *\r\n")
    rw.wait_for_resp(
        "5",
        "OK",
        require=[b"\\* LSUB \\(.*\\) \"/\" INBOX"],
    )

    rw.put(b"6 UNSUBSCRIBE INBOX\r\n")
    rw.wait_for_resp("6", "OK")

    rw.put(b"7 LSUB \"\" *\r\n")
    rw.wait_for_resp(
        "7",
        "OK",
        disallow=[b"\\* LSUB \\(.*\\) \"/\" INBOX"],
    )

    # test CREATE, RENAME, and DELETE
    name = codecs.encode(b"deleteme_" + os.urandom(5), "hex_codec")
    rename = codecs.encode(b"deleteme_" + os.urandom(5), "hex_codec")

    rw.put(b"8 CREATE %s\r\n"%name)
    rw.wait_for_resp("8", "OK")

    rw.put(b"9 LIST \"\" *\r\n")
    rw.wait_for_resp(
        "9",
        "OK",
        require=[b"\\* LIST \\(.*\\) \"/\" %s"%name],
    )

    rw.put(b"10 RENAME %s %s\r\n"%(name, rename))
    rw.wait_for_resp("10", "OK")

    rw.put(b"11 DELETE %s\r\n"%rename)
    rw.wait_for_resp("11", "OK")

    rw.put(b"12 LIST \"\" *\r\n")
    rw.wait_for_resp(
        "12",
        "OK",
        disallow=[b"\\* LIST \\(.*\\) \"/\" %s"%name],
    )


@register_test
def test_passthru_unselected(cmd, maildir_root):
    with session(cmd) as rw:
        do_passthru_test(rw)


@register_test
def test_passthru_selected(cmd, maildir_root):
    with inbox(cmd) as rw:
        do_passthru_test(rw)


def get_highest_uid(mailbox, rw):
    rw.put(b"ghu1 select %s\r\n"%mailbox)
    rw.wait_for_resp("ghu1", "OK")

    uid = get_uid("*", rw)

    rw.put(b"ghu2 close\r\n")
    rw.wait_for_resp("ghu2", "OK")

    return int(uid)


@register_test
def test_append(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        # APPEND while not open
        with _session(subproc) as rw:
            uid_start = get_highest_uid(b"INBOX", rw)

            rw.put(b"1 APPEND INBOX {11}\r\n")
            rw.wait_for_match(b"\\+")
            rw.put(b"hello world\r\n")
            rw.wait_for_resp("1", "OK")

            uid_now = get_highest_uid(b"INBOX", rw)

            assert uid_now == uid_start + 1

        # APPEND while open
        with _inbox(subproc) as rw:
            rw.put(b"1 APPEND INBOX {11}\r\n")
            rw.wait_for_match(b"\\+")
            rw.put(b"hello world\r\n")
            rw.wait_for_resp("1", "OK")

            assert int(get_uid("*", rw)) == uid_start + 2

        # APPEND from either while two are open
        with _inbox(subproc) as rw1, _inbox(subproc) as rw2:
            rw1.put(b"1 APPEND INBOX {11}\r\n")
            rw1.wait_for_match(b"\\+")
            rw1.put(b"hello world\r\n")
            rw1.wait_for_resp("1", "OK")

            # Check for update with NOOP on 2
            rw2.put(b"1 NOOP\r\n")
            rw2.wait_for_resp(
                "1",
                "OK",
                require=[b"\\* [0-9]* EXISTS"],
            )

            rw2.put(b"2 APPEND INBOX {11}\r\n")
            rw2.wait_for_match(b"\\+")
            rw2.put(b"hello world\r\n")
            rw2.wait_for_resp("2", "OK")

            # Check for update with NOOP on 2
            rw1.put(b"2 NOOP\r\n")
            rw1.wait_for_resp(
                "2",
                "OK",
                require=[b"\\* [0-9]* EXISTS"],
            )

        # APPEND from an unrelated connection while open
        with _session(subproc) as rw1, _inbox(subproc) as rw2:
            rw1.put(b"1 APPEND INBOX {11}\r\n")
            rw1.wait_for_match(b"\\+")
            rw1.put(b"hello world\r\n")
            rw1.wait_for_resp("1", "OK")

            # Check for update with NOOP on 2
            rw2.put(b"2 NOOP\r\n")
            rw2.wait_for_resp(
                "2",
                "OK",
                require=[b"\\* [0-9]* EXISTS"],
            )


@register_test
def test_append_to_nonexisting(cmd, maildir_root):
    bad_path = os.path.join(maildir_root, USER, "asdf")
    assert not os.path.exists(bad_path), \
            "non-existing directory exists before APPEND"

    with Subproc(cmd) as subproc:
        with _session(subproc) as rw:
            rw.put(b"1 APPEND asdf {11}\r\n")
            rw.wait_for_match(b"\\+")
            rw.put(b"hello world\r\n")
            rw.wait_for_resp(
                "1",
                "NO",
                require=[b"1 NO \\[TRYCREATE\\]"],
            )

    assert not os.path.exists(bad_path), \
            "path to a non-existing directory exists after APPEND"


def get_msg_count(rw, box):
    rw.put(b"N STATUS %s (MESSAGES)\r\n"%box)
    matches = rw.wait_for_resp(
        "N",
        "OK",
        require=[b"\\* STATUS %s \\(MESSAGES ([0-9]*)\\)"%box],
    )
    return int(matches[0][1])


@register_test
def test_copy(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _session(subproc) as rw:
            # first count how many messages there are
            inbox_count = get_msg_count(rw, b"INBOX")
            other_count = get_msg_count(rw, b"\"Test Folder\"")

            # figure on a paritally valid range
            assert inbox_count > 2, "inbox too empty for test"
            copy_range = (inbox_count - 1, inbox_count + 1)

            rw.put(b"1 SELECT INBOX\r\n")
            rw.wait_for_resp("1", "OK")

            # COPY to other mailbox
            rw.put(b"2 COPY %d:%d \"Test Folder\"\r\n"%copy_range)
            rw.wait_for_resp("2", "OK")

            # COPY to this mailbox, with partially valid range
            rw.put(b"3 COPY %d:%d INBOX\r\n"%copy_range)
            rw.wait_for_resp(
                "3",
                "OK",
                require=[b"\\* %d EXISTS"%(inbox_count+2)],
            )

            # COPY to nonexisting mailbox
            rw.put(b"4 COPY %d:%d asdf\r\n"%copy_range)
            rw.wait_for_resp("4", "NO", require=[b"4 NO \\[TRYCREATE\\]"])

            rw.put(b"5 CLOSE\r\n")
            rw.wait_for_resp("5", "OK")

            # check counts
            got = get_msg_count(rw, b"INBOX")
            exp = inbox_count + 2
            assert exp == got, f"expected {exp} but got {got}"

            got = get_msg_count(rw, b"\"Test Folder\"")
            exp = other_count + 2
            assert exp == got, f"expected {exp} but got {got}"


@register_test
def test_examine(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _session(subproc) as rw:
            # EXAMINE from unselected
            rw.put(b"1 EXAMINE INBOX\r\n")
            rw.wait_for_resp("1", "OK")

            # SELECT from EXAMINED
            rw.put(b"2 SELECT INBOX\r\n")
            rw.wait_for_resp("2", "OK")

            # Test STORE.
            rw.put(b"3 store 1 flags \\Seen\r\n")
            rw.wait_for_resp("3", "OK")
            rw.put(b"4 fetch 1 flags\r\n")
            rw.wait_for_resp(
                "4",
                "OK",
                require=[b"\\* 1 FETCH \\(FLAGS \\(\\\\Seen\\)\\)"],
            )

            # EXAMINE from SELECTED
            rw.put(b"5 EXAMINE INBOX\r\n")
            rw.wait_for_resp("5", "OK")

        # Force one up_t to transition multiple times due to other up_t's
        with _session(subproc) as rw1, _session(subproc) as rw2:
            # EXAMINE on 1
            rw1.put(b"1a EXAMINE INBOX\r\n")
            rw1.wait_for_resp("1a", "OK")

            # SELECT on 2
            rw2.put(b"1b SELECT INBOX\r\n")
            rw2.wait_for_resp("1b", "OK")

            # Test STORE.
            rw2.put(b"2b store 1 -flags \\Seen\r\n")
            rw2.wait_for_resp("2b", "OK")

            rw2.put(b"3b fetch 1 flags\r\n")
            rw2.wait_for_resp(
                "3b",
                "OK",
                require=[b"\\* 1 FETCH \\(FLAGS \\(\\)\\)"],
            )

            # EXAMINE on 2
            rw2.put(b"4b EXAMINE INBOX\r\n")
            rw2.wait_for_resp("4b", "OK")

            # SELECT on 2
            rw2.put(b"5b SELECT INBOX\r\n")
            rw2.wait_for_resp("5b", "OK")

            # UNSELECT on 2
            rw2.put(b"6b CLOSE\r\n")
            rw2.wait_for_resp("6b", "OK")

            # Introduce another connection
            with _inbox(subproc) as rw3:
                # Test STORE.
                rw3.put(b"1c store 1 flags \\Seen\r\n")
                rw3.wait_for_resp("1c", "OK")
                rw3.put(b"2c fetch 1 flags\r\n")
                rw3.wait_for_resp(
                    "2c",
                    "OK",
                    require=[b"\\* 1 FETCH \\(FLAGS \\(\\\\Seen\\)\\)"],
                )


@register_test
def test_terminate_with_open_connection(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as rw:
            rw.wait_for_match(b"\\* OK")

            p = subproc.p
            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


@register_test
def test_terminate_with_open_session(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as rw:
            rw.wait_for_match(b"\\* OK")

            rw.put(
                b"1 login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8'))
            )
            rw.wait_for_match(b"1 OK")

            p = subproc.p
            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


@register_test
def test_terminate_with_open_mailbox(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as rw:
            rw.wait_for_match(b"\\* OK")

            rw.put(
                b"1 login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8'))
            )
            rw.wait_for_match(b"1 OK")

            rw.put(b"2 select INBOX\r\n")
            rw.wait_for_match(b"2 OK")

            p = subproc.p
            p.send_signal(signal.SIGTERM)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGTERM was not handled fast enough"


@register_test
def test_syntax_errors(cmd, maildir_root):
    with inbox(cmd) as rw:
        # incomplete command
        rw.put(b"1 ERROR\r\n")
        rw.wait_for_resp(
            "1",
            "BAD",
            require=[br".*at input: ERROR\\r\\n.*"],
        )
        # complete command then error
        rw.put(b"2 CLOSE ERROR\r\n")
        rw.wait_for_resp(
            "2",
            "BAD",
            require=[br".*at input:  ERROR\\r\\n.*"],
        )
        # total junk
        rw.put(b"(junk)\r\n")
        rw.wait_for_resp(
            "*",
            "BAD",
            require=[br".*at input: \(junk\)\\r\\n.*"],
        )
        # a response
        rw.put(b"* SEARCH\r\n")
        rw.wait_for_resp(
            "*",
            "BAD",
            require=[br".*at input: SEARCH\\r\\n.*"],
        )


@register_test
def test_idle(cmd, maildir_root):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as rw1:
            # Add a message to the inbox
            rw1.put(b"1a APPEND INBOX {11}\r\n")
            rw1.wait_for_match(b"\\+")
            rw1.put(b"hello world\r\n")
            rw1.wait_for_resp("1a", "OK")
            uid = get_uid("*", rw1)

            # Ensure that the message is already \Seen
            rw1.put(b"2a STORE %s flags \\Seen\r\n"%(uid))
            rw1.wait_for_resp("2a", "OK")

            # make a direct connection to dovecot
            with _session(None, host="127.0.0.1", port=993) as rw2:
                user = USER.encode('utf8')
                passwd = PASS.encode('utf8')
                rw2.put(b"2b SELECT INBOX\r\n")
                rw2.wait_for_resp("2b", "OK")

                # ensure that the NOOP on first connection returns nothing
                rw1.put(b"3a NOOP\r\n")
                rw1.wait_for_resp(
                    "3a",
                    "OK",
                    disallow=[br"\* .*"],
                )

                rw2.put(b"3b STORE %s flags \\Deleted\r\n"%(uid))
                rw2.wait_for_resp("3b", "OK")

                # There's currently no way to synchronize this, so we'll have
                # to just expect that it happens within a certain timeframe.
                try:
                    deadline = time.time() + 1
                    while time.time() < deadline:
                        rw1.put(b"4a NOOP\r\n")
                        rw1.wait_for_resp(
                            "4a",
                            "OK",
                            disallow=[br"\* .*"],
                        )
                except DisallowedError:
                    pass
                else:
                    raise ValueError("NOOP never showed a FLAGS response")

                rw2.put(b"4b CLOSE\r\n")
                rw2.wait_for_resp("4b", "OK")


@register_test
def prep_test_large_initial_download(cmd, maildir_root):
    with inbox(cmd) as rw:
        for i in range(10):
            tag = b"%d"%(i+1)
            copy_range = b"1:%d"%(2**i)

            rw.put(b"%s COPY %s INBOX\r\n"%(tag, copy_range))
            rw.wait_for_resp(tag, "OK")


@register_test
def test_large_initial_download(cmd, maildir_root):
    with inbox(cmd) as rw:
        pass


# delete the initial contents of the inbox
# (otherwise it grows until it slows down tests unnecessarily)
def prep_starting_inbox(cmd, maildir_root):
    with inbox(cmd) as rw:
        rw.put(b"1 store 1:* flags \\Deleted\r\n")
        rw.wait_for_resp("1", "OK")
        rw.put(b"2 expunge\r\n")
        rw.wait_for_resp("2", "OK")
        for i in range(3, 10):
            tag = str(2 + i).encode("ascii")
            rw.put(b"%s APPEND INBOX {11}\r\n"%tag)
            rw.wait_for_match(b"\\+")
            rw.put(b"hello world\r\n")
            rw.wait_for_resp(tag, "OK")


# Prepare a subdirectory
@contextlib.contextmanager
def temp_maildir_root():
    tempdir = tempfile.mkdtemp()
    try:
        yield tempdir
    finally:
        shutil.rmtree(tempdir)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(
            "usage: %s /path/to/test/files [PATTERN...]"%(sys.argv[0]),
            file=sys.stderr
        )
        sys.exit(1)

    test_files = sys.argv[1]
    patterns = [p if p.startswith("^") else ".*" + p for p in sys.argv[2:]]

    if len(patterns) > 0:
        # filter tests by patterns from command line
        tests = [
            t for t in all_tests
            if any(re.match(p, t.__name__) is not None for p in patterns)
        ]

        if len(tests) == 0:
            print("no tests match any patterns", file=sys.stderr)
            sys.exit(1)
    else:
        tests = all_tests

    for test in [prep_starting_inbox] + tests:
        print(test.__name__ + "... ", end="", flush="true")
        with temp_maildir_root() as maildir_root:
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
            try:
                test(cmd, maildir_root)
                print("PASS")
            except:
                print("FAIL")
                raise

    print("PASS")
