#!/usr/bin/env python3

from subprocess import Popen, PIPE
from sys import stderr, argv, exit
import os
import getopt

# gen rootca key, save crt and return the key (stored only in memory)
def gen_ca(filename, o_val, cn_val):
    global sslcmd, conf, outdir, stdin_name

    args = [*sslcmd, 'genrsa', '2048']
    h = Popen(args, stdout=PIPE)
    key = h.stdout.read()
    if h.wait() != 0:
        raise ValueError("openssl command failed: " + ''.join(args))

    args = [*sslcmd, 'req', '-x509', '-new', '-nodes', '-sha512',
            '-days', '3650', '-config', conf, '-key', stdin_name,
            '-out', os.path.join(outdir, filename),
            '-subj', '/C=US/O=%s/OU=Org/CN=%s'%(o_val, cn_val)]
    h = Popen(args, stdin=PIPE)
    h.stdin.write(key)
    h.stdin.close()
    if h.wait() != 0:
        raise ValueError("openssl command failed: " + ''.join(args))

    return key

def signed_key(cakey, cafile, basename, days, o_val, cn_val):
    global sslcmd, conf, outdir, stdin_name
    caname = os.path.join(outdir, cafile)
    keyname = os.path.join(outdir, basename + '-key.pem')
    csrname = os.path.join(outdir, basename + '.csr')
    certname = os.path.join(outdir, basename + '-cert.pem')
    srlname = os.path.join(outdir, cafile + '.srl')

    # generate key
    args = [*sslcmd, 'genrsa', '-out', keyname, '2048']
    h = Popen(args)
    if h.wait() != 0:
        raise ValueError("openssl command failed: " + ''.join(args))

    # generate cert sign request
    args = [*sslcmd, 'req', '-new', '-config', conf, '-key', keyname,
            '-out', csrname, '-subj', '/C=US/O=%s/OU=Org/CN=%s'%(o_val, cn_val)]
    h = Popen(args)
    if h.wait() != 0:
        raise ValueError("openssl command failed: " + ''.join(args))

    # sign with CA key
    args = [*sslcmd, 'x509', '-req', '-days', str(days), '-sha512',
            '-extensions', 'req_ext', '-CAserial', srlname, '-CAcreateserial',
            '-extfile', conf, '-in', csrname, '-CA', caname,
            '-CAkey', stdin_name, '-out', certname]
    h = Popen(args, stdin=PIPE)
    h.stdin.write(cakey)
    h.stdin.close()
    if h.wait() != 0:
        raise ValueError("openssl command failed: " + ''.join(args))

    # append the CA cert to the SSL cert to create a cert chain
    with open(caname, "r") as c:
        with open(certname, "a") as cc:
            cc.write(c.read())

    os.remove(csrname)
    os.remove(srlname)

def print_usage():
    helpstr = '''
    usage: gentestkeys.py [OPTIONS] [-- OPENSSL_COMMAND]

        where OPTIONS is a combination of:

        -h --help   print this text
        -o DIR      specify output directory
                    Default: "."
        -c FILE     openssl.cnf file
                    Default: "../../../support_files/openssl.cnf"
        -s NAME     specify name of stdin
                    Default: "-" (works in Linux and Windows)
                    (set to "/dev/stdin" for Mac)'''
    print(helpstr, file=stderr)

if __name__ == '__main__':

    try:
        opts, args = getopt.gnu_getopt(argv, 'ho:c:s:', ['help'])
    except getopt.GetoptError as e:
        print(e, file=stderr)
        print_usage()
        exit(1)

    # check for help options
    if '-h' in [o[0] for o in opts] or '--help' in [o[0] for o in opts]:
        print_usage()
        exit(0)

    confopt = [o[1] for o in opts if o[0] == '-c']
    conf = '../../../support_files/openssl.cnf' if len(confopt) == 0 else confopt[-1]

    outopt = [o[1] for o in opts if o[0] == '-o']
    outdir = '.' if len(outopt) == 0 else outopt[-1]

    stdinopt = [o[1] for o in opts if o[0] == '-s']
    stdin_name = '-' if len(stdinopt) == 0 else stdinopt[-1]

    # get custom openssl command if necessary
    if '--' in argv:
        idx = argv.index('--') + 1
        sslcmd = argv[idx:]
    else:
        sslcmd = ('openssl',)

    # make sure that openssl command works
    args = [*sslcmd, 'version']
    h = Popen(args)
    if h.wait() != 0:
        print('error: openssl command `' + ' '.join(sslcmd) + '` is not working')
        exit(1)

    # create a trusted and an untrusted certificate
    good_key = gen_ca('good.crt', 'Trusted Test Local CA', 'trusted.localhost')
    bad_key = gen_ca('bad.crt', 'Untrusted Test Local CA', 'untrusted.localhost')

    # create good-signed good key
    signed_key(good_key, 'good.crt', 'good', 3650, 'Good Test Cert', '127.0.0.1')
    # create good-signed expired key
    signed_key(good_key, 'good.crt', 'expired', -1, 'Expired Test Cert', '127.0.0.1')
    # create good-signed wrong-hostname key
    signed_key(good_key, 'good.crt', 'wronghost', 3650, 'Wrong Host Test Cert', 'not.localhost')
    # create bad-signed but otherwise valid key
    signed_key(bad_key, 'bad.crt', 'unknown', 3650, 'JoeNobody Test Cert', '127.0.0.1')
