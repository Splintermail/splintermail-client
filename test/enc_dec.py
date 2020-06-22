#!/usr/bin/env python3

import subprocess
import os
import base64
import sys
import select
import threading
import traceback
import codecs


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


def test_server_enc_dec(enc, dec, test_files, script_runner):

    def readkey(keyfile):
        with open(os.path.join(test_files, "key_tool", keyfile)) as f:
            return f.read()

    privs = [readkey("key_%s.pem"%x) for x in "aemn"]

    pubs = [readkey("key_%s.pub"%x) for x in "aemn"]

    def hexify(bstr):
        return codecs.encode(bstr,'hex').decode('utf8')

    fprs = [hexify(pysm.hash_key(k)) for k in pubs]

    # populate the database with the keys we want
    device_vals = [
        "(0, \"%s\", \"%s\")"%(fpr, pub) for fpr, pub in zip(fprs, pubs)
    ]
    sql = """
        delete from accounts;
        insert into accounts (user_id, email, domain_id, password)
        values (0, "test@splintermail.com", 0, "password");

        delete from devices;

        insert into devices (user_id, fingerprint, public_key)
        values %s;
    """%(", ".join(device_vals))
    script_runner.run(sql, "splintermail")

    payload = base64.b64encode(os.urandom(10000))

    # encrypt the message once to all keys in the database
    enc_proc = subprocess.Popen(
        [enc, "--debug-sock", script_runner.sockpath],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        env={"USER": "test@splintermail.com"},
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
        assert payload == result, f"payload mismatch:\n{payload}\n  vs\n{result}"


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(
            "usage: %s /path/to/test/files [--server]"%(sys.argv[0]),
            file=sys.stderr,
        )
        sys.exit(1)

    test_files = sys.argv[1]

    enc = "./encrypt_msg"
    dec = "./decrypt_msg"

    test_file_enc_dec(enc, dec, test_files)

    if "--server" in sys.argv:
        import mariadb
        # TODO: fix this
        pysm_path = "./server"
        sys.path.append(pysm_path)
        import pysm
        with mariadb.mariadb("./mariadb") as script_runner:
            test_server_enc_dec(enc, dec, test_files, script_runner)

    print("PASS")
