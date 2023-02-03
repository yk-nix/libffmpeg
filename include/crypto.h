/*
 * crypto.h
 *
 *  Created on: 2023-02-01 14:41:19
 *      Author: yui
 */

#ifndef INCLUDE_CRYPTO_H_
#define INCLUDE_CRYPTO_H_

/* Supported en-cryption/de-cryption functions:
 *   1) aes      : key_bits would be 128, 192 or 256; iv(initialization vector) for CBC mode, if NULL then ECB will be used.
 *   2) des      : key_bits must be 64 or 192; iv(initialization vector) for CBC mode, if NULL then ECB will be used, must be 8-byte aligned.
 *   3) rc4      : key_bits must be a multiple of 8; iv(initialization vector) is not (yet) used for RC4, should be NULL.
 *   4) camellia : key_bits possible are 128, 192, 256; iv(initialization vector) for CBC mode, if NULL then ECB will be used.
 *   5) cast5    : key_bits possible are 40,48,...,128; iv(initialization vector) for CBC mode, if NULL then ECB will be used.
 *   note that, the iv has the same bytes as block-size.(aes:16,  des:8,  camellia:16,  rc4:1,  cast5:8)
 * encrypt 'src' with encryption function specified by 'crypto_type', if '*out' is NULL, a newly allocated output will
 * be returned, and the caller is responsible to free it via 'av_freep'. '*out' and 'src' may point to the same memory,
 * it means rewrite encrypted data at the same memory of the source data; but when 'iv' is not NULL(CBC mode), '*out' and 'src' must be different.
 */
int av_encrypt(const char *crypto_type, const uint8_t *key, int key_bits, const uint8_t *iv, const uint8_t *src, size_t len, uint8_t **out);
/*
 * Do the de-cryption operation as same as 'av_encrypt'.
 */
int av_decrypt(const char *crypto_type, const uint8_t *key, int key_bits, const uint8_t *iv, const uint8_t *src, size_t len, uint8_t **out);

#endif /* INCLUDE_CRYPTO_H_ */
