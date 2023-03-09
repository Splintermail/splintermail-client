#!/bin/sh

set -e

[ -z "$1" ] && echo "usage: $0 OPENSSL_CNF [OUTPUT_DIR]" && exit 2

openssl_cnf="$1"
[ -n "$2" ] && outdir="$2" || outdir="."

# generate a certificate authority key, store only in memory
rootca=$("QW '" "'^generate_ca_key_args WQ")

# self-sign rootca, store cert as file
echo "$rootca" | "QW '" "'^self_sign_ca_args WQ"

# create key
"QW '" "'^create_key_args WQ"

# create certificate sign request
"QW '" "'^create_csr_args WQ"

# sign with rootca
echo "$rootca" | "QW '" "'^sign_csr_args WQ"

# turn the certificate into a proper chain
cat "QW keygen.ca_path WQ" >> "QW keygen.cert_path WQ"

# cleanup unecessary files (srl naming is unreliable across platforms)
rm -f "$outdir/$(echo QW ca_name WQ | sed -e 's/[^a-z].*//')"*.srl
rm -f "$outdir/sig_req.csr"

unset rootca
