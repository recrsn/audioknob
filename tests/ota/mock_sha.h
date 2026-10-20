#pragma once

#include <openssl/evp.h>

typedef struct {
    EVP_MD_CTX *ctx;
} mbedtls_sha256_context;

static inline void mbedtls_sha256_init(mbedtls_sha256_context *sha) { sha->ctx = NULL; }
static inline void mbedtls_sha256_free(mbedtls_sha256_context *sha) { EVP_MD_CTX_free(sha->ctx); }
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *sha, int is224) {
    (void)is224;
    sha->ctx = EVP_MD_CTX_new();
    return sha->ctx && EVP_DigestInit_ex(sha->ctx, EVP_sha256(), NULL) == 1 ? 0 : -1;
}
static inline int mbedtls_sha256_update(mbedtls_sha256_context *sha, const unsigned char *data, size_t size) {
    return EVP_DigestUpdate(sha->ctx, data, size) == 1 ? 0 : -1;
}
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *sha, unsigned char out[32]) {
    return EVP_DigestFinal_ex(sha->ctx, out, NULL) == 1 ? 0 : -1;
}
