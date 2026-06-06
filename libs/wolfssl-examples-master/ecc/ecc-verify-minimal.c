/* ecc-verify-minimal.c
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
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>

#include <wolfssl/wolfcrypt/ecc.h>

#include <stdio.h>
#include <string.h>

static const byte signedMessageSha256Hash[32] = {
    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5
};

static const byte ecdsaSignatureDer[72] = {
    0x30, 0x46, 0x02, 0x21, 0x00, 0xd9, 0x8a, 0xe0, 0xea, 0xcf, 0xae, 0x30,
    0x70, 0xd1, 0x9c, 0xdd, 0x0d, 0xd0, 0x77, 0x73, 0x77, 0x87, 0xb6, 0x2a,
    0x99, 0xe5, 0x41, 0x91, 0x29, 0xda, 0xff, 0x5e, 0x95, 0xe8, 0xec, 0xf9,
    0x9a, 0x02, 0x21, 0x00, 0xa6, 0x71, 0x98, 0xed, 0x9b, 0x82, 0xa0, 0xba,
    0x60, 0xc5, 0xd2, 0x56, 0xae, 0x68, 0xb2, 0xd3, 0x29, 0x56, 0x88, 0xe8,
    0x47, 0xd5, 0xf1, 0x91, 0x0b, 0xac, 0xe4, 0xe9, 0x00, 0xf8, 0x31, 0x4c
};

/* X9.63 uncompressed public key (0x04 || X || Y) */
static const byte signerPublicKey[65] = {
    0x04, 0xf6, 0x7f, 0x27, 0xc2, 0xa3, 0xeb, 0x3b, 0x4f, 0xc9, 0xec, 0xdb,
    0x64, 0x72, 0xe7, 0x16, 0x51, 0xc3, 0xfb, 0xdd, 0x5c, 0xe0, 0x82, 0xc6,
    0x0c, 0x9e, 0x62, 0x6a, 0x34, 0xfc, 0x47, 0xcb, 0xe6, 0x1a, 0x08, 0x7c,
    0x44, 0x54, 0x88, 0x69, 0xb4, 0x6f, 0x5d, 0x93, 0xde, 0xdc, 0x4e, 0x7f,
    0x1a, 0xb7, 0x75, 0xe2, 0xfd, 0x5f, 0xd6, 0x7d, 0x6a, 0xd3, 0x00, 0x2c,
    0x09, 0x99, 0xaf, 0x2f, 0x0f
};

int main(void)
{
    int ret, is_valid_sig = 0;
    ecc_key key;

    /* Initialize the ecc_key structure before use. */
    ret = wc_ecc_init(&key);
    if (ret != 0) {
        printf("wc_ecc_init failed: %d\n", ret);
        return ret;
    }

    /* Load the signer's public key in X9.63 uncompressed form. */
    if (ret == 0) {
        ret = wc_ecc_import_x963_ex(signerPublicKey, sizeof(signerPublicKey),
                                    &key, ECC_SECP256R1);
        if (ret != 0) {
            printf("wc_ecc_import_x963_ex failed: %d\n", ret);
        }
    }

    /* Reject short digests to guard against any short-hash bypass. */
    if (ret == 0 && sizeof(signedMessageSha256Hash) < WC_MIN_DIGEST_SIZE) {
        printf("hash too short: %lu bytes (minimum %d)\n",
               sizeof(signedMessageSha256Hash), WC_MIN_DIGEST_SIZE);
        ret = -1;
    }

    /* Verify the ECDSA signature over the precomputed hash. */
    if (ret == 0) {
        ret = wc_ecc_verify_hash(ecdsaSignatureDer, sizeof(ecdsaSignatureDer),
                                 signedMessageSha256Hash, sizeof(signedMessageSha256Hash),
                                 &is_valid_sig, &key);
        printf("wc_ecc_verify_hash: ret=%d, is_valid_sig=%d\n", ret, is_valid_sig);

        /* A clean call with an invalid signature must still exit non-zero. */
        if (ret == 0 && is_valid_sig == 0) {
            ret = -1;
        }
    }

    wc_ecc_free(&key);
    return ret;
}
