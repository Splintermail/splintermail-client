#include <sys/types.h>

#include <openssl/rsa.h>

#define CIPHER_TYPE EVP_aes_256_gcm()
#define CIPHER_IV_LEN 12
#define CIPHER_BLOCK_SIZE 16
#define CIPHER_TAG_LEN 16

#define MAX_ENCRYPTER_PUBKEYS 128

// decryption failed due to missing key
extern derr_type_t E_NOT4ME;

// keypair_t is really a shared pointer
typedef struct {
    EVP_PKEY* pair;
    const dstr_t *fingerprint;
    link_t link;
} keypair_t;
DEF_CONTAINER_OF(keypair_t, link, link_t);

typedef struct {
    EVP_CIPHER_CTX* ctx;
    char pre64_buffer[1024];
    dstr_t pre64;
    size_t block_size;
    // keys to encrypt with
    link_t keys;  // keypair_t->link
    // an array-list copy of the key pointers
    EVP_PKEY* pkeys[MAX_ENCRYPTER_PUBKEYS];
    size_t nkeys;
} encrypter_t;

typedef struct {
    EVP_CIPHER_CTX* ctx;
    // pointers for recording recipients
    LIST(dstr_t)* recips;
    dstr_t* recips_block;
    bool header_found;
    bool footer_found;
    bool version_found;
    bool key_found;
    bool iv_found;
    bool message_started;
    bool tag_found;
    dstr_t enc_key;
    char iv_buffer[CIPHER_IV_LEN];
    dstr_t iv;
    const keypair_t *kp;
    // stuff that hasn't been decoded yet
    char base64_buffer[2048];
    dstr_t base64;
    // stuff that has been decoded but not parsed yet
    char buffer_buffer[2048];
    dstr_t buffer;
    // a place to put the GCM authentication tag
    char tag_buffer[CIPHER_TAG_LEN];
    dstr_t tag;
} decrypter_t;

/* not necessary in the same application where networking.h's ssl_library_int()
   is called if they both use openssl backend */
derr_t crypto_library_init(void);
void crypto_library_close(void);

derr_t gen_key(int bits, const char* keyfile);
/* throws: E_SSL (anything with the SSL library)
           E_NOMEM
           E_OPEN (failed to open file for writing) */

// keypair_load needs a matching keypair_free afterwards
derr_t keypair_load(keypair_t **out, const char *keyfile);
/* throws: E_SSL (anything with the SSL library)
           E_NOMEM
           E_OPEN (failed to open file for reading) */

derr_t keypair_from_pem(keypair_t **out, const dstr_t *pem);
void keypair_free(keypair_t **old);
derr_t keypair_get_public_pem(keypair_t *kp, dstr_t *out);
/* throws: E_NOMEM (either internally with the memory BIO or writing to *out)
           E_FIXEDSIZE (writing to *out)
           E_INTERNAL */

derr_t keypair_copy(const keypair_t *old, keypair_t **out);

derr_t encrypter_new(encrypter_t* ec);
void encrypter_free(encrypter_t* ec);
// encrypter_start will make a copy of the keys (via keypair_copy)
derr_t encrypter_start(encrypter_t* ec, link_t *keys, dstr_t* out);
derr_t encrypter_update(encrypter_t* ec, dstr_t* in, dstr_t* out);
derr_t encrypter_finish(encrypter_t* ec, dstr_t* out);

derr_t decrypter_new(decrypter_t* dc);
/* throws: E_INTERNAL
           E_NOMEM */

void decrypter_free(decrypter_t* dc);

/* don't free kp until after decrypter_finish(), it is not used until
   the encrypted key is seen from *in during decrypter_update().  Also, *recips
   and *recips_block can be NULL if you don't want them */
derr_t decrypter_start(decrypter_t* dc, const keypair_t* kp,
        LIST(dstr_t)* recips, dstr_t* recips_block);
/* throws: E_NOMEM */

/* the most this can possibly output is (in.len + block_size).  Also, the most
   it will leave behind is one line of text, which should be 64 bytes unless
   the message is broken. */
derr_t decrypter_update(decrypter_t* dc, dstr_t* in, dstr_t* out);
/* throws: E_PARAM (message was not parsable)
           E_SSL (message parsable but not decryptable)
           E_INTERNAL
           E_NOT4ME
           E_FIXEDSIZE (writing to *out)
           E_NOMEM     (writing to *out) */

