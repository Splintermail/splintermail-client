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


def test_file_enc_dec(enc, dec, test_files):
    keyfile = os.path.join(test_files, "key_tool/key_m.pem")

    payload = base64.b64encode(os.urandom(10000))

    # sometimes, in windows, when trying to open one file from a samba share
    # simultaneously from multiple files, you can see a permissions error,
    # so rather than start decoder and pipe it to the encoder, we run them
    # in serial to make the test more robust.

    enc_proc = subprocess.run(
        [enc, keyfile], input=payload, stdout=subprocess.PIPE, check=True,
    )
    msg = enc_proc.stdout
    dec_proc = subprocess.run(
        [dec, keyfile], input=msg, stdout=subprocess.PIPE, check=True,
    )

    result = dec_proc.stdout

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
    enc_proc = subprocess.run(
        [enc, "--socket", sock],
        input=payload,
        stdout=subprocess.PIPE,
        env={"USER": pysm.to_fsid(uuid) + "@x.splintermail.com"},
        check=True,
    )

    # get the encrypted message
    msg = enc_proc.stdout

    # ensure that encrypt_msg is encrypting to all recipients in the table
    keyfiles = [
        os.path.join(test_files, "key_tool", "key_%s.pem"%x) for x in "aemn"
    ]
    for keyfile in keyfiles:
        dec_proc = subprocess.run(
            [dec, keyfile], input=msg, stdout=subprocess.PIPE, check=True
        )
        assert dec_proc.stdout == payload, (
            f"payload mismatch:\n{payload}\n  vs\n{result}"
        )


def test_server_enc_dec_nokeys(enc, smsql, sock):
    # Test again without any keys
    uuid = smsql.create_account("nokeys@splintermail.com", "passwordpassword")

    payload = base64.b64encode(os.urandom(10000))

    # encrypt the message once to all keys in the database
    enc_proc = subprocess.run(
        [enc, "--socket", sock],
        input=payload,
        stdout=subprocess.PIPE,
        env={"USER": pysm.to_fsid(uuid) + "@x.splintermail.com"},
        check=True,
    )

    # get the (un)encrypted message
    msg = enc_proc.stdout
    assert msg == payload, f"payload mismatch\n{payload}\n  vs\n{msg}"


if __name__ == "__main__":
    if len(sys.argv) not in (1, 2):
        print(
            "usage: %s [--server]"%(sys.argv[0]),
            file=sys.stderr,
        )
        sys.exit(1)

    enc = "./encrypt_msg"
    dec = "./decrypt_msg"

    if "--server" not in sys.argv:
        test_file_enc_dec(enc, dec, test_files)
    else:
        import mariadb
        sys.path.append("./server/pysm")
        import pysm
        migmysql_path = "./server/migmysql"
        with mariadb.mariadb(None, migrations, migmysql_path) as script_runner:
            sock = script_runner.sockpath
            with pysm.SMSQL(sock=sock) as smsql:
                test_server_enc_dec(enc, dec, test_files, smsql, sock)
                test_server_enc_dec_nokeys(enc, smsql, sock)

    print("PASS")
