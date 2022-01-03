#!/usr/bin/env python3

import argparse
import codecs
import contextlib
import os
import queue
import random
import re
import selectors
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import traceback

HERE = os.path.dirname(__file__)
test_files = os.path.join(HERE, "files")

all_tests = []

def register_test(fn):
    all_tests.append(fn)
    return fn

TIMEOUT = 2.5

USER="test@splintermail.com"
PASS="passwordpassword"

# global values for the whole test
HOST=None
PORT=None

def as_bytes(msg):
    if isinstance(msg, bytes):
        return msg
    return msg.encode("utf8")

class StatusError(Exception):
    pass

class RequiredError(Exception):
    pass

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
        self.lock = threading.Lock()
        super().__init__(*arg)

    def run(self):
        try:
            while True:
                data = self.io.readline()
                if len(data) == 0:
                    break
                with self.lock:
                    self.buffer.append(data)
                self.q.put(data)
        finally:
            self.q.put(None)

    def inject_message(self, msg, end=b"\n"):
        with self.lock:
            self.buffer.append(msg + end)

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
                try:
                    data = self.io.recv(4096)
                except ConnectionResetError:
                    break
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


class TLS:
    def __init__(self, sock, modify_fn, send_fn):
        self.sock = sock
        self.modify_fn = modify_fn
        self.send_fn = send_fn

        self.out = b""
        self.handshake = False
        self.read_wants_write = False
        self.write_wants_read = False
        self.registered = None

    def advance_handshake(self, readable, writable):
        try:
            self.sock.do_handshake()
        except ssl.SSLWantReadError:
            self.modify(read=True, write=False)
        except ssl.SSLWantWriteError:
            self.modify(read=False, write=True)
        else:
            self.handshake = True
            self.modify(read=True, write=len(self.out) > 0)

    def advance_state(self, readable, writable):
        if not self.handshake:
            return self.advance_handshake(readable, writable)

        if readable or (writable and self.read_wants_write):
            try:
                data = self.sock.recv(4096)
            except ssl.SSLWantReadError:
                self.modify(read=True, write=False)
                return False
            except ssl.SSLWantWriteError:
                self.modify(read=False, write=True)
                self.read_wants_write = True
                return False
            except ConnectionResetError:
                return True

            if not data:
                return True

            self.modify(read=True, write=len(self.out) > 0)
            self.send_fn(data)

        if writable or (readable and self.write_wants_read):
            assert self.out, "nothing to write"
            try:
                written = self.sock.send(self.out)
            except ssl.SSLWantReadError:
                self.modify(read=True, write=False)
                self.write_wants_read = True
                return False
            except ssl.SSLWantWriteError:
                self.modify(read=False, write=True)
                return False

            if not written:
                return True

            self.out = self.out[written:]
            self.modify(read=True, write=len(self.out) > 0)

    def send(self, data):
        self.out += data
        if not self.handshake:
            return
        if self.read_wants_write:
            return
        if self.write_wants_read:
            return
        self.modify(read=True, write=len(self.out) > 0)

    def modify(self, read, write):
        if self.registered == (read, write):
            return
        self.registered = (read, write)

        mask = 0
        if read:
            mask |= selectors.EVENT_READ
        if write:
            mask |= selectors.EVENT_WRITE
        self.modify_fn(self.sock, mask)


def peel_line(text):
    idx = text.find(b'\n')
    if idx < 0:
        return None, text
    return text[:idx+1], text[idx+1:]


class TLSPair:
    def __init__(self, sel, local, remote):
        self.sel = sel

        self.closed = False

        self.local_buf = b""
        self.remote_buf = b""

        self.cond = threading.Condition()
        self.trap_response = None
        self.trapped = False

        self.local = TLS(local, self.modify_fn, self.local_data)
        self.remote = TLS(remote, self.modify_fn, self.remote_data)
        self.sel.register(local, selectors.EVENT_READ, self)
        self.sel.register(remote, selectors.EVENT_WRITE, self)

    def hook(self, sock, readable, writable):
        if self.closed:
            return
        if sock == self.local.sock:
            if self.local.advance_state(readable, writable):
                self.close()
        else:
            if self.remote.advance_state(readable, writable):
                self.close()

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.sel.unregister(self.local.sock)
        self.sel.unregister(self.remote.sock)
        self.local.sock.close()
        self.remote.sock.close()

    def remote_data(self, data):
        """Forward a line at a time"""
        self.local_buf += data
        while not self.trapped:
            line, self.local_buf = peel_line(self.local_buf)
            if line is None:
                break
            # Do we need to trap this response?
            if self.trap_response and re.match(self.trap_response, line):
                with self.cond:
                    self.trapped = True
                    self.cond.notify()
                self.local_buf = line + self.local_buf
                break
            self.local.send(line)

    def local_data(self, data):
        """Forward a line at a time"""
        self.remote_buf += data
        while True:
            line, self.remote_buf = peel_line(self.remote_buf)
            if line is None:
                break
            self.remote.send(line)

    def modify_fn(self, sock, mask):
        self.sel.modify(sock, mask, self)

    # event loop function
    def set_trap_response(self, pattern):
        with self.cond:
            self.trap_response = pattern
            self.cond.notify()

    # event loop function
    def release_trap(self):
        with self.cond:
            self.trapped = False
            self.trap_response = None
            self.cond.notify()
        self.remote_data(b"")

    # main thread function
    def wait_for_trap_start(self):
        with self.cond:
            while self.trap_response is None:
                self.cond.wait()

    # main thread function
    def wait_for_trap(self):
        with self.cond:
            while not self.trapped:
                self.cond.wait()

    # main thread function
    def wait_for_trap_end(self):
        with self.cond:
            while self.trapped:
                self.cond.wait()


class TLSSocketIntercept(threading.Thread):
    def __init__(self, cmd):
        self.closed = False

        self.pairs = []

        self.quitting = False
        self.started = False

        # configure command and gather host/port info
        self.port = random.randint(32768, 60999)
        self.cmd = [*cmd]
        if "--remote-host" in cmd:
            host_idx = cmd.index("--remote-host") + 1
            self.remote_host = cmd[host_idx]
            self.cmd[host_idx] = "127.0.0.1"
        else:
            self.remote_host = "127.0.0.1"
            cmd += ["--remote-host", "127.0.0.1"]
        if "--remote-port" in cmd:
            port_idx = cmd.index("--remote-port") + 1
            self.remote_port = int(cmd[port_idx])
            self.cmd[port_idx] = str(self.port)
        else:
            self.remote_port = 993
            cmd += ["--remote-port", str(self.port)]

        # create a listener
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", self.port))
        self.listener.listen(5)

        # create a control connection (must be TCP so select works on windows)
        self.ctl_w = socket.socket()
        self.ctl_w.connect(("127.0.0.1", self.port))

        # accept the other side of the control connection
        self.ctl_r, _ = self.listener.accept()
        self.ctl_r.setblocking(False)

        # configure event loop
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.ctl_r, selectors.EVENT_READ)
        self.listener.setblocking(False)
        self.sel.register(self.listener, selectors.EVENT_READ)

        super().__init__()

    def ctl_ready(self):
        msg = self.ctl_r.recv(4096)
        if msg == b"quit":
            self.quitting = True
        if msg.startswith(b"trap:"):
            pattern = msg[5:]
            self.pairs[0].set_trap_response(pattern)
        if msg == b"untrap":
            self.pairs[0].release_trap()

    def listener_ready(self):
        local, _ = self.listener.accept()
        local.setblocking(False)

        # connect to remote
        remote = socket.socket()
        remote.connect((self.remote_host, self.remote_port))
        remote.setblocking(False)

        local = server_context().wrap_socket(
            local, server_side=True, do_handshake_on_connect=False,
        )

        remote = client_context().wrap_socket(
            remote,
            server_hostname=self.remote_host,
            do_handshake_on_connect=False,
        )

        pair = TLSPair(self.sel, local, remote)
        self.pairs.append(pair)

    def run(self):
        while not self.quitting:
            events = self.sel.select()
            for key, mask in events:
                readable = mask & selectors.EVENT_READ
                writable = mask & selectors.EVENT_WRITE
                if key.fileobj == self.listener:
                    self.listener_ready()
                    continue
                if key.fileobj == self.ctl_r:
                    self.ctl_ready()
                    continue
                # all other objects should register with their pair
                assert isinstance(key.data, TLSPair)
                key.data.hook(key.fileobj, readable, writable)

    @contextlib.contextmanager
    def trap_response(self, pattern):
        """Meant to be called from the main thread"""
        assert self.pairs, "nothing intercepted yet"
        pair = self.pairs[0]

        # notify the event loop of the trap
        self.ctl_w.send(b"trap:" + pattern)

        # wait for the event loop to acknowledge
        pair.wait_for_trap_start()

        try:
            yield pair.wait_for_trap
        finally:
            self.ctl_w.send(b"untrap")
            pair.wait_for_trap_end()

    def __enter__(self):
        if not self.started:
            self.started = True
            self.start()
        return self

    def __exit__(self, *arg):
        self.close()

    def close(self):
        # To be called from the main thread only

        if self.closed:
            return
        self.closed = True

        self.quitting = True
        if self.started:
            self.ctl_w.send(b"quit")
            self.join()

        self.sel.unregister(self.listener)
        self.listener.close()

        self.sel.unregister(self.ctl_r)
        self.ctl_r.close()
        self.ctl_w.close()

        for pair in self.pairs:
            pair.close()

        self.sel.close()


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
                    raise StatusError(f"needed {status}, got: {line}")
                break
        # return the first match of every required pattern
        req_matches = []
        # requires should be required in order
        lines = iter(recvd)
        for req in require:
            for line in lines:
                match = re.match(req, line)
                if match is not None:
                    req_matches.append(match)
                    break
            else:
                raise RequiredError(f"required pattern ({req}) not found")
        # return all matches, and all the content but the tagged status line
        return req_matches, b"".join(recvd[:-1])


