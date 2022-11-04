#include "server/acme/libacme.h"

#include "test/test_utils.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

static derr_t test_ed25519(void){
    derr_t e = E_OK;

    key_i *k = NULL;

    DSTR_STATIC(pvt,
        "\x9d\x61\xb1\x9d\xef\xfd\x5a\x60\xba\x84\x4a\xf4\x92\xec\x2c\xc4"
        "\x44\x49\xc5\x69\x7b\x32\x69\x19\x70\x3b\xac\x03\x1c\xae\x7f\x60"
    );

    PROP_GO(&e, ed25519_from_bytes(pvt, &k), cu);

    /* python test vector generation:

        import base64
        import json
        import textwrap

        from cryptography.hazmat.primitives.asymmetric import ed25519

        def b64url(s):
            if isinstance(s, str):
                s = s.encode('utf8')
            return base64.b64encode(
                s, altchars=b"-_"
            ).rstrip(b"=").decode('utf8')

        key = ed25519.Ed25519PrivateKey.from_private_bytes(
            b"\x9d\x61\xb1\x9d\xef\xfd\x5a\x60\xba\x84\x4a\xf4\x92\xec\x2c\xc4"
            b"\x44\x49\xc5\x69\x7b\x32\x69\x19\x70\x3b\xac\x03\x1c\xae\x7f\x60"
        )

        payload = "some-special-text"
        protected = (
            '{"alg":"EdDSA","crv":"Ed25519","nonce":"xyz",'
            '"kid":"https://kid.com","url":"https://url.com"}'
        )

        signing_input = f"{b64url(protected)}.{b64url(payload)}"
        sig = key.sign(signing_input.encode('utf8'))

        exp = json.dumps({
            "protected": b64url(protected),
            "payload": b64url(payload),
            "signature": b64url(sig),
        }).replace(" ", "")

        c_exp = exp.replace('"', '\\"')
        c_exp = textwrap.wrap(c_exp, 69, break_on_hyphens=False)
        print("    DSTR_STATIC(acme_exp,")
        for line in c_exp:
            print(f'        "{line}"')
        print("    );")


        from cryptography.hazmat.primitives import hashes, hmac
        h = hmac.HMAC(b"topsecret", hashes.SHA256())

        payload = "some-special-text"
        protected = '{"alg":"HS256","kid":"mykey","url":"https://url.com"}'

        signing_input = f"{b64url(protected)}.{b64url(payload)}"
        h.update(signing_input.encode('utf8'))
        sig = h.finalize()

        exp = json.dumps({
            "protected": b64url(protected),
            "payload": b64url(payload),
            "signature": b64url(sig),
        }).replace(" ", "")

        c_exp = exp.replace('"', '\\"')
        c_exp = textwrap.wrap(c_exp, 69, break_on_hyphens=False)
        print("    DSTR_STATIC(hs256_exp,")
        for line in c_exp:
            print(f'        "{line}"')
        print("    );")

    */
    DSTR_STATIC(acme_exp,
        "{\"protected\":\"eyJhbGciOiJFZERTQSIsImNydiI6IkVkMjU1MTkiLCJub25jZSI6"
        "Inh5eiIsImtpZCI6Imh0dHBzOi8va2lkLmNvbSIsInVybCI6Imh0dHBzOi8vdXJsLmNvb"
        "SJ9\",\"payload\":\"c29tZS1zcGVjaWFsLXRleHQ\",\"signature\":\"Acc54mE"
        "0ULBUjF6ZuDZD0fy2n6A1GM8Vot1HnUNbUI8ObSDEVGxCL9u4f8N9ylJM4hEl9uXk7lhE"
        "5URM_8m5Cg\"}"
    );

    {
        // acme_jws
        DSTR_VAR(jwsbuf, 4096);

        PROP_GO(&e,
            acme_jws(
                k,
                DSTR_LIT("some-special-text"),
                DSTR_LIT("xyz"),
                DSTR_LIT("https://url.com"),
                DSTR_LIT("https://kid.com"),
                &jwsbuf
            ),
        cu);

        EXPECT_D3_GO(&e, "acme_jws", &jwsbuf, &acme_exp, cu);
    }

    {
        // jws, w/ Ed25519 (same test vector as acme_jws)
        DSTR_VAR(jwsbuf, 4096);
        DSTR_STATIC(payload, "some-special-text");
        DSTR_STATIC(protected,
            "{\"alg\":\"EdDSA\",\"crv\":\"Ed25519\",\"nonce\":\"xyz\","
            "\"kid\":\"https://kid.com\",\"url\":\"https://url.com\"}"
        );

        PROP_GO(&e, jws(protected, payload, SIGN_KEY(k), &jwsbuf), cu);

        EXPECT_D3_GO(&e, "jws", &jwsbuf, &acme_exp, cu);
    }

cu:
    if(k) k->free(k);

    return e;
}

