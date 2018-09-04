#ifndef FIXED_LENGTHS_H
#define FIXED_LENGTHS_H

// server enforces 10 devices per account or less
#define FL_DEVICES 32

// length of key fingerprints (sha256, in binary format)
#define FL_FINGERPRINT 32

// email address is limited to 100 characters
#define FL_USERNAME 128
// password is limited to 72 characters
#define FL_PASSWORD 128

// encryption block size (AES256)
#define FL_ENCRYPTION_BLOCK_SIZE 16

// chunk size used by pop client
#define POP_CLIENT_CHUNK_SIZE 4096

#endif // FIXED_LENGTHS_H
