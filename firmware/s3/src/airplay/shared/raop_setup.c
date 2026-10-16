/*
 * Ported from philippe44's squeezelite-esp32 raop component
 * (c) Philippe 2019-2020, philippe_44@outlook.com -- MIT License,
 * https://opensource.org/licenses/MIT
 */

#ifndef AK_NATIVE

#include "raop_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"

static const char *TAG = "raop_auth";

// The AirPort Express private key that every AirPlay 1 sender authenticates
// receivers against; public knowledge since 2011 (shairport et al.).
static const char airport_rsa_key[] =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
    "wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
    "wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
    "UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
    "BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
    "LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
    "NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
    "lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
    "aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
    "a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
    "oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
    "oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
    "k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
    "AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
    "cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
    "17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
    "1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
    "LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
    "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----";

static mbedtls_pk_context s_pk;
// RSA operations update the context's blinding values.
static SemaphoreHandle_t s_lock;

static int rng(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

bool raop_setup_init(void) {
    if (s_lock) {
        return true;
    }
    mbedtls_pk_init(&s_pk);
    if (mbedtls_pk_parse_key(&s_pk, (const uint8_t *)airport_rsa_key, sizeof(airport_rsa_key), NULL, 0, rng, NULL) !=
        0) {
        ESP_LOGE(TAG, "cannot parse RSA key");
        mbedtls_pk_free(&s_pk);
        return false;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL;
}

size_t raop_base64_decode(const char *src, uint8_t *dst, size_t dst_size) {
    size_t len = strlen(src);
    size_t padded_len = (len + 3) & ~(size_t)3;
    char *padded = malloc(padded_len + 1);
    size_t out_len = 0;

    if (padded) {
        memcpy(padded, src, len);
        memset(padded + len, '=', padded_len - len);
        padded[padded_len] = '\0';
        if (mbedtls_base64_decode(dst, dst_size, &out_len, (const uint8_t *)padded, padded_len) != 0) {
            out_len = 0;
        }
        free(padded);
    }
    return out_len;
}

char *raop_sdp_attr(const char *body, const char *name) {
    char needle[24];
    snprintf(needle, sizeof(needle), "a=%s:", name);
    const char *p = strcasestr(body, needle);
    if (!p) {
        return NULL;
    }
    p += strlen(needle);
    return strndup(p, strcspn(p, "\r\n"));
}

char *raop_auth_challenge(const char *challenge, struct in_addr local, const uint8_t mac[6]) {
    uint8_t data[32] = {0};
    size_t n = raop_base64_decode(challenge, data, 16);
    if (n == 0) {
        return NULL;
    }
    memcpy(data + n, &local.s_addr, 4);
    memcpy(data + n + 4, mac, 6);

    // "Encrypt with the private key": a PKCS#1 v1.5 type-1 signature over
    // the raw 32 bytes, no digest.
    uint8_t sig[256];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(s_pk);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    int ret = mbedtls_rsa_pkcs1_sign(rsa, rng, NULL, MBEDTLS_MD_NONE, sizeof(data), data, sig);
    xSemaphoreGive(s_lock);
    if (ret != 0) {
        ESP_LOGE(TAG, "Apple-Challenge signing failed");
        return NULL;
    }

    char b64[352];
    size_t b64_len = 0;
    if (mbedtls_base64_encode((uint8_t *)b64, sizeof(b64), &b64_len, sig, sizeof(sig)) != 0) {
        return NULL;
    }
    while (b64_len > 0 && b64[b64_len - 1] == '=') {
        b64[--b64_len] = '\0';
    }
    return strdup(b64);
}

bool raop_auth_aes_key(const char *b64, uint8_t out[16]) {
    uint8_t encrypted[256 + 4];
    size_t len = raop_base64_decode(b64, encrypted, sizeof(encrypted));
    uint8_t plain[256];
    size_t plain_len = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(s_pk);
    bool ok = len == mbedtls_rsa_get_len(rsa);
    if (ok) {
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
        ok = mbedtls_rsa_pkcs1_decrypt(rsa, rng, NULL, &plain_len, encrypted, plain, sizeof(plain)) == 0 &&
             plain_len == 16;
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        memcpy(out, plain, 16);
    }
    return ok;
}

bool raop_parse_transport(const char *transport, uint16_t *control_port, uint16_t *timing_port) {
    const char *p;
    unsigned int cport = 0, tport = 0;
    if (!transport) {
        return false;
    }
    if ((p = strcasestr(transport, "control_port="))) {
        sscanf(p + 13, "%u", &cport);
    }
    if ((p = strcasestr(transport, "timing_port="))) {
        sscanf(p + 12, "%u", &tport);
    }
    *control_port = (uint16_t)cport;
    *timing_port = (uint16_t)tport;
    return cport && tport;
}

void raop_parse_rtp_info(const char *rtp_info, uint16_t *seqno, uint32_t *rtptime) {
    const char *p;
    unsigned long value;

    *seqno = 0;
    *rtptime = 0;
    if (rtp_info && (p = strcasestr(rtp_info, "seq=")) && sscanf(p + 4, "%lu", &value) == 1) {
        *seqno = (uint16_t)value;
    }
    if (rtp_info && (p = strcasestr(rtp_info, "rtptime=")) && sscanf(p + 8, "%lu", &value) == 1) {
        *rtptime = (uint32_t)value;
    }
}

#endif  // !AK_NATIVE
