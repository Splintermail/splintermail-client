#!/usr/bin/env python3

"""
proxy.py: a custom SOCKS-like proxy for running tests on non-linux.

server usage: python path/to/proxy.py PROXY_SPEC
(must be run from a build directory with -DBUILD_SERVER_CODE=yes)

client usage: ./proxy.py PROXY_SPEC TUNNEL_SPEC

(each SPEC is a [HOST:]PORT)
"""

import base64
import sys
import threading
import argparse

import trio
from trio import socket

# message types:
# C -> S:
#   - d: start dovecot
#   - xd: stop dovecot
#   - cd,ID: connect dovecot
#   - t,ID,B64: base64-encoded bytes on connection
#
# S -> C:
#   - s,ID: SHUT_WR (we can't read anymore, conn might be open)
#   - x,ID: connection closed or failed
#   - t,ID,B64: base64-encoded bytes on connection


### Helper code


class MessageReader:
    """
    Read one line at a time from the master connection.
    """
    def __init__(self, conn):
        self.conn = conn
        self.buf = b""

    async def recv(self):
        # wait for a complete line
        while b'\n' not in self.buf:
            try:
                data = await self.conn.recv(4096)
            except Exception as e:
                print(e)
                data = None
            if not data:
                # EOF
                return None
            self.buf += data
        # check for a complete line
        idx = self.buf.index(b'\n')
        line = self.buf[:idx].decode('utf8')
        self.buf = self.buf[idx + 1:]
        return line.rstrip('\r\n').split(',')


def handle_conn(conn, cid, send_w, poppable=None):
    """
    Handle the read side and the write side of a socket until completion.
    """
    rw_cancel = trio.CancelScope()
    chan_w, chan_r = trio.open_memory_channel(10)

    async def _read_fn():
        while True:
            try:
                msg = await conn.recv(4096)
            except Exception as e:
                print(e)
                msg = None
            if not msg:
                await send_w.send(f"x,{cid}")
                rw_cancel.cancel()
                break
            await send_w.send(b"t,%d,%s"%(cid, base64.b64encode(msg)))

    async def _write_fn():
        while True:
            msg = await chan_r.receive()
            if msg is None:
                await send_w.send(f"x,{cid}")
                rw_cancel.cancel()
                break
            while msg:
                try:
                    n = await conn.send(msg)
                    msg = msg[n:]
                except Exception as e:
                    print(e)
                    await send_w.send(f"x,{cid}")
                    rw_cancel.cancel()
                    return

    async def _fn():
        # print("handling conn", cid)
        with rw_cancel:
            try:
                async with trio.open_nursery() as nursery:
                    nursery.start_soon(_read_fn)
                    nursery.start_soon(_write_fn)
            finally:
                # print("closing conn", cid)
                conn.close()
                chan_r.close()
                if poppable is not None:
                    poppable.pop(cid, None)
                # only pass 'x' when we weren't canceled
                if not rw_cancel.cancel_called:
                    await send_w.send(f"x,{cid}")

    return _fn, chan_w, rw_cancel


async def send_from_channel(conn, send_r, cancel_scope):
    """
    Use only one sender on a connection, and feed that sender packets over a
    channel.  This guarantees that packets are never overlapping.
    """
    try:
        async with send_r:
            while True:
                # print("awaiting packet")
                msg = await send_r.receive()
                if isinstance(msg, str):
                    msg = msg.encode('utf8')
                msg = msg.rstrip(b'\n') + b'\n'
                # print("have packet", msg)
                while msg:
                    # print("sending packet", msg)
                    n = await conn.send(msg)
                    # print("sent packet!", msg)
                    msg = msg[n:]
    finally:
        if cancel_scope:
            cancel_scope.cancel()


### Client code