derr_t decrypter_finish(decrypter_t* dc, dstr_t* out);
/* throws: E_PARAM (message hadn't started)
           E_SSL (message parsable but not decryptable)
           E_INTERNAL
           E_NOT4ME
           E_FIXEDSIZE (writing to *out)
           E_NOMEM     (writing to *out) */

derr_t hmac(const dstr_t* secret, const dstr_t* payload, dstr_t* hmac);
/* throws: E_PARAM (secret too long)
           E_NOMEM     (writing to *hmac)
           E_FIXEDSIZE (writing to *hmac)
           E_INTERNAL */

derr_t random_bytes(dstr_t* out, size_t nbytes);

/* Example of our custom PEM-like format:
   (note that everything in betwen the PEM-like header/footer is base64 encoded
   and numbers inside the base64 are just ascii, but octets are raw.  Also note
   the second base64 chunk is separated from the first by a "\n=" sequence)

-----BEGIN SPLINTERMAIL MESSAGE-----
// begin block of base64 encoding
V:1 // version = 1, meaning SHA256 fingerprint and AES-256 GCM encryption
R:128:<128 hash octets>:256:<256 key octets> // each recipient line has two
R:128:<128 hash octets>:256:<256 key octets> // blocks of binary data, the hash
R:128:<128 hash octets>:256:<256 key octets> // of this recipient's public key
R:128:<128 hash octets>:256:<256 key octets> // followed by the publickey-
R:128:<128 hash octets>:256:<256 key octets> // encrypted symmetric key
IV:32:<32 initialization vector octets> // IV for the CBC-mode message
M:<message octets until the end of base64 data> // the encrypted message
// end block of base64 encoding
=<base64 encoded GCM authentication tag, note the '=' prefix>
-----END SPLINTERMAIL MESSAGE-----

   This was chosen over existing standards such as PKCS7 because:
     - PKCS7 encodes a bunch of extra data (key issuer, key serial number)
     - But PKCS7 does not seem to encode the hash of the key anywhere
     - the openssl PKCS7 implementation compares recipient keys on those data,
         and I don't want to make shit up on key generation. Just use the hash.
     - PKCS7 does not allow one-pass writing, due to having to write the
         message size before the message itself), even though the encryption
         it uses can easily be done one block at a time.  This could have
         potentially expensive results on the server due to the large variable
         RAM requirement upon recieving emails with a large attachment, meaning
         that the RAM of SMTP processes cannot be as carefully planned (ie, you
         need to pay much more for more RAM in the VPS).  I have not yet
         verified that large attachments do not ballon the RAM usage of another
         process bulit-into Postfix, but at least my milter won't need that.
   */


/* RAND_stats before make RSA
   see https://wiki.openssl.org/index.php/Random_Numbers#Initialization
     that says rand_poll() should work to seed the PRNG even on Windows
     but `man 3 RAND_add` says:
         "On systems that provide "/dev/urandom", the randomness device is
         used to seed the PRNG transparently. However, on all other systems,
         the application is responsible for seeding the PRNG by calling
         RAND_add()"
     but Thomas Pornin says that docs are wrong and its always automatic:
         "https://security.stackexchange.com/a/56471"

   */

// event-based updates for a list of keys
struct key_listener_i;
typedef struct key_listener_i key_listener_i;

struct key_listener_i {
    void (*add)(key_listener_i*, keypair_t*);
    void (*del)(key_listener_i*, const dstr_t *fingerprint);
    link_t link;  // keyshare_t->listeners
};
DEF_CONTAINER_OF(key_listener_i, link, link_t);

typedef struct {
    link_t keys; // shared_keypair_t->link
    link_t listeners; // key_listener_i->link
} keyshare_t;

derr_t keyshare_init(keyshare_t *keyshare);

// all listeners must already be unregistered
void keyshare_free(keyshare_t *keyshare);

derr_t keyshare_add_key(keyshare_t *keyshare, const keypair_t *kp);
void keyshare_del_key(keyshare_t *keyshare, const dstr_t *fingerprint);

derr_t keyshare_register(keyshare_t *keyshare, key_listener_i *key_listener,
        link_t *initial_keys);
void keyshare_unregister(keyshare_t *keyshare, key_listener_i *key_listener);
