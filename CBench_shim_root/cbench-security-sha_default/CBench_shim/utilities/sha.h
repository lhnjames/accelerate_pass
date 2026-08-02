#ifndef SHA_H
#define SHA_H

/* NIST Secure Hash Algorithm */
/* heavily modified from Peter C. Gutmann's implementation */

/* Useful defines & typedefs */

typedef unsigned char BYTE;
/* MUST be exactly 32 bits wide. The original cBench source says
   `unsigned long`, which was 32-bit on the ILP32 machines it was written
   for but is 64-bit under LP64 (aarch64/x86-64), and every part of this
   SHA implementation silently breaks at that width:
     - ROT32(x,n) = (x << n) | (x >> (32 - n)) stops being a rotate;
     - SHA_INFO.data becomes 128 bytes while SHA_BLOCKSIZE stays 64, so
       sha_final()'s memset clears only through byte 56 and bytes 56..63 of
       the block fed to sha_transform() are uninitialised stack -- which is
       why the reference digest differed on EVERY run;
     - byte_reverse() swaps 4 bytes but strides 8, so it byte-swaps half of
       each word and skips the rest, and runs 8 iterations instead of 16;
     - sha_final() writes the bit count to data[14]/data[15], i.e. byte
       offsets 112/120, far outside the 64-byte block that gets hashed.
   `unsigned int` is 32-bit under both ILP32 and LP64, restoring the
   algorithm's intended semantics. */
typedef unsigned int LONG;

#define SHA_BLOCKSIZE		64
#define SHA_DIGESTSIZE		20

typedef struct {
    LONG digest[5];		/* message digest */
    LONG count_lo, count_hi;	/* 64-bit bit count */
    LONG data[16];		/* SHA data buffer */
} SHA_INFO;

void sha_init(SHA_INFO *);
void sha_update(SHA_INFO *, BYTE *, int);
void sha_final(SHA_INFO *);

void sha_stream(SHA_INFO *, FILE *);
void sha_print(SHA_INFO *);

#endif /* SHA_H */