_client_context = None

def client_context(cafile=None):
    global _client_context

    if _client_context is not None:
        return _client_context

    _client_context = ssl.create_default_context()
    if cafile is not None:
        _client_context.load_verify_locations(cafile=cafile)
    # on some OS's ssl module loads certs automatically
    if len(_client_context.get_ca_certs()) == 0:
        # on others it doesn't
        import certifi
        _client_context.load_verify_locations(cafile=certifi.where())
    # always manually include our test certs
    _client_context.load_verify_locations(
        cafile=os.path.join(test_files, "ssl", "good.crt")
    )

    # always load our self-signed test CA
    _client_context.load_verify_locations(
        os.path.join(test_files, "ssl", "good.crt")
    )

    return _client_context


_server_context = None

def server_context(cert=None, key=None):
    global _server_context

    if _server_context is not None:
        return _server_context

    assert cert is not None and key is not None, \
        "server_context must be initialized with a cert and key"

    _server_context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    _server_context.load_cert_chain(certfile=cert, keyfile=key)

    return _server_context


@contextlib.contextmanager
def run_connection(closable, host=None, port=None):
    host = host or "127.0.0.1"
    port = port or 2993
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
        if b"listener ready" in line:
            break
        print('\x1b[31mmessage is:', line, '\x1b[m')


# SIGINT is weird in windows.  When we send it to our subproces, we also get it
# ourselves some time later.  Just count expected ones and raise an error if
# we detect the user sent one to us.
if sys.platform == "win32":

    _expect_sigints = 0
    _recvd_sigints = 0

    _old_sigint_handler = None

    def _handle_sigint(*arg):
        global _recvd_sigints
        _recvd_sigints += 1
        if _recvd_sigints > _expect_sigints:
            _old_sigint_handler(*arg)

    _old_sigint_handler = signal.signal(signal.SIGINT, _handle_sigint)

    def send_sigint(p):
        global _expect_sigints
        _expect_sigints += 1
        p.send_signal(signal.CTRL_C_EVENT)

else:

    def send_sigint(p):
        p.send_signal(signal.SIGINT)


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

    def inject_message(self, msg, end=b"\n"):
        self.reader.inject_message(msg, end)

    def close(self):
        if not self.started:
            return

        if self.closed:
            return
        self.closed = True

        try:
            send_sigint(self.p)

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
        rw.wait_for_resp("A", "OK")

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
def temp_box(rw):
    name = b"deleteme_" + codecs.encode(os.urandom(5), "hex_codec")
    rw.put(b"B1 create %s\r\n"%name)
    rw.wait_for_resp("B1", "OK")

    yield name

    # don't bother cleaning up in exception cases;
    # there are some exceptions where you couldn't anyway.
    rw.put(b"B2 delete %s\r\n"%name)
    rw.wait_for_resp("B2", "OK")


