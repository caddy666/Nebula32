/* aesgcm-minimal.c
 *
 * Copyright (C) 2006-2026 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

#ifndef WOLFSSL_USER_SETTINGS
    #include <wolfssl/options.h>
#endif
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>

#include <stdio.h>
#include <string.h>

#define KEY_SZ   AES_256_KEY_SIZE
#define NONCE_SZ GCM_NONCE_MID_SZ
#define TAG_SZ   AES_BLOCK_SIZE

static int GenerateKeyAndIv(byte* key, byte* iv)
{
    WC_RNG rng;
    int ret;

    ret = wc_InitRng(&rng);
    if (ret != 0) {
        return ret;
    }

    ret = wc_RNG_GenerateBlock(&rng, key, KEY_SZ);
    if (ret == 0) {
        ret = wc_RNG_GenerateBlock(&rng, iv, NONCE_SZ);
    }
    wc_FreeRng(&rng);
    return ret;
}

static int Encrypt(const byte* key, const byte* iv, const byte* plaintext,
    word32 plaintextSz, byte* ciphertext, byte* tag, const byte* aad,
    word32 aadSz)
{
    int ret;
    Aes aes;

    ret = wc_AesInit(&aes, NULL, INVALID_DEVID);
    if (ret == 0) {
        ret = wc_AesGcmSetKey(&aes, key, KEY_SZ);
    }
    if (ret == 0) {
        ret = wc_AesGcmEncrypt(&aes, ciphertext, plaintext, plaintextSz, iv,
            NONCE_SZ, tag, TAG_SZ, aad, aadSz);
    }
    wc_AesFree(&aes);
    return ret;
}

static int Decrypt(const byte* key, const byte* iv, const byte* ciphertext,
    word32 ciphertextSz, byte* plaintext, const byte* tag, const byte* aad,
    word32 aadSz)
{
    int ret;
    Aes aes;

    ret = wc_AesInit(&aes, NULL, INVALID_DEVID);
    if (ret == 0) {
        ret = wc_AesGcmSetKey(&aes, key, KEY_SZ);
    }
    if (ret == 0) {
        ret = wc_AesGcmDecrypt(&aes, plaintext, ciphertext, ciphertextSz, iv,
            NONCE_SZ, tag, TAG_SZ, aad, aadSz);
    }
    wc_AesFree(&aes);
    return ret;
}

void print_hex(const char* label, const byte* data, word32 sz)
{
    word32 i;
    printf("%s: ", label);
    for (i = 0; i < sz; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

int main(void)
{
    /* Setup/example data and buffers. */
    byte key[KEY_SZ];
    const byte aad[] = "example-aad";
    const byte plaintext[] = "single block msg";
    byte decrypted[sizeof(plaintext)];
    byte iv[NONCE_SZ];
    byte ciphertext[sizeof(plaintext)];
    byte tag[TAG_SZ];
    int ret;

    ret = GenerateKeyAndIv(key, iv);
    if (ret != 0) {
        printf("Key/IV generation failed: %d\n", ret);
        return 1;
    }
    print_hex("Plaintext", plaintext, sizeof(plaintext));
    print_hex("  Key", key, sizeof(key));
    print_hex("  IV", iv, sizeof(iv));

    /* Encrypt with key + IV (+ optional AAD) to get tag. */
    ret = Encrypt(key, iv, plaintext, sizeof(plaintext), ciphertext, tag, aad,
        (word32)(sizeof(aad) - 1));
    if (ret != 0) {
        printf("Encryption failed: %d\n", ret);
        return 1;
    }
    print_hex("Ciphertext", ciphertext, sizeof(ciphertext));
    print_hex("  Auth Tag", tag, sizeof(tag));

    /* Decrypt with the same key/IV/AAD and received tag. */
    ret = Decrypt(key, iv, ciphertext, sizeof(ciphertext), decrypted, tag, aad,
        (word32)(sizeof(aad) - 1));
    if (ret != 0) {
        printf("Decryption failed: %d\n", ret);
        return 1;
    }

    if (memcmp(plaintext, decrypted, sizeof(plaintext)) != 0) {
        printf("Round-trip mismatch\n");
        return 1;
    }
    print_hex("Decrypted", decrypted, sizeof(decrypted));

    return 0;
}
