#!/usr/bin/env python3

import subprocess
import os
import base64
import sys
import select
import threading
import traceback
import codecs

parent_path = os.path.dirname(__file__)
test_files = os.path.join(parent_path, "files")
migrations = os.path.join(parent_path, "..", "server", "migrations")

class ReaderThread(threading.Thread):
    def __init__(self, io):
        self.io = io
        self.error = None
        super().__init__()

    def run(self):
        try:
            self.data = self.io.read()
        except Exception:
            self.error = traceback.format_exc()

    def get_data(self):
        self.join()
        if self.error is not None:
            print(self.error, file=sys.stderr)
            raise ValueError("error on ReaderThread")
        return self.data


def test_file_enc_dec(enc, dec, test_files):
    keyfile = os.path.join(test_files, "key_tool/key_m.pem")

    payload = base64.b64encode(os.urandom(10000))

    dec_proc = subprocess.Popen(
        [dec, keyfile], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    )

    enc_proc = subprocess.Popen(
        [enc, keyfile], stdin=subprocess.PIPE, stdout=dec_proc.stdin,
    )

    # let go of our file descriptor for dec_proc.stdin
    dec_proc.stdin.close()

    # start reading from the end of the pipe
    reader = ReaderThread(dec_proc.stdout)
    reader.start()

    # write into the start of the pipe
    enc_proc.stdin.write(payload)
    enc_proc.stdin.close()
    ret = enc_proc.wait()
    assert ret == 0, f"encrypt_msg exited {ret}"

    ret = dec_proc.wait()
    assert ret == 0, f"decrypt_msg exited {ret}"

    result = reader.get_data()
    assert payload == result, f"payload mismatch:\n{payload}\n  vs\n{result}"


def test_server_enc_dec(enc, dec, test_files, smsql, sock):

    def readkey(keyfile):
        with open(os.path.join(test_files, "key_tool", keyfile)) as f:
            return f.read()

    # create a temporary account
    uuid = smsql.create_account("test@splintermail.com", "passwordpassword")

    # populate some public keys
    privs = [readkey("key_%s.pem"%x) for x in "aemn"]
    pubs = [readkey("key_%s.pub"%x) for x in "aemn"]
    fprs = [smsql.add_device(uuid, pub) for pub in pubs]

    payload = base64.b64encode(os.urandom(10000))

    # encrypt the message once to all keys in the database
    enc_proc = subprocess.Popen(
        [enc, "--socket", sock],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        env={"USER": pysm.to_fsid(uuid) + "@x.splintermail.com"},
    )

    reader = ReaderThread(enc_proc.stdout)
    reader.start()

    enc_proc.stdin.write(payload)
    enc_proc.stdin.close()

    ret = enc_proc.wait()
    assert ret == 0, f"encrypt_msg exited {ret}"

    # get the encrypted message
    msg = reader.get_data()

    # ensure that encrypt_msg is encrypting to all recipients in the table
    keyfiles = [
        os.path.join(test_files, "key_tool", "key_%s.pem"%x) for x in "aemn"
    ]
    for keyfile in keyfiles:
        dec_proc = subprocess.Popen(
            [dec, keyfile], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        )

        reader = ReaderThread(dec_proc.stdout)
        reader.start()

        dec_proc.stdin.write(msg)
        dec_proc.stdin.close()

        ret = dec_proc.wait()
        assert ret == 0, f"decrypt_msg exited {ret}"

        result = reader.get_data()
        assert payload == result, (
            f"payload mismatch:\n{payload}\n  vs\n{result}"
        )


def test_server_enc_dec_nokeys(enc, smsql, sock):
    # Test again without any keys
    uuid = smsql.create_account("nokeys@splintermail.com", "passwordpassword")

    payload = base64.b64encode(os.urandom(10000))

    # encrypt the message once to all keys in the database
    enc_proc = subprocess.Popen(
        [enc, "--socket", sock],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        env={"USER": pysm.to_fsid(uuid) + "@x.splintermail.com"},
    )

    reader = ReaderThread(enc_proc.stdout)
    reader.start()

    enc_proc.stdin.write(payload)
    enc_proc.stdin.close()

    ret = enc_proc.wait()
    assert ret == 0, f"encrypt_msg exited {ret}"

    # get the (un)encrypted message
    msg = reader.get_data()
    assert msg == payload


if __name__ == "__main__":
    if len(sys.argv) not in (1, 2):
        print(
            "usage: %s [--server]"%(sys.argv[0]),
            file=sys.stderr,
        )
        sys.exit(1)

    enc = "./encrypt_msg"
    dec = "./decrypt_msg"

    test_file_enc_dec(enc, dec, test_files)

    if "--server" in sys.argv:
        import mariadb
        # TODO: fix this
        pysm_path = "./server/pysm"
        sys.path.append(pysm_path)
        import pysm
        migmysql_path = "./server/migmysql"
        with mariadb.mariadb(None, migrations, migmysql_path) as script_runner:
            sock = script_runner.sockpath
            with pysm.SMSQL(sock=sock) as smsql:
                test_server_enc_dec(enc, dec, test_files, smsql, sock)
                test_server_enc_dec_nokeys(enc, smsql, sock)

    print("PASS")