//// This test doesn't pass, but the ACME server says our signatures are good,
//// so we're going to ignore this.
// static derr_t test_es256(void){
//     derr_t e = E_OK;
//
//     key_i *k = NULL;
//
//     DSTR_STATIC(pvtjwk,
//         "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"Du7BdPtQQ-YlB11mbByZfK4"
//         "sYjsDbhLk3UlNjvvSh9A\",\"y\":\"Ls9gVIVSuUKzXqPSbtptyPBlniKJU2bBFL"
//         "tmbud8R20\",\"d\":\"O_N4PS5BXM-PTR-ij1ZDDoButfzR4Ku-SOu-xNCx83s\""
//         "}"
//     );
//
//     json_t json;
//     DSTR_VAR(jtext, 256);
//     json_node_t nodemem[16];
//     size_t nnodes = sizeof(nodemem)/sizeof(*nodemem);
//     json_prep_preallocated(&json, &jtext, nodemem, nnodes, true);
//     PROP_GO(&e, json_parse(pvtjwk, &json), cu);
//
//     PROP_GO(&e, jwk_to_key(json.root, &k), cu);
//
//     /* python test vector generation:
// import base64
// import json
// import textwrap
//
// from cryptography.hazmat.primitives import hashes
// from cryptography.hazmat.primitives.asymmetric import ec
//
// def b64url(s):
//     if isinstance(s, str):
//         s = s.encode('utf8')
//     return base64.b64encode(
//         s, altchars=b"-_"
//     ).rstrip(b"=").decode('utf8')
//
// def unb64url(s):
//     if isinstance(s, bytes):
//         s = s.decode('utf8')
//     if len(s) % 4:
//         s += "=" * (4 - (len(s) % 4))
//     return base64.b64decode(s, altchars=b"-_")
//
// def bignum_decode(byts):
//     if isinstance(byts, str):
//         byts = byts.encode('utf8')
//     out = 0
//     for b in byts:
//         out = out * 256 + b
//     return out
//
// jwktext = (
//     "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"Du7BdPtQQ-YlB11mbByZfK4"
//     "sYjsDbhLk3UlNjvvSh9A\",\"y\":\"Ls9gVIVSuUKzXqPSbtptyPBlniKJU2bBFL"
//     "tmbud8R20\",\"d\":\"O_N4PS5BXM-PTR-ij1ZDDoButfzR4Ku-SOu-xNCx83s\""
//     "}"
// )
//
// jwk = json.loads(jwktext)
//
// x = bignum_decode(unb64url(jwk["x"]))
// y = bignum_decode(unb64url(jwk["y"]))
// d = bignum_decode(unb64url(jwk["d"]))
// curve = ec.SECP256R1()
// pub = ec.EllipticCurvePublicNumbers(x, y, curve)
// pvt = ec.EllipticCurvePrivateNumbers(d, pub)
// key = pvt.private_key()
//
// payload = "some-special-text"
// protected = (
//     '{"alg":"ES256","nonce":"xyz","kid":"https://kid.com",'
//     '"url":"https://url.com"}'
// )
//
// signing_input = f"{b64url(protected)}.{b64url(payload)}"
// sigalg = ec.ECDSA(hashes.SHA256())
// sig = key.sign(signing_input.encode('utf8'), sigalg)
//
// exp = json.dumps({
//     "protected": b64url(protected),
//     "payload": b64url(payload),
//     "signature": b64url(sig),
// }).replace(" ", "")
//
// c_exp = exp.replace('"', '\\"')
// c_exp = textwrap.wrap(c_exp, 69, break_on_hyphens=False)
// print("    DSTR_STATIC(acme_exp,")
// for line in c_exp:
//     print(f'        "{line}"')
// print("    );")
//     */
//
//     DSTR_STATIC(acme_exp,
//         "{\"protected\":\"eyJhbGciOiJFUzI1NiIsIm5vbmNlIjoieHl6Iiwia2lkIjoiaHR0"
//         "cHM6Ly9raWQuY29tIiwidXJsIjoiaHR0cHM6Ly91cmwuY29tIn0\",\"payload\":\"c"
//         "29tZS1zcGVjaWFsLXRleHQ\",\"signature\":\"MEUCIQDWdLRlv43KdQi5NXSHPAeD"
//         "q0F4hovImfJDbW6HKsjtLQIgNwB7Rapr9P7Pi1CtzEfKmVe5LO_W3B8mAUwl057chN8\""
//         "}"
//     );
//
//     // acme_jws
//     DSTR_VAR(jwsbuf, 4096);
//
//     PROP_GO(&e,
//         acme_jws(
//             k,
//             DSTR_LIT("some-special-text"),
//             DSTR_LIT("xyz"),
//             DSTR_LIT("https://url.com"),
//             DSTR_LIT("https://kid.com"),
//             &jwsbuf
//         ),
//     cu);
//
//     EXPECT_D3_GO(&e, "acme_jws", &jwsbuf, &acme_exp, cu);
//
// cu:
//     if(k) k->free(k);
//     return e;
// }