class ProxyClient(threading.Thread):
    """
    Run a trio-based client on a background thread.
    """
    def __init__(self, proxy_spec, tunnel_spec):
        self.proxy_spec = proxy_spec
        self.tunnel_spec = tunnel_spec
        self.cond = threading.Condition()
        self.cancel_scope = None
        self.exited = False
        super().__init__()

        self.trio_token = None
        self.dovecot_ready = False

        # maps cids to (chan_w, rw_cancel)
        self.conns = {}
        self.cid = 0

    async def run_proxy(self):
        """
        Listen for local connections that we proxy to dovecot.
        """
        with socket.socket() as l:
            if hasattr(socket, "SO_REUSEADDR"):
                l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            await l.bind(self.tunnel_spec)
            l.listen()
            while True:
                conn, addr = await l.accept()
                cid = self.cid
                self.cid += 1

                fn, chan_w, rw_cancel = handle_conn(conn, cid, self.send_w, self.conns)
                self.conns[cid] = (chan_w, rw_cancel)
                await self.send_w.send(f"cd,{cid}")

                #self.nursery.start_soon(fn, lambda: self.conns.pop(cid, None))
                self.nursery.start_soon(fn)

    ## (CMDS) Since in our test we only open dovecot once, we can safely turn
    ## on dovecot automatically and leave it on for the lifetime of the thread.
    ## Some day if we want to change that we have to tools right here.
    # async def run_cmds(self, port, cmds_r):
    #     """
    #     Listen for local connections that will control when dovecot is open.
    #     """
    #     with socket.socket() as l:
    #         if hasattr(socket, "SO_REUSEADDR"):
    #             l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    #         await l.bind(("127.0.0.1", port))
    #         l.listen()
    #         while True:
    #             conn, addr = await l.accept()
    #             with conn:
    #                 # start dovecot
    #                 await self.send_w.send("d")
    #                 # wait for startup to finish
    #                 ok = await cmds_r.receive()
    #                 if ok:
    #                     # tell the client it worked
    #                     await conn.send(b"+\n")
    #                     # wait for the client to disconnect
    #                     _ = await conn.recv(1)
    #                     # shut dovecot back down
    #                     await self.send_w.send("xd")
    #                     # wait for the server to tell us it shut down
    #                     _ = await cmds_r.receive()

    async def recv_from_server(self, conn): #, cmds_w):  # (CMDS)
        """
        Listen on the master connection to the server.
        """
        reader = MessageReader(conn)
        while True:
            msg = await reader.recv()
            if msg is None:
                raise ValueError("broken master connection")
            # if msg[0] != "t":
            #     print(msg)
            if msg[0] == "x":
                cid = int(msg[1])
                if cid not in self.conns:
                    continue
                _, rw_cancel = self.conns[cid]
                rw_cancel.cancel()
            elif msg[0] == "t":
                cid = int(msg[1])
                data = base64.b64decode(msg[2])
                if cid not in self.conns:
                    await self.send_w.send(f"x,{cid}")
                    continue
                chan_w, _ = self.conns[cid]
                try:
                    await chan_w.send(data)
                except Exception as e:
                    print(e)
                    await self.send_w.send(f"x,{cid}")
            elif msg[0] == "d":
                # (CMDS) dovecot ready
                with self.cond:
                    self.dovecot_ready = True
                    self.cond.notify_all()
                # (CMDS)
                # await cmds_w.send(True)
            elif msg[0] == "xd":
                print("dovecot died!")
                # (CMDS)
                # await cmds_w.send(False)

    async def _run(self):
        # open the master connection to the proxy server
        with socket.socket() as conn:
            await conn.connect(self.proxy_spec)

            # channel for sending to master
            self.send_w, send_r = trio.open_memory_channel(10)
            ## channel for commands (CMDS)
            #cmds_w, cmds_r = trio.open_memory_channel(10)

            # enable off-thread access
            self.trio_token = trio.lowlevel.current_trio_token()

            # everything is cancelable from off-thread
            with trio.CancelScope() as self.cancel_scope:
                ## initial setup complete (CMDS)
                # with self.cond:
                #     self.cond.notify_all()

                async with trio.open_nursery() as self.nursery:
                    self.nursery.start_soon(
                        send_from_channel, conn, send_r, self.cancel_scope
                    )

                    # Automatically hold dovecot open for now (CMDS).
                    # self.nursery.start_soon(self.run_cmds, 4108, cmds_r)
                    await self.send_w.send("d")

                    self.nursery.start_soon(self.run_proxy)

                    await self.recv_from_server(conn) # , cmds_w) # (CMDS)

    def run(self):
        try:
            trio.run(self._run)
        finally:
            self.exited = True
            with self.cond:
                self.cond.notify_all()

    def start(self):
        # kick-off the thread
        super().start()
        # wait for the thread to finish setup, or to fail out
        with self.cond:
            # while self.cancel_scope is None and not self.exited:
            while self.dovecot_ready is False and not self.exited:
                self.cond.wait()
        if self.exited:
            raise ValueError("ProxyClient failed")
        # return just the imaps_port to be e2e_citm.dovecot_setup()-compatible
        return self.tunnel_spec[1]

    def close(self):
        if self.cancel_scope is not None:
            trio.from_thread.run_sync(
                self.cancel_scope.cancel, trio_token=self.trio_token
            )
            self.join()

    def __enter__(self):
        return self.start()

    def __exit__(self, *arg):
        self.close()


