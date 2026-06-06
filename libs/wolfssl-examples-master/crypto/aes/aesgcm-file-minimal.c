/* aesgcm-file-minimal.c
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
#include <stdlib.h>
#include <string.h>

#define KEY_SZ   AES_256_KEY_SIZE
#define NONCE_SZ GCM_NONCE_MID_SZ
#define TAG_SZ   AES_BLOCK_SIZE

static int Encrypt(const byte* key, const byte* iv, const byte* in,
    word32 inSz, byte* out, byte* tag)
{
    Aes aes;
    int ret;
    ret = wc_AesInit(&aes, NULL, INVALID_DEVID);
    if (ret == 0) {
        ret = wc_AesGcmSetKey(&aes, key, KEY_SZ);
    }
    if (ret == 0) {
        ret = wc_AesGcmEncrypt(&aes, out, in, inSz, iv, NONCE_SZ,
            tag, TAG_SZ, NULL, 0);
    }
    wc_AesFree(&aes);
    return ret;
}

static int Decrypt(const byte* key, const byte* iv, const byte* in,
    word32 inSz, byte* out, const byte* tag)
{
    Aes aes;
    int ret;
    ret = wc_AesInit(&aes, NULL, INVALID_DEVID);
    if (ret == 0) {
        ret = wc_AesGcmSetKey(&aes, key, KEY_SZ);
    }
    if (ret == 0) {
        ret = wc_AesGcmDecrypt(&aes, out, in, inSz, iv, NONCE_SZ,
            tag, TAG_SZ, NULL, 0);
    }
    wc_AesFree(&aes);
    return ret;
}

static int ReadFile(const char* path, byte** data, word32* sz)
{
    FILE* file;
    long fileSz;
    byte* buf;

    *data = NULL;
    *sz = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    fileSz = ftell(file);
    if (fileSz < 0) {
        fclose(file);
        return -1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    buf = (byte*)malloc((size_t)fileSz);
    if (buf == NULL && fileSz != 0) {
        fclose(file);
        return -1;
    }
    if (fileSz != 0 && fread(buf, 1, (size_t)fileSz, file) != (size_t)fileSz) {
        free(buf);
        fclose(file);
        return -1;
    }
    fclose(file);
    *data = buf;
    *sz = fileSz;
    return 0;
}

static int WriteFile(const char* path, const byte* data, long dataSz)
{
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    if (dataSz != 0 &&
        fwrite(data, 1, (size_t)dataSz, file) != (size_t)dataSz) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static void print_hex(const char* label, const byte* data, word32 sz)
{
    word32 i;
    printf("%s: ", label);
    for (i = 0; i < sz; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

int main(int argc, char** argv)
{
    byte key[KEY_SZ], iv[NONCE_SZ], tag[TAG_SZ];
    byte* plaintext = NULL;
    byte* ciphertext = NULL;
    byte* decrypted = NULL;
    word32 plaintextSz = 0;
    WC_RNG rng;
    int ret;

    if (argc != 4) {
        printf("Usage: %s <input-file> <encrypted-file> <decrypted-file>\n",
            argv[0]);
        return 1;
    }

    if (ReadFile(argv[1], &plaintext, &plaintextSz) != 0) {
        printf("Failed to read: %s\n", argv[1]);
        return 1;
    }

    ciphertext = (byte*)malloc((size_t)plaintextSz);
    decrypted = (byte*)malloc((size_t)plaintextSz);
    if ((ciphertext == NULL || decrypted == NULL) && plaintextSz != 0) {
        printf("alloc failed\n");
        ret = 1;
        goto exit;
    }

    ret = wc_InitRng(&rng);
    if (ret == 0) {
        ret = wc_RNG_GenerateBlock(&rng, key, KEY_SZ);
        if (ret == 0) {
            ret = wc_RNG_GenerateBlock(&rng, iv, NONCE_SZ);
        }
        wc_FreeRng(&rng);
    }
    if (ret != 0) {
        printf("Key/IV generation failed: %d\n", ret);
        goto exit;
    }

    ret = Encrypt(key, iv, plaintext, plaintextSz, ciphertext, tag);
    if (ret != 0) {
        printf("Encryption failed: %d\n", ret);
        goto exit;
    }

    if (WriteFile(argv[2], ciphertext, plaintextSz) != 0) {
        printf("Failed to write: %s\n", argv[2]);
        ret = 1;
        goto exit;
    }

    ret = Decrypt(key, iv, ciphertext, plaintextSz, decrypted, tag);
    if (ret != 0) {
        printf("Decryption failed: %d\n", ret);
        goto exit;
    }

    if (WriteFile(argv[3], decrypted, plaintextSz) != 0) {
        printf("Failed to write: %s\n", argv[3]);
        ret = 1;
        goto exit;
    }

    print_hex("Key", key, KEY_SZ);
    print_hex("IV", iv, NONCE_SZ);
    print_hex("Tag", tag, TAG_SZ);
    printf("Encrypted %u bytes to %s\n", plaintextSz, argv[2]);
    printf("Decrypted %u bytes to %s\n", plaintextSz, argv[3]);

    if (memcmp(plaintext, decrypted, (size_t)plaintextSz) == 0) {
        printf("Round-trip OK: decrypted output matches original input\n");
    }
    else {
        printf("Round-trip FAILED: mismatch\n");
        ret = 1;
    }

exit:
    free(decrypted);
    free(ciphertext);
    free(plaintext);
    return ret;
}
