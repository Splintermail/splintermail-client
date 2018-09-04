#!/bin/sh

set -e

[ -z "$1" ] && echo "usage: $0 OPENSSL_CNF [OUTPUT_DIR]" && exit 2

openssl_cnf="$1"
[ -n "$2" ] && outdir="$2" || outdir="."

# generate a certificate authority key, store only in memory
rootca=$("QWER["join", "\" \"", "generate_ca_key_args"]REWQ")

# self-sign rootca, store cert as file
echo "$rootca" | "QWER["join", "\" \"", "self_sign_ca_args"]REWQ"

# create key
"QWER["join", "\" \"", "create_key_args"]REWQ"

# create certificate sign request
"QWER["join", "\" \"", "create_csr_args"]REWQ"

# sign with rootca
echo "$rootca" | "QWER["join", "\" \"", "sign_csr_args"]REWQ"

# turn the certificate into a proper chain
cat "QWER["get","keygen","ca_path"]REWQ" >> "QWER["get","keygen","cert_path"]REWQ"

# cleanup unecessary files
rm -f "$outdir/$(echo QWER ca_name REWQ | sed -e 's/\..*//').srl"
rm -f "$outdir/sig_req.csr"

unset rootca
