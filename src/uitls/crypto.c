/*
 * crypto.c
 *
 *  Created on: 2023-02-01 14:41:06
 *      Author: yui
 */

#include <string.h>

#include <libavutil/encryption_info.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/aes.h>
#include <libavutil/des.h>
#include <libavutil/camellia.h>
#include <libavutil/cast5.h>
#include <libavutil/base64.h>
#include <libavutil/rc4.h>

enum CryptoType {
	AES,	DES,	CAMELLIA,	RC4,	CAST5,	UNSUPPORTED=-1,
};
struct AVCryptoContext {
	enum CryptoType type;
	const uint8_t *key;
	int key_bits;
	int decrypt;
	uint8_t *iv;
	int block_size;
	union {
		struct AVAES *aes;
		struct AVCAMELLIA *camellia;
		struct AVRC4 *rc4;
		struct AVCAST5 *cast5;
		struct AVDES *des;
	};
};
struct crypto_type_entry {
	const char *name;
	enum CryptoType type;
	int block_size;
} crypto_type_dict[] = {
	{"aes",  AES,  16}, { "des", DES,  8}, {"camellia", CAMELLIA, 16}, {"rc4", RC4, 1}, {"cast5", CAST5, 8}, { NULL, UNSUPPORTED}
};
enum CryptoType get_crypto_type(const char *name) {
	for (int i = 0; i < FF_ARRAY_ELEMS(crypto_type_dict); i++) {
		if (!strcasecmp(name, crypto_type_dict[i].name))
			return crypto_type_dict[i].type;
	}
	return UNSUPPORTED;
}
int get_crypto_block_size(const char *name) {
	for (int i = 0; i < FF_ARRAY_ELEMS(crypto_type_dict); i++) {
		if (!strcasecmp(name, crypto_type_dict[i].name))
			return crypto_type_dict[i].block_size;
	}
	return AVERROR(ENOSYS);
}
static int crypto_ctx_alloc(struct AVCryptoContext *ctx) {
	switch(ctx->type) {
	case AES:
		ctx->aes = av_aes_alloc();  break;
	case DES:
		ctx->des = av_des_alloc();  break;
	case CAMELLIA:
		ctx->camellia = av_camellia_alloc();  break;
	case RC4:
		ctx->rc4 = av_rc4_alloc();  break;
	case CAST5:
		ctx->cast5 = av_cast5_alloc();  break;
	default:
		return AVERROR(ENOSYS);
	}
	if (ctx->aes == NULL && ctx->des == NULL && ctx->camellia == NULL && ctx->rc4 == NULL && ctx->cast5 == NULL)
		return AVERROR(ENOMEM);
	return 0;
}
static int crypto_ctx_init(struct AVCryptoContext *ctx) {
	switch(ctx->type) {
	case AES:
		return av_aes_init(ctx->aes, ctx->key, ctx->key_bits, ctx->decrypt);
	case DES:
		return av_des_init(ctx->des, ctx->key, ctx->key_bits, ctx->decrypt);
	case CAMELLIA:
		return av_camellia_init(ctx->camellia, ctx->key, ctx->key_bits);
	case RC4:
		return av_rc4_init(ctx->rc4, ctx->key, ctx->key_bits, ctx->decrypt);
	case CAST5:
		return av_cast5_init(ctx->cast5, ctx->key, ctx->key_bits);
	default:
		return AVERROR(ENOSYS);
	}
}
static void crypto_ctx_crypt(struct AVCryptoContext *ctx, const uint8_t *src, int count, uint8_t *dst) {
	switch (ctx->type) {
	case AES:
		av_aes_crypt(ctx->aes, dst, src, count, ctx->iv, ctx->decrypt);  break;
	case DES:
		av_des_crypt(ctx->des, dst, src, count, ctx->iv, ctx->decrypt);  break;
	case CAMELLIA:
		av_camellia_crypt(ctx->camellia, dst, src, count, ctx->iv, ctx->decrypt);  break;
	case RC4:
		av_rc4_crypt(ctx->rc4, dst, src, count, NULL, ctx->decrypt);  break;
	case CAST5:
		av_cast5_crypt2(ctx->cast5, dst, src, count, ctx->iv, ctx->decrypt);  break;
	default:
		return;
	}
}
static void crypto_ctx_free(struct AVCryptoContext *ctx) {
	av_freep(&ctx->iv);
	switch (ctx->type) {
	case AES:
		av_freep(&ctx->aes);  break;
	case DES:
		av_freep(&ctx->des);  break;
	case CAMELLIA:
		av_freep(&ctx->camellia);  break;
	case RC4:
		av_freep(&ctx->rc4);  break;
	case CAST5:
		av_freep(&ctx->cast5);  break;
	default:
		return;
	}
}
static int _av_crypt(const char *crypto_type, const uint8_t *key, int key_bits, const uint8_t *iv, const uint8_t *src, size_t len, uint8_t **out, int decrypt) {
	int ret = 0;
	struct AVCryptoContext ctx = {
		.type = get_crypto_type(crypto_type),
		.key = key,
		.key_bits = key_bits,
		.decrypt = decrypt,
		.block_size = get_crypto_block_size(crypto_type),
	};
	uint8_t *dst = NULL, *src_copy = NULL;
	int count = ceil(len / (float)ctx.block_size);
	if (iv != NULL) {
		ctx.iv = av_memdup(iv, ctx.block_size);
		if (ctx.iv == NULL) {
			ret = AVERROR(ENOMEM);
			goto end;
		}
	}
	if (ctx.type == UNSUPPORTED) {
		ret = AVERROR(ENOSYS);
		goto end;
	}
	if (src != *out) {
		src_copy = av_calloc(count, ctx.block_size);
		if (src_copy == NULL) {
			ret = AVERROR(ENOMEM);
			goto end;
		}
		memcpy(src_copy, src, len);
		if (count * ctx.block_size > len) {
			memset(src_copy + len, 0, count * ctx.block_size - len);
		}
	}
	else {
		if (ctx.iv != NULL) {
			av_log(NULL, AV_LOG_ERROR, "av_crypt(%s) error: src and *out are equivalent, but iv is not NULL:"
					" can't work on CBC mode when src and dst are the same memory.\n", decrypt ? "decryption" : "encryption");
			ret = AVERROR(EINVAL);
			goto end;
		}
		src_copy = (uint8_t *)src;
	}
	if (*out == NULL) {
		dst = av_calloc(count, ctx.block_size);
		if (dst == NULL) {
			ret = AVERROR(ENOMEM);
			goto end;
		}
	}
	else {
		dst = *out;
	}
	if ((ret =crypto_ctx_alloc(&ctx)) < 0) {
		goto err1;
	}
	if ((ret = crypto_ctx_init(&ctx))< 0)
		goto err1;
	crypto_ctx_crypt(&ctx, src_copy, count, dst);
	crypto_ctx_free(&ctx);
	if (*out == NULL)
		*out = dst;
	ret = count * ctx.block_size;
	goto end;
err1:
	if (*out == NULL)
		av_freep(&dst);
end:
	if (src != *out)
		av_freep(&src_copy);
	return ret;
}
int av_encrypt(const char *crypto_type, const uint8_t *key, int key_bits, const uint8_t *iv, const uint8_t *src, size_t len, uint8_t **out) {
	return _av_crypt(crypto_type, key, key_bits, iv, src, len, out, 0);
}
int av_decrypt(const char *crypto_type, const uint8_t *key, int key_bits, const uint8_t *iv, const uint8_t *src, size_t len, uint8_t **out) {
	return _av_crypt(crypto_type, key, key_bits, iv, src, len, out, 1);
}
