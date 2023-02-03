/*
 * hash.h
 *
 *  Created on: 2023-02-01 14:48:32
 *      Author: yui
 */

#ifndef INCLUDE_HASH_H_
#define INCLUDE_HASH_H_

/* Supported hash functions:
 *  MD5, murmur3, RIPEMD128, RIPEMD160, RIPEMD256, RIPEMD320, SHA160,
 *  SHA224, SHA256, SHA512/224, SHA512/256, SHA384, SHA512, CRC32, adler32
 */
#define MD5_VALUE_SIZE           16
#define MURMUR3_VALUE_SIZE       16
#define RIPEMD128_VALUE_SIZE     16
#define RIPEMD160_VALUE_SIZE     20
#define RIPEMD256_VALUE_SIZE     32
#define RIPEMD320_VALUE_SIZE     40
#define SHA160_VALUE_SIZE        20
#define SHA224_VALUE_SIZE        28
#define SHA256_VALUE_SIZE        32
#define SHA512_224_VALUE_SIZE    28
#define SHA512_256_VALUE_SIZE    32
#define SHA384_VALUE_SIZE        48
#define SHA512_VALUE_SIZE        64
#define CRC32_VALUE_SIZE         4
#define ADLER32_VALUE_SIZE       4
/*
 * Calculate hash value of the file identified by 'url'.  Return length of the hash value.
 * If '*out' is NULL, the hash value will be returned in the newly allocated memory pointed by '*out',
 * and the caller must free it via 'av_freep' when it is not used any more.
 * Return a negative error code if anything wrong.
 */
extern int av_hash_file(const char *hash_type, const char *url, uint8_t **out);
/*
 * Calculate hash value of the 'msg'.  Return length of the hash value.
 * If '*out' is NULL, the hash value will be returned in the newly allocated memory pointed by '*out',
 * and the caller must free it via 'av_freep' when it is not used any more.
 * Return a negative value if anything wrong.
 */
extern int av_hash_msg(const char *hash_type, const char *msg, size_t len, uint8_t **out);

#endif /* INCLUDE_HASH_H_ */