@contextlib.contextmanager
def _inbox(closable):
    with _session(closable) as rw:
        rw.put(b"B select INBOX\r\n")
        rw.wait_for_match(b"B OK")

        yield rw

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
def test_start_kill(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        pass


@register_test
def test_login_logout(cmd, maildir_root, **kwargs):
    with session(cmd) as rw:
        pass


@register_test
def test_select_logout(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        pass


@register_test
def test_select_reselect(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        rw.put(b"1 select INBOX\r\n")
        rw.wait_for_resp("1", "OK")


@register_test
def test_select_close(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        rw.put(b"1 close\r\n")
        rw.wait_for_resp("1", "OK")


@register_test
def test_select_select(cmd, maildir_root, **kwargs):
    with session(cmd) as rw:
        with temp_box(rw) as folder:
            rw.put(b"1 select INBOX\r\n")
            rw.wait_for_resp("1", "OK")
            rw.put(b"2 select %s\r\n"%folder)
            rw.wait_for_resp("2", "OK")
            rw.put(b"3 close\r\n")
            rw.wait_for_resp("3", "OK")
            # TODO: remove this and still pass the test
            # underlying bug is that an up_t is not being disconnected
            rw.put(b"4 select INBOX\r\n")
            rw.wait_for_resp("4", "OK")


def inject_local_msg(maildir_root, mbx="INBOX"):
    cmd = [
        "./inject_local_msg",
        "--root",
        maildir_root,
        "--user",
        USER,
        "--mailbox",
        mbx,
    ]
    msg = b"hello world!\r\n"
    p = subprocess.run(cmd, input=msg, stdout=subprocess.PIPE, check=True)
    return int(p.stdout.strip())


def get_seq_num(uid, rw):
    rw.put(b"S UID fetch %d UID\r\n"%uid)
    matches, _ = rw.wait_for_resp(
        "S",
        "OK",
        require=[b"\\* ([0-9]*) FETCH.*"],
    )
    return matches[0][1]

def get_uid(seq_num, rw):
    rw.put(b"U fetch %s UID\r\n"%str(seq_num).encode("ascii"))
    matches, _ = rw.wait_for_resp(
        "U",
        "OK",
        require=[b"\\* [0-9]* FETCH \\(UID ([0-9]*)\\)"],
    )
    return matches[0][1]


@register_test
def test_store(cmd, maildir_root, **kwargs):
    def require_line(uid, flag_str):
        flags = []
        if "s" in flag_str:
            flags.append(b"\\\\Seen")
        if "a" in flag_str:
            flags.append(b"\\\\Answered")
        if "d" in flag_str:
            flags.append(b"\\\\Draft")
        return b"".join(
            [
                b"\\* [0-9]+ FETCH \\(FLAGS \\(",
                b" ".join(flags),
                b"\\) UID %d\\)"%uid
            ]
        )

    def assert_flags(rw, tag, state):
        uids = list(state.keys())
        rw.put(b"%s uid fetch %d:%d flags\r\n"%(tag, min(uids), max(uids)))
        rw.wait_for_resp(
            tag,
            "OK",
            require=[require_line(k, v) for k, v in state.items()],
        )

    # inject a couple local uids
    l1 = inject_local_msg(maildir_root)
    l2 = inject_local_msg(maildir_root)

    with Subproc(cmd) as subproc, \
            _inbox(subproc) as rw1, \
            _inbox(subproc) as rw2:
        # inject a couple of uids_up
        append_messages(rw1, 2)

        u2 = int(get_uid("*", rw1))
        u1 = u2 - 1

        # make sure we counted right; we expect l1 < l2 < u1 < u2
        assert int(u1) == l2 + 1, f"u1 != l2+1 ({int(u1)}!={l2+1})"

        ## Part 1: mix/match storing uids_up and uids_local
        ## Test various parts of dn_t::store_cmd().

        # pure uid_up case
        rw1.put(b"2 uid store %d:%d flags \\Seen\r\n"%(u1, u2))
        rw1.wait_for_resp("2", "OK")
        assert_flags(rw1, b"3", {l1: "", l2: "", u1: "s", u2: "s"})

        # pure uid_local case
        rw1.put(b"5 uid store %d:%d +flags \\Answered\r\n"%(l1, l2))
        rw1.wait_for_resp("5", "OK")
        assert_flags(rw1, b"6", {l1: "a", l2: "a", u1: "s", u2: "s"})

        # mixed uid_up/uid_local case
        rw1.put(b"7 uid store %d:%d -flags \\Seen\r\n"%(l1, u2))
        rw1.wait_for_resp("7", "OK")
        assert_flags(rw1, b"8", {l1: "a", l2: "a", u1: "", u2: ""})

        rw1.put(b"7 uid store %d:%d flags \\Draft\r\n"%(l1, u2))
        rw1.wait_for_resp("7", "OK")
        assert_flags(rw1, b"8", {l1: "d", l2: "d", u1: "d", u2: "d"})

        rw1.put(b"8 store 4294967294 flags \\Seen\r\n")
        rw1.wait_for_resp(
            "8",
            "OK",
            require=[b"8 OK noop STORE"],
            disallow=[b"\\* [0-9]* FLAGS"],
        )

        ## Part 2: mix/match .SILENT and external updates.
        ## Test various parts of dn_t::send_store_fetch_resps().
        rw2.put(b"9 NOOP\r\n")
        rw2.wait_for_resp("9", "OK")

        rw2.put(b"10 uid store %d flags \\Answered\r\n"%u1)
        rw2.wait_for_resp("10", "OK")
        rw1.put(b"11 uid store %d flags \\Seen\r\n"%u2)
        rw1.wait_for_resp(
            "11",
            "OK",
            require=[
                # external updates also appear (send_store_resp_noexp)
                b".*FETCH.*FLAGS \\(\\\\Answered\\) UID %d"%u1,
                # non-SILENT stores return their flag updates
                # (send_store_resp_expupdate, msg_flags_eq() == true, !silent)
                b".*FETCH.*FLAGS \\(\\\\Seen\\) UID %d"%u2,
            ],
        )

        rw1.put(b"12 uid store %d flags.silent ()\r\n"%u1)
        rw1.wait_for_resp(
            "12",
            "OK",
            # SILENT stores return no FETCH.
            # (send_store_resp_expupdate, msg_flags_eq() == true, silent)
            disallow=[b".*FETCH.*"],
        )

        rw2.put(b"13 uid store %d flags \\Answered\r\n"%u1)
        rw2.wait_for_resp("13", "OK")
        rw1.put(b"13 uid store %d +flags.silent \\Seen\r\n"%u1)
        rw1.wait_for_resp(
            "13",
            "OK",
            # SILENT stores do return a FETCH when the result was unexpected.
            # (send_store_resp_expupdate, msg_flags_eq() == false)
            require=[b".*FETCH.*FLAGS \\(\\\\Answered \\\\Seen\\) UID %d"%u1],
        )

        rw1.put(b"14 uid store %d flags (\\Answered \\Seen)\r\n"%u1)
        rw1.wait_for_resp(
            "14",
            "OK",
            # Expected non-silent non-change, we still report it
            # send_store_resp_noupdate, msg_flags_eq() == true, !silent
            require=[b".*FETCH.*FLAGS \\(\\\\Answered \\\\Seen\\) UID %d"%u1],
        )

        rw1.put(b"15 uid store %d flags.silent (\\Answered \\Seen)\r\n"%u1)
        rw1.wait_for_resp(
            "15",
            "OK",
            # Expected silent non-change, we don't report it
            # send_store_resp_noupdate, msg_flags_eq() == true, silent
            disallow=[b".*FETCH.*"],
        )

        # note: the (send_store_resp_noupdate, msg_flags_eq() == false) case
        # is practically impossible to test, maybe impossible to occur.


@register_test
def test_expunge(cmd, maildir_root, **kwargs):
    def assert_expunged(rw, tag, uid):
        rw.put(b"%s search UID %d\r\n"%(tag, u1))
        rw.wait_for_resp(
            tag,
            "OK",
            disallow=[b"\\* SEARCH [0-9]*"],
        )

    # the 'pure uid_up' case
    with inbox(cmd) as rw:
        # make sure there are at least two messages present
        append_messages(rw, 2)

        u2 = int(get_uid("*", rw))
        u1 = u2 - 1
        seq_base = int(get_seq_num(u1, rw))

        # expunge two messages and confirm they report back in reverse order
        rw.put(b"1 store %d:%d flags \\Deleted\r\n"%(seq_base, seq_base+1))
        rw.wait_for_resp("1", "OK")

        rw.put(b"2 expunge\r\n")
        rw.wait_for_resp(
            "2",
            "OK",
            require=[
                b"\\* %d EXPUNGE"%(seq_base+1),
                b"\\* %d EXPUNGE"%seq_base,
            ],
        )

        assert_expunged(rw, b"3", u1)
        assert_expunged(rw, b"4", u2)

    # inject a few local uids
    l1 = inject_local_msg(maildir_root)
    l2 = inject_local_msg(maildir_root)
    l3 = inject_local_msg(maildir_root)
    l4 = inject_local_msg(maildir_root)

    # the 'pure uid_local' case
    with inbox(cmd) as rw:
        # expunge two messages and confirm they report back in reverse order
        rw.put(b"1 uid store %d:%d flags \\Deleted\r\n"%(l1, l2))
        rw.wait_for_resp("1", "OK")

        rw.put(b"2 expunge\r\n")
        rw.wait_for_resp(
            "2",
            "OK",
            require=[
                b"\\* %d EXPUNGE"%(seq_base+1),
                b"\\* %d EXPUNGE"%seq_base,
            ],
        )

        assert_expunged(rw, b"3", l1)
        assert_expunged(rw, b"4", l2)

    # the "mixed uid_up and uid_local" case
    with inbox(cmd) as rw:
        # make sure there are at least two uid_up messages present
        append_messages(rw, 2)

        u2 = int(get_uid('*', rw))
        u1 = u2 - 1

        # make sure we counted right; we expect l3 -> l4 -> u1 -> u2
        assert int(u1) == l4 + 1, f"u1 != l4+1 ({int(u1)}!={l4+1})"

        # expunge two messages and confirm they report back in reverse order
        rw.put(b"1 uid store %d:%d flags \\Deleted\r\n"%(l3, u2))
        rw.wait_for_resp("1", "OK")

        rw.put(b"2 expunge\r\n")
        rw.wait_for_resp(
            "2",
            "OK",
            require=[
                b"\\* %d EXPUNGE"%(seq_base+3),
                b"\\* %d EXPUNGE"%(seq_base+2),
                b"\\* %d EXPUNGE"%(seq_base+1),
                b"\\* %d EXPUNGE"%seq_base,
            ],
        )

        assert_expunged(rw, b"3", l3)
        assert_expunged(rw, b"4", l4)
        assert_expunged(rw, b"5", u1)
        assert_expunged(rw, b"6", u2)


@register_test
def test_expunge_on_close(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        # make sure there is at least one message present
        append_messages(rw, 1)

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
def test_no_expunge_on_logout(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as rw:
            # make sure there is at least one message present
            append_messages(rw, 1)

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
def test_store_and_fetch_after_expunged(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as rw1:
            # create a message we'll keep and one we'll expunge
            append_messages(rw1, 2)
            delete_id = get_msg_count(rw1, b"INBOX")
            keep_id = delete_id - 1

            # let session 2 take a snapshot of inbox
            with _inbox(subproc) as rw2:
                # delete from session 1
                rw1.put(b"1 store %d flags \\Deleted\r\n"%delete_id)
                rw1.wait_for_resp("1", "OK")

                rw1.put(b"2 expunge\r\n")
                rw1.wait_for_resp(
                    "2", "OK", require=[b"\\* %d EXPUNGE"%delete_id]
                )

                # store from session 2
                rw2.put(b"1 store %d,%d flags \\Seen\r\n"%(delete_id, keep_id))
                rw2.wait_for_resp(
                    "1",
                    "OK",
                    require=[
                        b"\\* %d FETCH \\(FLAGS \\(\\\\Seen\\)\\)"%keep_id,
                        b"\\* %d FETCH \\(FLAGS \\(\\\\Seen\\)\\)"%delete_id,
                    ]
                )

                # fetch deleted body from session 2
                rw2.put(b"2 fetch %d body[]\r\n"%(delete_id))
                rw2.wait_for_resp(
                    "2",
                    "OK",
                    require=[
                        b'\\* %d FETCH \\(BODY\\[\\] "hello world"\\)'%(
                            delete_id,
                        )
                    ]
                )


@register_test
def test_noop(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        with _inbox(subproc) as rw1, _inbox(subproc) as rw2:
            # make sure there are at least three messages present
            append_messages(rw1, 3)

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
def test_up_transition(cmd, maildir_root, **kwargs):
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
    name = b"deleteme_" + codecs.encode(os.urandom(5), "hex_codec")
    rename = b"deleteme_" + codecs.encode(os.urandom(5), "hex_codec")

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
def test_passthru_unselected(cmd, maildir_root, **kwargs):
    with session(cmd) as rw:
        do_passthru_test(rw)


@register_test
def test_passthru_selected(cmd, maildir_root, **kwargs):
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
def test_append(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        # APPEND while not open
        with _session(subproc) as rw:
            append_messages(rw, 1)
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
            # require that APPEND-while-selected returns an EXISTS response
            rw.wait_for_resp("1", "OK", require=[b"\\* [0-9]+ EXISTS"])

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
                require=[b"\\* [0-9]+ EXISTS"],
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
                require=[b"\\* [0-9]+ EXISTS"],
            )

        # APPEND from an unrelated citm connection while open
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

        # APPEND from a totally unrelated connection, not passing through citm
        with _inbox(subproc) as rw:
            with _session(
                None, host="127.0.0.1", port=kwargs["imaps_port"]
            ) as rwx:
                # APPEND from the unrelated connection
                append_messages(rwx, 1)

            # append a non-local message to guarantee STORE commands will cause
            # relays to the mail server
            nonlocal_uid = uid_start

            # The up_t should require 3 round-trip relays to guarantee that the
            # new message is downloaded and given to the dn_t:
            # - after 1, the EXISTS must have been seen
            # - after 2, the detection fetch is sent and received, because we
            #            know that the detection fetch is sent before another
            #            relay is sent (and therefore the response is back
            #            before the relay response is back)
            # - after 3, the content fetch is sent and recieved as well, but
            #            the update_t with the new message may not be included
            #            by the dn_t yet.
            # - now a NOOP should certainly show the EXISTS response if it
            #   hasn't arrived earlier

            # use LIST to count round trips
            for i in range(3):
                tag = b"x%d"%i
                rw.put(b"%s LIST \"\" *\r\n"%tag)
                try:
                    rw.wait_for_resp(
                        tag, "OK", require=[b"\\* [0-9]* EXISTS"],
                    )
                    break
                except RequiredError:
                    pass
            else:
                # if we haven't seen the EXISTS yet, a noop shall surely work
                rw.put(b"2 NOOP\r\n")
                rw.wait_for_resp("2", "OK", require=[b"\\* [0-9]* EXISTS"])

            # Append from the direct connection again, this time with a timer
            # instead of round-trips to test that the up_t's IDLE works
            with _session(
                None, host="127.0.0.1", port=kwargs["imaps_port"]
            ) as rwx:
                append_messages(rwx, 1)

            # allow up to 3 seconds
            for i in range(2):
                time.sleep(1)
                tag = b"y%d"%i
                rw.put(b"%s NOOP\r\n"%tag)
                try:
                    rw.wait_for_resp(tag, "OK", require=[b"\\* [0-9]* EXISTS"])
                    break
                except RequiredError:
                    pass
            else:
                time.sleep(1)
                rw.put(b"2 NOOP\r\n")
                rw.wait_for_resp("2", "OK", require=[b"\\* [0-9]* EXISTS"])


@register_test
def test_append_to_nonexisting(cmd, maildir_root, **kwargs):
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
    matches, _ = rw.wait_for_resp(
        "N",
        "OK",
        require=[b"\\* STATUS %s \\(MESSAGES ([0-9]*)\\)"%box],
    )
    return int(matches[0][1])


@register_test
def test_copy(cmd, maildir_root, **kwargs):
    # sync the inbox before injecting uids (so the local messages are appended)
    with inbox(cmd):
        pass

    # inject a couple local uids
    l1 = inject_local_msg(maildir_root)
    l2 = inject_local_msg(maildir_root)

    with session(cmd) as rw:
        # pure uid_up cases
        with temp_box(rw) as folder:
            # first count how many messages there are
            inbox_count = get_msg_count(rw, b"INBOX")

            rw.put(b"1 SELECT INBOX\r\n")
            rw.wait_for_resp("1", "OK")

            # add a couple uid_up messages
            append_messages(rw, 2)
            inbox_count += 2

            u2 = int(get_uid("*", rw))
            u1 = u2 - 1

            # make sure we counted right; we expect l1 -> l2 -> u1 -> u2
            assert int(u1) == l2 + 1, f"u1 != l2+1 ({int(u1)}!={l2+1})"

            # figure on a partially valid range
            copy_range = (inbox_count - 1, inbox_count + 1)

            # COPY to other mailbox
            rw.put(b"2 COPY %d:%d %s\r\n"%(*copy_range, folder))
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
            assert got == exp, f"expected {exp} but got {got}"

            got = get_msg_count(rw, folder)
            exp = 2
            assert got == exp, f"expected {exp} but got {got}"

        # pure uid_local
        with temp_box(rw) as folder:
            rw.put(b"1 SELECT INBOX\r\n")
            rw.wait_for_resp("1", "OK")

            copy_range = (l1, l2)

            # COPY to other mailbox
            rw.put(b"2 UID COPY %d:%d %s\r\n"%(*copy_range, folder))
            rw.wait_for_resp("2", "OK")

            # COPY to this mailbox
            rw.put(b"3 UID COPY %d:%d INBOX\r\n"%copy_range)
            rw.wait_for_resp(
                "3",
                "OK",
                require=[b"\\* %d EXISTS"%(inbox_count+4)],
            )

            # TODO: fix the automatic mailbox creation on local COPY
            # # COPY to nonexisting mailbox
            # rw.put(b"4 UID COPY %d:%d asdf\r\n"%copy_range)
            # rw.wait_for_resp("4", "NO", require=[b"4 NO \\[TRYCREATE\\]"])

            rw.put(b"5 CLOSE\r\n")
            rw.wait_for_resp("5", "OK")

            # check counts
            got = get_msg_count(rw, b"INBOX")
            exp = inbox_count + 4
            assert got == exp, f"expected {exp} but got {got}"

            got = get_msg_count(rw, folder)
            exp = 2
            assert got == exp, f"expected {exp} but got {got}"

        # mixed uid_up/uid_local
        with temp_box(rw) as folder:
            rw.put(b"1 SELECT INBOX\r\n")
            rw.wait_for_resp("1", "OK")

            copy_range = (l1, u2)

            # COPY to other mailbox
            rw.put(b"2 UID COPY %d:%d %s\r\n"%(*copy_range, folder))
            rw.wait_for_resp("2", "OK")

            # COPY to this mailbox
            rw.put(b"3 UID COPY %d:%d INBOX\r\n"%copy_range)
            rw.wait_for_resp(
                "3",
                "OK",
                require=[b"\\* %d EXISTS"%(inbox_count+8)],
            )

            # COPY to nonexisting mailbox
            rw.put(b"4 UID COPY %d:%d asdf\r\n"%copy_range)
            rw.wait_for_resp("4", "NO", require=[b"4 NO \\[TRYCREATE\\]"])

            rw.put(b"5 CLOSE\r\n")
            rw.wait_for_resp("5", "OK")

            # check counts
            got = get_msg_count(rw, b"INBOX")
            exp = inbox_count + 8
            assert got == exp, f"expected {exp} but got {got}"

            got = get_msg_count(rw, folder)
            exp = 2
            assert got == exp, f"expected {exp} but got {got}"


@register_test
def test_examine(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        with _session(subproc) as rw:
            append_messages(rw, 1)

            # EXAMINE from unselected
            rw.put(b"1 EXAMINE INBOX\r\n")
            rw.wait_for_resp("1", "OK [READ-ONLY]")

            # SELECT from EXAMINED
            rw.put(b"2 SELECT INBOX\r\n")
            rw.wait_for_resp("2", "OK [READ-WRITE]")

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
def test_terminate_with_open_connection(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as rw:
            rw.wait_for_match(b"\\* OK")

            p = subproc.p
            send_sigint(p)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGINT was not handled fast enough"


@register_test
def test_terminate_with_open_session(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as rw:
            rw.wait_for_match(b"\\* OK")

            rw.put(
                b"1 login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8'))
            )
            rw.wait_for_resp("1", "OK")

            p = subproc.p
            send_sigint(p)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGINT was not handled fast enough"


@register_test
def test_terminate_with_open_mailbox(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc:
        with run_connection(subproc) as rw:
            rw.wait_for_match(b"\\* OK")

            rw.put(
                b"1 login %s %s\r\n"%(USER.encode('utf8'), PASS.encode('utf8'))
            )
            rw.wait_for_resp("1", "OK")

            rw.put(b"2 select INBOX\r\n")
            rw.wait_for_match(b"2 OK")

            p = subproc.p
            send_sigint(p)
            p.wait(TIMEOUT)
            assert p.poll() is not None, "SIGINT was not handled fast enough"


@register_test
def test_syntax_errors(cmd, maildir_root, **kwargs):
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
def test_idle(cmd, maildir_root, **kwargs):
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
            dovecot_port = kwargs["imaps_port"]
            with _session(None, host="127.0.0.1", port=dovecot_port) as rw2:
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
def test_intercept(cmd, maildir_root, **kwargs):
    with TLSSocketIntercept(cmd) as intercept:
        with Subproc(intercept.cmd) as subproc:
            with _session(subproc) as rw:
                with _session(subproc) as rw:
                    with _session(subproc) as rw:
                        pass


@register_test
def test_imaildir_hold(cmd, maildir_root, **kwargs):
    with TLSSocketIntercept(cmd) as intercept:
        with Subproc(intercept.cmd) as subproc:
            # start hold before opening mailbox
            with _session(subproc) as rw1, _session(subproc) as rw2:
                # The first connection will be frozen to trap the inbox in a
                # hold state.
                rw1.put(b"1 APPEND INBOX {11}\r\n")
                rw1.wait_for_match(b"\\+")

                # The second connection will have the inbox open, but will not
                # be able to see an EXISTS message since we can't downlad
                rw2.put(b"1 SELECT INBOX\r\n")
                rw2.wait_for_resp("1", "OK")
                rw2.put(b"2 NOOP\r\n")
                rw2.wait_for_resp("2", "OK")

                with intercept.trap_response(b"^[^*]+ OK.*") as wait:
                    # finish setting up the hold
                    rw1.put(b"hello world\r\n")
                    wait()

                    # force some synchronization with the mail server but
                    # expect to not see the * EXISTS message locally yet
                    exists_pattern = br"\* [0-9]* EXIST"

                    rw2.put(b"3 store 1 flags \\Seen\r\n")
                    rw2.wait_for_resp("3", "OK", disallow=[exists_pattern])

                # end the hold
                rw1.wait_for_resp("1", "OK")

                # expect the EXISTS
                rw2.put(b"5 NOOP\r\n")
                rw2.wait_for_resp("5", "OK", require=[exists_pattern])


@register_test
def test_initial_deletions(cmd, maildir_root, **kwargs):
    inbox_path = os.path.join(maildir_root, USER, "mail", "INBOX", "cur")
    with Subproc(cmd) as subproc:
        # initial sync
        with _inbox(subproc):
            pass

        # initial count, server side
        with _session(subproc) as rw:
            msgs = get_msg_count(rw, b"INBOX")
            assert msgs > 0

        # initial count, file side
        names = os.listdir(inbox_path)
        assert len(names) == msgs

        # delete files
        for name in names:
            os.remove(os.path.join(inbox_path, name))

        # re-sync
        with _inbox(subproc):
            pass

        # initial count, server side
        with _session(subproc) as rw:
            msgs_now = get_msg_count(rw, b"INBOX")
            assert msgs_now == 0, f"still have {msgs_now} of {msgs} messages"


@register_test
def prep_test_large_initial_download(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        for i in range(10):
            tag = b"%d"%(i+1)
            copy_range = b"1:%d"%(2**i)

            rw.put(b"%s COPY %s INBOX\r\n"%(tag, copy_range))
            rw.wait_for_resp(tag, "OK")


@register_test
def test_large_initial_download(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        pass


@register_test
def test_mangling(cmd, maildir_root, **kwargs):
    # Dovecot mangles APPENDED messages to use \r\n instead of \n, so there's
    # no e2e way to test the non-\r\n message mangling

    # unencryped messages get 'NOT ENCRYPTED: ' prepended to their subject
    unenc = (
        b"To: you\r\n"
        b"From: me\r\n"
        b"Subject: top secret\r\n"
        b"\r\n"
        b"Hey!\r\n"
        b"\r\n"
        b"Let's meet at the place later.\r\n"
    )
    unenc_exp = (
        b"To: you\r\n"
        b"From: me\r\n"
        b"Subject: NOT ENCRYPTED: top secret\r\n"
        b"\r\n"
        b"Hey!\r\n"
        b"\r\n"
        b"Let's meet at the place later.\r\n"
    )

    # unencryped messages with no subject get a whole new subject
    nosubj = (
        b"To: you\r\n"
        b"From: me\r\n"
        b"\r\n"
        b"Hey!\r\n"
        b"\r\n"
        b"Let's meet at the place later.\r\n"
    )
    nosubj_exp = (
        b"To: you\r\n"
        b"From: me\r\n"
        b"Subject: NOT ENCRYPTED: (no subject)\r\n"
        b"\r\n"
        b"Hey!\r\n"
        b"\r\n"
        b"Let's meet at the place later.\r\n"
    )

    # regression test: handle the case with a subject but not '\nSubject:'
    subjfirst = (
        b"Subject: hello\r\n"
        b"\r\n"
        b"world\r\n"
    )
    subjfirst_exp = (
        b"Subject: NOT ENCRYPTED: hello\r\n"
        b"\r\n"
        b"world\r\n"
    )

    # we pass broken, unencrypted messages through untouched
    broken = (
        b"Hey!\r\n"
        b"Let's meet at the place later.\r\n"
    )
    broken_exp = broken

    # messages that appear encrypted but can't be decrypted get a whole header
    # (E_PARAM error; message can't be parsed)
    corrupted_hdr = (
        b"From: CITM <citm@localhost>\r\n"
        b"To: Local User <email_user@localhost>\r\n"
        b"Date: ..., .. ... .... ........ .....\r\n"
        b"Subject: CITM failed to decrypt message\r\n"
        b"\r\n"
        b"The following message appears to be corrupted"
        b" and cannot be decrypted:\r\n"
        b"\r\n"
    )

    noparse = (
        b"-----BEGIN SPLINTERMAIL MESSAGE-----\r\n"
        b"-----END SPLINTERMAIL MESSAGE-----\r\n"
    )

    noparse_exp = (
        corrupted_hdr +
        b"-----BEGIN SPLINTERMAIL MESSAGE-----\r\n"
        b"-----END SPLINTERMAIL MESSAGE-----\r\n"
    )

    # messages that appear encrypted but are invalid (E_PARAM)
    # (delayed because the right keys get generated later)
    def make_enc():
        cmd = [
            "./encrypt_msg",
            os.path.join(maildir_root, USER, "keys", "mykey.pem"),
        ]
        p = subprocess.run(
            cmd, input=unenc, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        assert p.returncode == 0, f"encrypt_msg failed: {p.stderr}"

        # convert to \r\n-endings
        enc = p.stdout.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")

        # mess up the checksum
        if enc[-45] == b"A":
            enc = enc[:-45] + b"a" + enc[-45 + 1:]
        else:
            enc = enc[:-45] + b"A" + enc[-45 + 1:]

        enc_exp = corrupted_hdr + enc

        return enc, enc_exp

    with Subproc(cmd) as subproc:
        # initial sync
        with _inbox(subproc):
            pass

        enc, enc_exp = make_enc()

        # initial msg count
        with _session(subproc) as rw:
            msgs = get_msg_count(rw, b"INBOX")

        # inject messages directly to dovecot
        dovecot_port = kwargs["imaps_port"]
        with _session(None, host="127.0.0.1", port=dovecot_port) as rw:
            for i, msg in enumerate(
                [unenc, nosubj, broken, noparse, enc, subjfirst]
            ):
                rw.put(b"A%d APPEND INBOX {%d}\r\n"%(i, len(msg)))
                rw.wait_for_match(b"\\+")
                rw.put(msg + b"\r\n")
                _, recvd = rw.wait_for_resp(b"A%d"%i, "OK")

        # resync
        with _inbox(subproc):
            pass

        # count again
        with _session(subproc) as rw:
            new = get_msg_count(rw, b"INBOX") - msgs
            assert (new) == 6, f"expected 6 new messages, got {(new)}"

        # check contents
        with _inbox(subproc) as rw:
            for i, exp in enumerate(
                [
                    unenc_exp,
                    nosubj_exp,
                    broken_exp,
                    noparse_exp,
                    enc_exp,
                    subjfirst_exp,
                ]
            ):
                # fetch the i'th new message and compare it to what we expect
                seq = msgs + 1 + i
                rw.put(b"%d fetch %d RFC822\r\n"%(i, seq))
                _, recvd = rw.wait_for_resp(b"%d"%i, "OK")
                exp_resp = b"* %d FETCH (RFC822 {%d}\r\n%s)"%(
                    seq, len(exp), exp
                )
                exp_pat = (
                    exp_resp.replace(b"(", b"\\(")
                    .replace(b")", b"\\)")
                    .replace(b"{", b"\\{")
                    .replace(b"}", b"\\}")
                    .replace(b"*", b"\\*")
                    .replace(b"+", b"\\+")
                )
                assert re.match(exp_pat, recvd), (
                    f"did not match\n    {exp_pat}\nagainst\n    {recvd}"
                )

@register_test
def test_mangle_corrupted(cmd, maildir_root, **kwargs):
    dovecot_port = kwargs["imaps_port"]
    with _session(None, host="127.0.0.1", port=dovecot_port) as rw2:
        rw2.put(b"2b SELECT INBOX\r\n")
        rw2.wait_for_resp("2b", "OK")
    with inbox(cmd) as rw:
        pass

@register_test
def test_search(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        # append a message that is sure to break the imf parsing
        append_messages(rw, 1)

        # append a message with known contents
        msg = b"My-Header: My Value\r\n\r\nhello world\r\n"
        rw.put(b"1 APPEND INBOX {%d}\r\n"%len(msg))
        rw.wait_for_match(b"\\+")
        rw.put(msg + b"\r\n")
        rw.wait_for_resp("1", "OK")

        # Get that UID
        uid = int(get_uid("*", rw))

        rw.put(b"2 UID SEARCH HEADER My-Header \"My Value\"\r\n")
        rw.wait_for_resp("2", "OK", require=[b"\\* SEARCH %d"%uid])

@register_test
def test_fetch_envelope(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        # append a message with known contents
        msg = (
            b"Return-Path: <no-reply@junkdomain.com>\r\n"
            b"Delivered-To: unknown\r\n"
            b"Received: from localhost; 4 July 1776 00:00:00 -0000\r\n"
            b"Delivered-To: junk@junkdomain.com\r\n"
            b"Received: 	(using TLSv1.2 with cipher ROT13)\r\n"
            b"	(No client certificate requested)\r\n"
            b"	by junkdomain.com (Postfix) with ESMTPS id AAAAAAAAAA\r\n"
            b"	for <junk@junkdomain.com>; Sat, 4 July 1776 00:00:00 -0000 (GMT)\r\n"
            b"Authentication-Results: junkdomain.com;\r\n"
            b"	dkim=pass (1024-bit key; unprotected)\r\n"
            b"	header.d=list.junkdomain.com header.i=@list.junkdomain.com\r\n"
            b"	header.b=\"AAAAAAAA\";\r\n"
            b"	dkim-atps=neutral\r\n"
            b"DKIM-Signature: v=1; d=list.junkdomain.com; s=x;\r\n"
            b"	h=Date:Message-Id:Reply-To:From:MIME-VersionSubject:To; bh=AAAAAAAA;\r\n"
            b"	b=AAAAAAAA;\r\n"
            b"Received: from root by list.junkdomain.com with local (Exim 4.80)\r\n"
            b"	(envelope-from <no-reply@junkdomain.com>)\r\n"
            b"	id AAAAAA-AAAAAA-AA\r\n"
            b"	for junk@junkdomain.com; Sat, 4 July 1776 00:00:00 -0000\r\n"
            b"To: junk@junkdomain.com\r\n"
            b"Subject: Hi there, this is a junk message!\r\n"
            b"Mime-Version: 1.0\r\n"
            b"From: Junk Name <junk@junkdomain.com>\r\n"
            b"Reply-To: junkreplyto@junkdomain.com\r\n"
            b"Sender: Different Junk Name <notjunk@junkdomain.com>\r\n"
            b"Message-Id: <ABCDEFG-HIJKLM-NO@list.junkdomain.com>\r\n"
            b"Date: Sat, 4 July 1776 00:00:00 -0000\r\n"
            b"\r\n"
            b"msg.\r\n"
        )
        rw.put(b"1 APPEND INBOX {%d}\r\n"%len(msg))
        rw.wait_for_match(b"\\+")
        rw.put(msg + b"\r\n")
        rw.wait_for_resp("1", "OK")

        # get that UID
        uid = int(get_uid("*", rw))

        rw.put(b"2 UID FETCH %d (ENVELOPE)\r\n"%uid)
        env_date = b"4 July 1776 00:00:00 -0000"
        env_subj = b"Hi there, this is a junk message!"
        rw.wait_for_resp("2", "OK", require=[
            b"\\* [0-9]+ FETCH \\(UID %d ENVELOPE \\("%uid
            + b"\"Sat, 4 July 1776 00:00:00 -0000\" "
            + b"\"Hi there, this is a junk message!\" "
            + b"\\(\"Junk Name\" NIL \"junk\" \"junkdomain.com\"\\) "
            + b"\\(\"Different Junk Name\" NIL \"notjunk\" \"junkdomain.com\"\\) "
            + b"\\(NIL NIL \"junkreplyto\" \"junkdomain.com\"\\) "
            + b"\\(NIL NIL \"junk\" \"junkdomain.com\"\\) "
            + b"NIL "
            + b"NIL "
            + b"NIL "
            + b"\"<ABCDEFG-HIJKLM-NO@list.junkdomain.com>\""
            + b"\\)\\)"
        ])


@register_test
def test_fetch_body(cmd, maildir_root, **kwargs):
    with inbox(cmd) as rw:
        # append a message with known contents
        msg = (
            b"Mime-Version: 1.0\r\n"
            b"Content-Type: multipart/mixed; boundary=\"root-boundary\"\r\n"
            b"Subject: root message\r\n"
            b"\r\n"
            b"--root-boundary\r\n"
            # part 1: text/plain
            b"\r\n"
            b"Root Body, part 1.\r\n"
            b"\r\n"
            b"--root-boundary\r\n"
            # part 2: a message/rfc822 with a text/plain body
            b"Content-Type: message/rfc822\r\n"
            b"\r\n"
            b"Subject: sub message\r\n"
            b"\r\n"
            # part 2.1 (via the special dereference case): text/plain
            b"Sub body.\r\n"
            b"\r\n"
            b"--root-boundary\r\n"
            # part 3: a message/rfc822 with a multipart body
            b"Content-Type: message/rfc822\r\n"
            b"\r\n"
            b"Subject: sub message\r\n"
            b"Content-Type: multipart/mixed; boundary=\"sub-boundary\"\r\n"
            b"\r\n"
            b"--sub-boundary\r\n"
            # part 3.1: text/plain
            b"\r\n"
            b"Sub-Sub Body 1.\r\n"
            b"\r\n"
            b"--sub-boundary\r\n"
            # part 3.2: text/plain
            b"\r\n"
            b"Sub-Sub Body 2.\r\n"
            b"\r\n"
            b"--sub-boundary--\r\n"
            b"\r\n"
            b"--root-boundary--\r\n"
        )
        rw.put(b"1 APPEND INBOX {%d}\r\n"%len(msg))
        rw.wait_for_match(b"\\+")
        rw.put(msg + b"\r\n")
        rw.wait_for_resp("1", "OK")

        # get that UID
        uid = int(get_uid("*", rw))

        # first, request a few invalid body requests
        for i, req in enumerate([b"0", b"4", b"2.2", b"2.1.1", b"1.1"]):
            rw.put(b"invalid%d UID FETCH %d BODY[%s]\r\n"%(i, uid,req))
            rw.wait_for_resp("invalid%d"%i, "NO")

        # requesting BODY[MIME] is not allowed by imap for some reason
        rw.put(b"2 UID FETCH %d BODY[MIME]\r\n"%uid)
        rw.wait_for_resp("2", "BAD")

        # request the BODY for the message
        rw.put(b"3 UID FETCH %d BODY\r\n"%uid)
        rw.wait_for_resp("3", "OK")

        # request the BODYSTRUCTURE for the message
        rw.put(b"4 UID FETCH %d BODYSTRUCTURE\r\n"%uid)
        rw.wait_for_resp("4", "OK")

        # broken mime: missing boundary parameter
        msg = (
            b"Mime-Version: 1.0\r\n"
            b"Content-Type: multipart/mixed\r\n"
            b"\r\n"
            b"---\r\n"
            b"\r\n"
            b"hi!\n"
            b"\r\n"
            b"-----\r\n"
        )
        rw.put(b"5 APPEND INBOX {%d}\r\n"%len(msg))
        rw.wait_for_match(b"\\+")
        rw.put(msg + b"\r\n")
        rw.wait_for_resp("5", "OK")
        uid = int(get_uid("*", rw))

        # BODY will show the one defaulted part
        rw.put(b"6 UID FETCH %d BODY\r\n"%uid)
        rw.wait_for_resp("6", "OK", require=[
            b'.*BODY \\(\\("text" "plain" \\("charset" "us-ascii"\\) NIL NIL '
            b'"7BIT" 0 0\\) "mixed"\\).*',
        ])

        # the one defaulted part is fetchable
        rw.put(b"7 UID FETCH %d BODY[1]\r\n"%uid)
        rw.wait_for_resp("7", "OK", require=[b'.*BODY\\[1\\] ""\\).*'])

        # broken mime again: this time, zero body parts
        msg = (
            b"Mime-Version: 1.0\r\n"
            b"Content-Type: multipart/mixed; boundary=\"-\"\r\n"
            b"\r\n"
            b"some prelude, but no boundary.\r\n"
        )
        rw.put(b"7 APPEND INBOX {%d}\r\n"%len(msg))
        rw.wait_for_match(b"\\+")
        rw.put(msg + b"\r\n")
        rw.wait_for_resp("7", "OK")
        uid = int(get_uid("*", rw))

        rw.put(b"8 UID FETCH %d BODY\r\n"%uid)
        rw.wait_for_resp("8", "OK", require=[
            b'.*BODY \\(\\("text" "plain" \\("charset" "us-ascii"\\) NIL NIL '
            b'"7BIT" 0 0\\) "mixed"\\).*',
        ])

        rw.put(b"9 UID FETCH %d BODY[1]\r\n"%uid)
        rw.wait_for_resp("9", "OK", require=[b'.*BODY\\[1\\] ""\\).*'])


@register_test
def test_uid_mode_fetch_responses(cmd, maildir_root, **kwargs):
    # UID FETCH/STORE/SEARCH/COPY should cause FETCH responses to report UID
    with Subproc(cmd) as subproc, \
            _inbox(subproc) as rw1, \
            _inbox(subproc) as rw2:
        # append four messages, but delete the first two, to ensure that we
        # have two messages where uid1, uid2, seqnum1, seqnum2 are all unique
        append_messages(rw1, 4)
        u2 = int(get_uid("*", rw1))
        u1 = u2 - 1
        rw1.put(b"1 uid store %d,%d flags \\Deleted\r\n"%(u1-2, u1-1))
        rw1.wait_for_resp("1", "OK")
        rw1.put(b"2a expunge\r\n")
        rw1.wait_for_resp("2a", "OK")
        n2 = int(get_seq_num(u2, rw1))
        n1 = n2 - 1
        assert len(set((u1, u2, n1, n2))) == 4, set((u1, u2, n1, n2))

        # rw2 needs to catch up on what messages exist in order to modify them
        rw2.put(b"3 noop\r\n")
        rw2.wait_for_resp("3", "OK")

        # plain FETCH -> no UID
        rw2.put(b"4b STORE %d +FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("4b", "OK")
        rw1.put(b"4a FETCH %d FLAGS\r\n"%(n2))
        rw1.wait_for_resp(
            "4a",
            "OK",
            disallow=[b'.*UID.*'],
            require=[
                b"\\* %d FETCH.*"%n2,
                b"\\* %d FETCH.*"%n1,
            ]
        )

        # UID FETCH -> yes UID
        rw2.put(b"5b STORE %d -FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("5b", "OK")
        rw1.put(b"5a UID FETCH %d FLAGS\r\n"%(u2))
        rw1.wait_for_resp(
            "5a",
            "OK",
            require=[
                b"\\* %d FETCH.*UID %d"%(n2,u2),
                b"\\* %d FETCH.*UID %d"%(n1,u1),
            ]
        )

        # plain STORE -> no UID
        rw2.put(b"6b STORE %d +FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("6b", "OK")
        rw1.put(b"6a STORE %d FLAGS \\Answered\r\n"%(n2))
        rw1.wait_for_resp(
            "6a",
            "OK",
            disallow=[b'.*UID.*'],
            require=[
                b"\\* %d FETCH.*"%n1,
                b"\\* %d FETCH.*"%n2,
            ]
        )

        # UID STORE -> yes UID
        rw2.put(b"7b STORE %d -FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("7b", "OK")
        rw1.put(b"7a UID STORE %d -FLAGS \\Answered\r\n"%(u2))
        rw1.wait_for_resp(
            "7a",
            "OK",
            require=[
                b"\\* %d FETCH.*UID %d"%(n1,u1),
                b"\\* %d FETCH.*UID %d"%(n2,u2),
            ]
        )

        # plain SEARCH -> no UID
        rw2.put(b"8b STORE %d +FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("8b", "OK")
        rw1.put(b"8a SEARCH all\r\n")
        rw1.wait_for_resp(
            "8a",
            "OK",
            disallow=[b'.*UID.*'],
            require=[
                b"\\* %d FETCH.*"%n1,
            ]
        )

        # UID SEARCH -> yes UID
        rw2.put(b"9b STORE %d -FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("9b", "OK")
        rw1.put(b"9a UID SEARCH all\n")
        rw1.wait_for_resp(
            "9a",
            "OK",
            require=[
                b"\\* %d FETCH.*UID %d"%(n1,u1),
            ]
        )

        # plain COPY -> no UID
        rw2.put(b"10b STORE %d +FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("10b", "OK")
        rw1.put(b"10a COPY %d INBOX\r\n"%n1)
        rw1.wait_for_resp(
            "10a",
            "OK",
            disallow=[b'.*UID.*'],
            require=[
                b"\\* %d FETCH.*"%n1,
            ]
        )

        # UID COPY -> yes UID
        rw2.put(b"11b STORE %d -FLAGS \\Answered\r\n"%(n1))
        rw2.wait_for_resp("11b", "OK")
        rw1.put(b"11a UID COPY %d INBOX\r\n"%u1)
        rw1.wait_for_resp(
            "11a",
            "OK",
            require=[
                b"\\* %d FETCH.*UID %d"%(n1,u1),
            ]
        )


@register_test
def test_combine_updates_into_fetch(cmd, maildir_root, **kwargs):
    with Subproc(cmd) as subproc, \
            _inbox(subproc) as rw1, \
            _inbox(subproc) as rw2:
        append_messages(rw1, 2)
        u2 = int(get_uid("*", rw1))
        u1 = u2 - 1

        # rw2 should not be able to fetch these messages yet (regression test)
        rw2.put(b"1 UID FETCH %d:%d BODY[]\r\n"%(u1, u2))
        rw2.wait_for_resp(
            "1",
            "OK",
            require=[b".*EXISTS.*"],
            disallow=[b"\\* [0-9]+ FETCH.*"]
        )

        # rw1 updates the flag on the first message, then even deletes it
        rw1.put(b"2 UID STORE %d FLAGS \\Deleted\r\n"%u1)
        rw1.wait_for_resp("2", "OK")
        rw1.put(b"3 EXPUNGE\r\n")
        rw1.wait_for_resp("3", "OK")

        # rw2 fetches bodies of both messages
        # order of responses should be:
        #  - FETCH of u1, with FLAGS
        #  - FETCH of u2, no FLAGS
        #  - EXPUNGE
        rw2.put(b"4 UID FETCH %d:%d INTERNALDATE\r\n"%(u1, u2))
        rw2.wait_for_resp(
            "4",
            "OK",
            require=[
                b"\\* [0-9]+ FETCH \\(FLAGS \\(\\\\Deleted\\) UID %d"%u1,
                b"\\* [0-9]+ FETCH \\(UID %d"%u2,
                b"\\* %d EXPUNGE"%u1,
            ],
        )

@register_test
def test_status_with_local_info(cmd, maildir_root, **kwargs):
    # We modify the upstream server's STATUS response in the following ways:
    #  - RECENT is always 0
    #  - MESSAGES and UNSEEN counts are are decreased for NOT4ME messages
    #  - MESSAGES and UNSEEN counts are are increased for uid_locals messages

    # not4me generated with:
    #   echo hi | ./encrypt_msg path/to/test/files/key_tool/key_n.pem
    not4me = (
        b"-----BEGIN SPLINTERMAIL MESSAGE-----\r\n"
        b"VjoxClI6MzI62ZxVYmKUJjz42+VMAoZnP2ZtmslptYVv94VYOUNl82A6MTI4Oo04\r\n"
        b"9bULnsibwCzTinZE9XFJqyt9OnnsQQZWXrMhvhXakgWoNAqYzKFaztyN8cntAz3g\r\n"
        b"CX6WS+MuCAyxjNd9ifkK9xlzx62WxVxUZ0TdEBYfk/gKyWtUeNSiso+2T7Kd/YuM\r\n"
        b"8ZgjzzPNa4Lcbwk+Pjdv6yc24PH3cZPmAXJj5rhECklWOjEyOmaAnAA+EtHS9ZbJ\r\n"
        b"YwpNOu+zQA==\r\n"
        b"=6x4fUZQpPFNgU1sKDUgB4g==\r\n"
        b"-----END SPLINTERMAIL MESSAGE-----\r\n"
    )

    def read_status(line, val):
        m = re.search(b"%s ([0-9]+)"%as_bytes(val), line)
        assert m, f"no {val} in '{line}'"
        return int(m[1])

    # run this in a temp mailbox
    mbx = b"deleteme_" + codecs.encode(os.urandom(5), "hex_codec")

    with _session(
        None, host="127.0.0.1", port=kwargs["imaps_port"]
    ) as rwx:
        rwx.put(b"1 CREATE %s\r\n"%mbx)
        rwx.wait_for_resp("1", "OK")

        # upload a recent, unseen, NOT4ME message
        rwx.put(b"2 APPEND %s {%d}\r\n"%(mbx, len(not4me)))
        rwx.wait_for_match(b"\\+")
        rwx.put(b"%s\r\n"%not4me)
        rwx.wait_for_resp("2", "OK")
        rwx.put(b"3 STATUS %s (MESSAGES UNSEEN RECENT)\r\n"%mbx)
        _, line = rwx.wait_for_resp("3", "OK")
        nmsgs = read_status(line, "MESSAGES")
        nunseen = read_status(line, "UNSEEN")
        nrecent = read_status(line, "RECENT")
        assert nmsgs == 1, nmsgs
        assert nunseen == 1, nunsen
        assert nrecent == 1, nrecent

    with session(cmd) as rw:
        # we haven't opened the mailbox yet, so we don't know it's NOT4ME; it
        # should count for MESSAGES and UNSEEN but RECENT must always be 0
        rw.put(b"4 STATUS %s (MESSAGES UNSEEN RECENT)\r\n"%mbx)
        _, line = rw.wait_for_resp("4", "OK")
        nmsgs = read_status(line, "MESSAGES")
        nunseen = read_status(line, "UNSEEN")
        nrecent = read_status(line, "RECENT")
        assert nmsgs == 1, nmsgs
        assert nunseen == 1, nunsen
        assert nrecent == 0, nrecent

        # sync the mailbox
        rw.put(b"5 SELECT %s\r\n"%mbx)
        rw.wait_for_resp("5", "OK")
        rw.put(b"6 CLOSE\r\n")
        rw.wait_for_resp("6", "OK")

        # Now that we know it's NOT4ME, it shouldn't count for anything
        rw.put(b"7 STATUS %s (MESSAGES UNSEEN RECENT)\r\n"%mbx)
        _, line = rw.wait_for_resp("7", "OK")
        nmsgs = read_status(line, "MESSAGES")
        nunseen = read_status(line, "UNSEEN")
        nrecent = read_status(line, "RECENT")
        assert nmsgs == 0, nmsgs
        assert nunseen == 0, nunsen
        assert nrecent == 0, nrecent

        # add a pair of unseen local message
        inject_local_msg(maildir_root, mbx)

        # Now we should add to the server's messages and unseen
        rw.put(b"8 STATUS %s (MESSAGES UNSEEN RECENT)\r\n"%mbx)
        _, line = rw.wait_for_resp("8", "OK")
        nmsgs = read_status(line, "MESSAGES")
        nunseen = read_status(line, "UNSEEN")
        nrecent = read_status(line, "RECENT")
        assert nmsgs == 1, nmsgs
        assert nunseen == 1, nunsen
        assert nrecent == 0, nrecent

        # cleanup the temp mailbox
        rw.put(b"9 DELETE %s\r\n"%mbx)
        rw.wait_for_resp("9", "OK")


def append_messages(rw, count, box="INBOX"):
    for n in range(count):
        rw.put(b"PRE%d APPEND %s {11}\r\n"%(n, as_bytes(box)))
        rw.wait_for_match(b"\\+")
        rw.put(b"hello world\r\n")
        rw.wait_for_resp(b"PRE%d"%n, "OK")


# Prepare a subdirectory
@contextlib.contextmanager
def temp_maildir_root():
    tempdir = tempfile.mkdtemp()
    try:
        yield tempdir
    finally:
        shutil.rmtree(tempdir)


@contextlib.contextmanager
def dovecot_setup():
    import mariadb
    import dovecot

    pysm_path = "server/pysm"
    sys.path.append(pysm_path)
    import pysm

    migrations = os.path.join(HERE, "..", "server", "migrations")
    migmysql_path = os.path.join("server", "migmysql")
    plugin_path = os.path.join("server", "xkey")
    with mariadb.mariadb(
        migrations=migrations, migmysql_path=migmysql_path
    ) as runner:
        # inject the test user
        with pysm.SMSQL(sock=runner.sockpath) as smsql:
            smsql.create_account(USER, PASS)
        with dovecot.tempdir() as basedir:
            with dovecot.Dovecot(
                basedir=basedir,
                sql_sock=runner.sockpath,
                bind_addr="127.0.0.1",
                plugin_path=plugin_path,
            ) as imaps_port:
                print(f"dovecot ready! ({imaps_port})")
                yield imaps_port


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("patterns", nargs="*")
    parser.add_argument("--proxy")
    parser.add_argument("--extra-subproc", action="store_true")
    args = parser.parse_args()

    if args.extra_subproc:
        assert sys.platform == "win32", "--extra-subproc is only for windows"
        # In windows, we can create an extra layer of subprocess in a new
        # subprocess group to protect the calling process from our crazy
        # SIGINT behavior, as we try to exercise the shutdown behavior of the
        # citm loop.
        cmd = [sys.executable, __file__]
        if args.proxy:
            cmd += ["--proxy", args.proxy]
        cmd += args.patterns
        print("extra subproc!", cmd)
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
        creationflags |= subprocess.CREATE_NEW_CONSOLE
        with open("e2e_citm_log", "w") as f:
            p = subprocess.Popen(
                cmd, creationflags=creationflags, stdout=f, stderr=f
            )
            exit(p.wait())

    # initialize the global server_context
    cert = os.path.join(test_files, "ssl", "good-cert.pem")
    key = os.path.join(test_files, "ssl", "good-key.pem")
    _ = server_context(cert, key)

    if len(args.patterns) == 0:
        tests = all_tests
    else:
        # filter tests by patterns from command line
        tests = [
            t for t in all_tests
            if any(re.search(p, t.__name__) is not None for p in args.patterns)
        ]

        if len(tests) == 0:
            print("no tests match any patterns", file=sys.stderr)
            sys.exit(1)

    if args.proxy is not None:
        import proxy
        proxy_spec = proxy.read_spec(args.proxy)
        imaps_port = 12385
        dovecot_ctx_mgr = proxy.ProxyClient(
            proxy_spec, ("127.0.0.1", imaps_port)
        )
    else:
        dovecot_ctx_mgr = dovecot_setup()

    with dovecot_ctx_mgr as imaps_port:
        with temp_maildir_root() as maildir_root:
            kwargs = {"imaps_port": imaps_port}
            for test in tests:
                # clean up the mail before each test, but leave the keys alone
                userpath = os.path.join(maildir_root, USER)
                mailpath = os.path.join(userpath, "mail")
                shutil.rmtree(mailpath, ignore_errors=True)

                print(test.__name__ + "... ", end="", flush="true")
                cmd = [
                    "libcitm/citm",
                    "--local-host",
                    "127.0.0.1",
                    "--local-port",
                    "2993",
                    "--remote-host",
                    "127.0.0.1",
                    "--remote-port",
                    str(imaps_port),
                    "--tls-key",
                    os.path.join(test_files, "ssl", "good-key.pem"),
                    "--tls-cert",
                    os.path.join(test_files, "ssl", "good-cert.pem"),
                    "--tls-dh",
                    os.path.join(test_files, "ssl", "dh_4096.pem"),
                    "--maildirs",
                    maildir_root,
                ]
                try:
                    test(cmd, maildir_root, **kwargs)
                    print("PASS")
                except:
                    print("FAIL")
                    raise

    print("PASS")
    time.sleep(1)