async def aproxy(host, port):
    with socket.socket() as conn:
        await conn.connect((host, port))
        while True:
            conn, addr = await listener.accept()
            with conn:
                await run_one_test(conn)


### Server code

class DovecotMgr:
    def __init__(self, send_w):
        self.child = None
        self.send_w = send_w
        self.port = None

    async def __aenter__(self):
        # don't start child until start message arrives
        return self

    async def __aexit__(self, *_):
        await self.aclose()

    async def astart(self):
        import e2e_citm
        assert self.child is None, "can't start two dovecots"

        try:
            child = e2e_citm.dovecot_setup()
            self.port = child.__enter__()
            self.child = child
            await self.send_w.send("d")
        except Exception as e:
            print(e)
            await self.send_w.send("xd")

    async def aclose(self):
        if self.child is not None:
            self.child.__exit__(None, None, None)
            self.child = None
        await self.send_w.send("xd")

    async def connect(self):
        if self.child is None:
            return None

        conn = socket.socket()
        try:
            await conn.connect(("localhost", self.port))
        except Exception as e:
            print(e)
            conn.close()
            return None
        return conn


async def run_one_test(conn):
    print("test beginning")
    reader = MessageReader(conn)
    # open a channel for queueing outbound messages
    # That way, multiple senders will never write over each other
    send_w, send_r = trio.open_memory_channel(10)

    with trio.CancelScope() as cancel_scope:
        async with trio.open_nursery() as nursery, DovecotMgr(send_w) as d:
            nursery.start_soon(send_from_channel, conn, send_r, cancel_scope)
            # maps cid to {conn, rw_cancel}
            conns = {}

            while True:
                # recieve a message
                msg = await reader.recv()
                if msg is None:
                    print("end of test")
                    cancel_scope.cancel()
                    break

                if msg[0] != "t":
                    print(msg)

                if msg[0] == "d": # start dovecot
                    await d.astart()
                elif msg[0] == "xd": # stop dovecot
                    await d.aclose()
                elif msg[0] == "cd": # connect to dovecot
                    cid = int(msg[1])
                    c = await d.connect()
                    if c is None:
                        await send_w.send(f"x,{cid}")
                        continue
                    # start this connection in a separate task
                    fn, chan_w, rw_cancel = handle_conn(c, cid, send_w, conns)
                    conns[cid] = (chan_w, rw_cancel)
                    nursery.start_soon(fn)

                elif msg[0] == "x":  # close a connection
                    cid = int(msg[1])
                    if cid not in conns:
                        continue
                    _, rw_cancel = conns[cid]
                    rw_cancel.cancel()

                elif msg[0] == "t":  # text for a connection
                    cid = int(msg[1])
                    data = base64.b64decode(msg[2])
                    if cid not in conns:
                        await send_w.send(f"x,{cid}")
                        continue
                    chan_w, _ = conns[cid]
                    await chan_w.send(data)


async def aserver(proxy_spec):
    with socket.socket() as listener:
        if hasattr(socket, "SO_REUSEADDR"):
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        await listener.bind(proxy_spec)
        listener.listen()
        while True:
            conn, addr = await listener.accept()
            with conn:
                await run_one_test(conn)


def read_spec(spec):
    colon = spec.rfind(":")
    if colon == -1:
        host = ""
        port = int(spec)
    else:
        host = spec[:colon]
        port = int(spec[colon+1:])
    return host, port


if __name__ == "__main__":
    try:
        if len(sys.argv) < 2:
            print("not enough args!", file=sys.stderr)
            print(__doc__, file=sys.stderr)
            exit(1)
        if len(sys.argv) == 2:
            # server mode
            proxy_spec = read_spec(sys.argv[1])
            trio.run(aserver, proxy_spec)
        else:
            # client mode
            proxy_spec = read_spec(sys.argv[1])
            tunnel_spec = read_spec(sys.argv[2])
            client = ProxyClient(proxy_spec, tunnel_spec)
            with client:
                print("listening at", tunnel_spec)
                client.join()
    except KeyboardInterrupt:
        pass
