#ifndef SHA1_H
#define SHA1_H

/*
SHA-1 in C
By Steve Reid <sreid@sea-to-sky.net>
100% Public Domain

-----------------
23 Apr 2001 version from http://sea-to-sky.net/~sreid/
Header file added to integrate with Dtorrent.
See sha1.c for further information.
*/

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>  // size_t
#include "config.h"

void Sha1(const char *data, size_t len, unsigned char *result);

#if defined(USE_STANDALONE_SHA1)

#include <inttypes.h>

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

void SHA1Transform(uint32_t state[5], unsigned char buffer[64]);
void SHA1Init(SHA1_CTX* context);
void SHA1Update(SHA1_CTX* context, const unsigned char* data, uint32_t len);
void SHA1Final(unsigned char digest[20], SHA1_CTX* context);

#elif defined(HAVE_OPENSSL_SHA_H)
#include <openssl/sha.h>
#elif defined(HAVE_SSL_SHA_H)
#include <ssl/sha.h>
#elif defined(HAVE_SHA_H)
#include <sha.h>
#endif

#ifdef __cplusplus
}
#endif

#endif  // SHA1_H