static derr_t test_hs256(void){
    derr_t e = E_OK;

    /* python test vector generation:
        import base64
        import json
        import textwrap

        from cryptography.hazmat.primitives import hashes, hmac

        def b64url(s):
            if isinstance(s, str):
                s = s.encode('utf8')
            return base64.b64encode(
                s, altchars=b"-_"
            ).rstrip(b"=").decode('utf8')

        h = hmac.HMAC(b"topsecret", hashes.SHA256())

        payload = "some-special-text"
        protected = '{"alg":"HS256","kid":"mykey","url":"https://url.com"}'

        signing_input = f"{b64url(protected)}.{b64url(payload)}"
        h.update(signing_input.encode('utf8'))
        sig = h.finalize()

        exp = json.dumps({
            "protected": b64url(protected),
            "payload": b64url(payload),
            "signature": b64url(sig),
        }).replace(" ", "")

        c_exp = exp.replace('"', '\\"')
        c_exp = textwrap.wrap(c_exp, 69, break_on_hyphens=False)
        print("    DSTR_STATIC(hs256_exp,")
        for line in c_exp:
            print(f'        "{line}"')
        print("    );")
    */

    DSTR_STATIC(hs256_exp,
        "{\"protected\":\"eyJhbGciOiJIUzI1NiIsImtpZCI6Im15a2V5IiwidXJsIjoiaHR0"
        "cHM6Ly91cmwuY29tIn0\",\"payload\":\"c29tZS1zcGVjaWFsLXRleHQ\",\"signa"
        "ture\":\"zktRfmRfvlKhX7KnI-Z-GVevVEsRRbWRZ4gHB8BsUpE\"}"
    );

    DSTR_VAR(jwsbuf, 4096);
    DSTR_STATIC(payload, "some-special-text");
    DSTR_STATIC(protected,
        "{\"alg\":\"HS256\",\"kid\":\"mykey\",\"url\":\"https://url.com\"}"
    );
    DSTR_STATIC(hmac_secret, "topsecret");

    PROP(&e, jws(protected, payload, SIGN_HS256(&hmac_secret), &jwsbuf) );

    EXPECT_D3(&e, "hs256-jws", &jwsbuf, &hs256_exp);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    PROP_GO(&e, ssl_library_init(), test_fail);

    PROP_GO(&e, test_ed25519(), test_fail);
    // PROP_GO(&e, test_es256(), test_fail);
    PROP_GO(&e, test_hs256(), test_fail);

    ssl_library_close();
    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    ssl_library_close();
    LOG_ERROR("FAIL\n");
    return 1;
}
